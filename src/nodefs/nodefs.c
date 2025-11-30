#include "nodefs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>

static int g_fs_fd = -1;
static Superblock g_sb;
static uint32_t g_current_node_id = 0;
static uint8_t* g_node_bitmap = NULL; // Cache for node allocation
static int update_superblock(void);

// Thread Safety
static pthread_mutex_t g_fs_lock = PTHREAD_MUTEX_INITIALIZER;

// FAT Cache - Multi-block LRU
#define FAT_CACHE_SIZE 16
#define FAT_ENTRIES_PER_BLOCK (BLOCK_SIZE / sizeof(uint32_t))

typedef struct {
    uint32_t data[FAT_ENTRIES_PER_BLOCK];
    int64_t block_idx; // -1 if empty
    int dirty;
    uint64_t last_used;
} FatCacheEntry;

static FatCacheEntry g_fat_cache[FAT_CACHE_SIZE];
static uint64_t g_lru_counter = 0;

// Helper: Current Time
static uint64_t get_current_time(void) {
    return (uint64_t)time(NULL);
}

// --- FAT Management (Unchanged mostly) ---
static void fat_init_cache(void) {
    for (int i = 0; i < FAT_CACHE_SIZE; ++i) {
        g_fat_cache[i].block_idx = -1;
        g_fat_cache[i].dirty = 0;
        g_fat_cache[i].last_used = 0;
    }
    g_lru_counter = 0;
}

static int fat_flush_slot(int slot_idx) {
    FatCacheEntry* entry = &g_fat_cache[slot_idx];
    if (entry->block_idx == -1 || !entry->dirty) return 0;

    off_t offset = g_sb.fat_ptr + (entry->block_idx * BLOCK_SIZE);
    if (lseek(g_fs_fd, offset, SEEK_SET) < 0) return -1;
    if (write(g_fs_fd, entry->data, BLOCK_SIZE) != BLOCK_SIZE) return -1;
    
    if (fsync(g_fs_fd) < 0) return -1;

    entry->dirty = 0;
    return 0;
}

static FatCacheEntry* fat_get_cache_slot(int64_t block_idx) {
    for (int i = 0; i < FAT_CACHE_SIZE; ++i) {
        if (g_fat_cache[i].block_idx == block_idx) {
            g_fat_cache[i].last_used = ++g_lru_counter;
            return &g_fat_cache[i];
        }
    }

    int lru_idx = 0;
    uint64_t min_time = g_fat_cache[0].last_used;
    for (int i = 1; i < FAT_CACHE_SIZE; ++i) {
        if (g_fat_cache[i].last_used < min_time) {
            min_time = g_fat_cache[i].last_used;
            lru_idx = i;
        }
    }

    if (fat_flush_slot(lru_idx) < 0) return NULL;

    off_t offset = g_sb.fat_ptr + (block_idx * BLOCK_SIZE);
    if (lseek(g_fs_fd, offset, SEEK_SET) < 0) return NULL;
    
    ssize_t n = read(g_fs_fd, g_fat_cache[lru_idx].data, BLOCK_SIZE);
    if (n < 0) return NULL;
    if ((size_t)n < BLOCK_SIZE) {
        memset((char*)g_fat_cache[lru_idx].data + n, 0, BLOCK_SIZE - n);
    }

    g_fat_cache[lru_idx].block_idx = block_idx;
    g_fat_cache[lru_idx].dirty = 0;
    g_fat_cache[lru_idx].last_used = ++g_lru_counter;

    return &g_fat_cache[lru_idx];
}

static int fat_flush_all(void) {
    for (int i = 0; i < FAT_CACHE_SIZE; ++i) {
        if (fat_flush_slot(i) < 0) return -1;
    }
    return 0;
}

static int fat_read_entry(uint64_t block_idx, uint32_t* entry) {
    if (block_idx >= g_sb.total_blocks) return -1;
    
    int64_t fat_block_idx = block_idx / FAT_ENTRIES_PER_BLOCK;
    uint64_t offset = block_idx % FAT_ENTRIES_PER_BLOCK;
    
    FatCacheEntry* slot = fat_get_cache_slot(fat_block_idx);
    if (!slot) return -1;
    
    *entry = slot->data[offset];
    return 0;
}

static int fat_write_entry(uint64_t block_idx, uint32_t entry) {
    if (block_idx >= g_sb.total_blocks) return -1;

    int64_t fat_block_idx = block_idx / FAT_ENTRIES_PER_BLOCK;
    uint64_t offset = block_idx % FAT_ENTRIES_PER_BLOCK;
    
    FatCacheEntry* slot = fat_get_cache_slot(fat_block_idx);
    if (!slot) return -1;
    
    slot->data[offset] = entry;
    slot->dirty = 1;
    return 0;
}

static int64_t fat_alloc(void) {
    uint64_t total_blocks = g_sb.total_blocks;
    uint64_t start_idx = g_sb.next_free_block;
    uint64_t checked_count = 0;
    
    uint64_t current_fat_block_idx = start_idx / FAT_ENTRIES_PER_BLOCK;
    uint64_t offset_in_block = start_idx % FAT_ENTRIES_PER_BLOCK;
    
    uint64_t total_fat_blocks = (total_blocks + FAT_ENTRIES_PER_BLOCK - 1) / FAT_ENTRIES_PER_BLOCK;

    for (uint64_t i = 0; i < total_fat_blocks; ++i) {
        uint64_t target_fat_block = (current_fat_block_idx + i) % total_fat_blocks;
        
        FatCacheEntry* slot = fat_get_cache_slot(target_fat_block);
        if (!slot) return -1;

        uint64_t start_k = (i == 0) ? offset_in_block : 0;
        
        for (uint64_t k = start_k; k < FAT_ENTRIES_PER_BLOCK; ++k) {
            uint64_t actual_block_idx = (target_fat_block * FAT_ENTRIES_PER_BLOCK) + k;
            
            if (actual_block_idx >= total_blocks) continue;
            if (checked_count >= total_blocks) return -1;
            
            if (slot->data[k] == FAT_FREE) {
                slot->data[k] = FAT_EOF;
                slot->dirty = 1;
                
                g_sb.next_free_block = (actual_block_idx + 1) % total_blocks;
                if (update_superblock() < 0) return -1;
                
                return actual_block_idx;
            }
            checked_count++;
        }
    }
    return -1;
}

static void fat_free_chain(uint64_t start_block) {
    uint64_t curr = start_block;
    while (curr != FAT_EOF && curr < g_sb.total_blocks) {
        uint32_t next;
        if (fat_read_entry(curr, &next) < 0) break;
        fat_write_entry(curr, FAT_FREE);
        curr = next;
    }
}

// --- Node Management ---

static int read_node(uint32_t id, Node* node) {
    if (g_fs_fd < 0 || id >= g_sb.max_nodes) return -1;
    off_t offset = sizeof(Superblock) + (id * sizeof(Node));
    if (lseek(g_fs_fd, offset, SEEK_SET) < 0) return -1;
    if (read(g_fs_fd, node, sizeof(Node)) != sizeof(Node)) return -1;
    return 0;
}

static int write_node(uint32_t id, const Node* node) {
    if (g_fs_fd < 0 || id >= g_sb.max_nodes) return -1;
    off_t offset = sizeof(Superblock) + (id * sizeof(Node));
    if (lseek(g_fs_fd, offset, SEEK_SET) < 0) return -1;
    if (write(g_fs_fd, node, sizeof(Node)) != sizeof(Node)) return -1;
    // fsync(g_fs_fd); // Optimization: Don't sync on every node write, rely on upper layer or sync()
    return 0;
}

static int update_superblock(void) {
    if (g_fs_fd < 0) return -1;
    if (lseek(g_fs_fd, 0, SEEK_SET) < 0) return -1;
    if (write(g_fs_fd, &g_sb, sizeof(Superblock)) != sizeof(Superblock)) return -1;
    // fsync(g_fs_fd);
    return 0;
}

// --- Bitmap Management ---

static int bitmap_init(void) {
    if (g_node_bitmap) free(g_node_bitmap);
    size_t bitmap_size = (g_sb.max_nodes + 7) / 8;
    g_node_bitmap = malloc(bitmap_size);
    if (!g_node_bitmap) return -1;
    
    if (lseek(g_fs_fd, g_sb.node_bitmap_ptr, SEEK_SET) < 0) return -1;
    if (read(g_fs_fd, g_node_bitmap, bitmap_size) != (ssize_t)bitmap_size) return -1;
    return 0;
}

static int bitmap_sync(void) {
    if (!g_node_bitmap) return -1;
    size_t bitmap_size = (g_sb.max_nodes + 7) / 8;
    if (lseek(g_fs_fd, g_sb.node_bitmap_ptr, SEEK_SET) < 0) return -1;
    if (write(g_fs_fd, g_node_bitmap, bitmap_size) != (ssize_t)bitmap_size) return -1;
    return 0;
}

static int32_t bitmap_alloc_id(void) {
    if (!g_node_bitmap) return -1;
    for (uint32_t i = 1; i < g_sb.max_nodes; ++i) { // ID 0 is Root
        if (!(g_node_bitmap[i / 8] & (1 << (i % 8)))) {
            g_node_bitmap[i / 8] |= (1 << (i % 8));
            bitmap_sync();
            return i;
        }
    }
    return -1;
}

static void bitmap_free_id(uint32_t id) {
    if (!g_node_bitmap || id >= g_sb.max_nodes) return;
    g_node_bitmap[id / 8] &= ~(1 << (id % 8));
    bitmap_sync();
}

// --- Directory Operations ---

// Helper to append a DirectoryEntry to a node's data
static int dir_add_entry(uint32_t parent_id, uint32_t child_id, const char* name, uint8_t type) {
    DirectoryEntry entry;
    entry.node_id = child_id;
    entry.type = type;
    strncpy(entry.name, name, NODE_NAME_MAX - 1);
    entry.name[NODE_NAME_MAX - 1] = '\0';

    Node parent;
    if (read_node(parent_id, &parent) < 0) return -1;
    if (parent.type != NODE_TYPE_DIR && parent.type != NODE_TYPE_ROOT) return -1;
    
    // Append to the end of the file
    uint64_t offset = parent.data_size;
    
    // We reuse nodefs_write_data logic but we need to be careful about recursion if we used public API
    // Implementing a direct append here using existing helpers
    
    // 1. Check if we need to allocate a new block
    if (parent.data_ptr == FAT_EOF) {
        int64_t new_block = fat_alloc();
        if (new_block < 0) return -1;
        parent.data_ptr = new_block;
        fat_write_entry(new_block, FAT_EOF);
    }

    // Calculate where to write
    uint64_t current_block_idx = parent.data_ptr;
    uint64_t block_offset = offset % BLOCK_SIZE;
    
    // Traverse to the last block
    // Traverse to the correct block based on offset
    size_t blocks_to_skip = offset / BLOCK_SIZE;
    for (size_t i = 0; i < blocks_to_skip; ++i) {
        uint32_t next;
        if (fat_read_entry(current_block_idx, &next) < 0) return -1;
        if (next == FAT_EOF) {
            // Corruption: Chain shorter than expected
            return -1;
        }
        current_block_idx = next;
    }

    // If entry doesn't fit in current block, alloc new one
    if (block_offset + sizeof(DirectoryEntry) > BLOCK_SIZE) {
        int64_t new_block = fat_alloc();
        if (new_block < 0) return -1;
        fat_write_entry(current_block_idx, new_block);
        fat_write_entry(new_block, FAT_EOF);
        current_block_idx = new_block;
        block_offset = 0;
    }

    // Write entry
    off_t disk_offset = g_sb.data_start_ptr + (current_block_idx * BLOCK_SIZE) + block_offset;
    lseek(g_fs_fd, disk_offset, SEEK_SET);
    write(g_fs_fd, &entry, sizeof(DirectoryEntry));

    // Update parent size
    parent.data_size += sizeof(DirectoryEntry);
    parent.mtime = parent.ctime = get_current_time();
    write_node(parent_id, &parent);
    
    return 0;
}

// Internal helper that doesn't lock
static int nodefs_read_data_nolock(uint32_t node_id, uint64_t offset, char* buffer, size_t len) {
    Node node;
    if (read_node(node_id, &node) < 0) return -1;
    
    if (node.data_size == 0 || node.data_ptr == FAT_EOF) return 0;
    if (offset >= node.data_size) return 0;

    size_t to_read = len;
    if (offset + to_read > node.data_size) to_read = node.data_size - offset;

    uint64_t curr_block = node.data_ptr;
    size_t blocks_to_skip = offset / BLOCK_SIZE;
    size_t block_offset = offset % BLOCK_SIZE;

    for (size_t i = 0; i < blocks_to_skip; ++i) {
        uint32_t next;
        if (fat_read_entry(curr_block, &next) < 0) return -1;
        curr_block = next;
        if (curr_block == FAT_EOF) return 0;
    }

    size_t bytes_read = 0;
    while (bytes_read < to_read && curr_block != FAT_EOF) {
        size_t chunk_size = BLOCK_SIZE - block_offset;
        if (chunk_size > (to_read - bytes_read)) chunk_size = to_read - bytes_read;

        off_t disk_offset = g_sb.data_start_ptr + (curr_block * BLOCK_SIZE) + block_offset;
        lseek(g_fs_fd, disk_offset, SEEK_SET);
        read(g_fs_fd, buffer + bytes_read, chunk_size);
        bytes_read += chunk_size;
        block_offset = 0;
        
        if (bytes_read < to_read) {
            uint32_t next;
            if (fat_read_entry(curr_block, &next) < 0) break;
            curr_block = next;
        }
    }
    return bytes_read;
}

static int dir_find_entry(uint32_t parent_id, const char* name) {
    Node parent;
    if (read_node(parent_id, &parent) < 0) return -1;
    if (parent.type != NODE_TYPE_DIR && parent.type != NODE_TYPE_ROOT) return -1;

    uint64_t offset = 0;
    DirectoryEntry entry;

    while (offset < parent.data_size) {
        if (nodefs_read_data_nolock(parent_id, offset, (char*)&entry, sizeof(DirectoryEntry)) != sizeof(DirectoryEntry)) {
            break;
        }
        if (entry.node_id != 0 && strcmp(entry.name, name) == 0) {
            return entry.node_id;
        }
        offset += sizeof(DirectoryEntry);
    }

    return -1;
}

// --- Public API ---

int nodefs_format(const char* path, size_t size_mb) {
    printf("[NodeFS] Formatting %s (%zu MB)...\n", path, size_mb);
    
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;

    uint64_t disk_size = (uint64_t)size_mb * 1024 * 1024;
    if (lseek(fd, disk_size - 1, SEEK_SET) < 0) { close(fd); return -1; }
    write(fd, "", 1);
    lseek(fd, 0, SEEK_SET);

    // Layout: [Superblock] [Node Table] [Node Bitmap] [FAT] [Data Blocks]
    uint32_t max_nodes = MAX_NODES;
    uint64_t node_table_size = max_nodes * sizeof(Node);
    uint64_t bitmap_size = (max_nodes + 7) / 8;
    
    uint64_t total_blocks = disk_size / BLOCK_SIZE;
    uint64_t fat_size = total_blocks * sizeof(uint32_t);
    
    uint64_t sb_size = sizeof(Superblock);
    uint64_t node_bitmap_offset = sb_size + node_table_size;
    uint64_t fat_offset = node_bitmap_offset + bitmap_size;
    uint64_t meta_end = fat_offset + fat_size;
    uint64_t data_start = (meta_end + BLOCK_SIZE - 1) & ~(BLOCK_SIZE - 1);
    
    total_blocks = (disk_size - data_start) / BLOCK_SIZE;
    
    Superblock sb;
    sb.magic = NODEFS_MAGIC;
    sb.version = NODEFS_VERSION;
    sb.disk_size = disk_size;
    sb.root_node_id = 0;
    sb.node_count = 1; 
    sb.max_nodes = max_nodes;
    sb.node_bitmap_ptr = node_bitmap_offset;
    sb.fat_ptr = fat_offset;
    sb.total_blocks = total_blocks;
    sb.data_start_ptr = data_start;
    sb.next_free_block = 0;

    write(fd, &sb, sizeof(Superblock));

    // Clear Node Table
    // Optimization: Write in chunks
    char* zero_buf = calloc(1, BLOCK_SIZE);
    uint64_t written = 0;
    while (written < node_table_size) {
        size_t to_write = (node_table_size - written) > BLOCK_SIZE ? BLOCK_SIZE : (node_table_size - written);
        write(fd, zero_buf, to_write);
        written += to_write;
    }

    // Clear Bitmap
    lseek(fd, node_bitmap_offset, SEEK_SET);
    written = 0;
    while (written < bitmap_size) {
        size_t to_write = (bitmap_size - written) > BLOCK_SIZE ? BLOCK_SIZE : (bitmap_size - written);
        write(fd, zero_buf, to_write);
        written += to_write;
    }

    // Initialize Root Node
    Node root;
    memset(&root, 0, sizeof(Node));
    root.id = 0;
    strcpy(root.name, "ROOT");
    root.type = NODE_TYPE_ROOT;
    root.atime = root.mtime = root.ctime = get_current_time();
    root.uid = 0; root.gid = 0; root.mode = 0755;
    root.data_ptr = FAT_EOF;
    
    lseek(fd, sizeof(Superblock), SEEK_SET);
    write(fd, &root, sizeof(Node));

    // Mark Root in Bitmap
    uint8_t root_bit = 1; // Bit 0
    lseek(fd, node_bitmap_offset, SEEK_SET);
    write(fd, &root_bit, 1);

    // Clear FAT
    lseek(fd, fat_offset, SEEK_SET);
    written = 0;
    while (written < fat_size) {
        size_t to_write = (fat_size - written) > BLOCK_SIZE ? BLOCK_SIZE : (fat_size - written);
        write(fd, zero_buf, to_write);
        written += to_write;
    }
    
    free(zero_buf);
    printf("[NodeFS] Format complete. Data starts at %lu, Blocks: %lu\n", data_start, total_blocks);
    close(fd);
    return 0;
}

int nodefs_mount(const char* path) {
    pthread_mutex_lock(&g_fs_lock);
    
    g_fs_fd = open(path, O_RDWR);
    if (g_fs_fd < 0) { pthread_mutex_unlock(&g_fs_lock); return -1; }

    if (read(g_fs_fd, &g_sb, sizeof(Superblock)) != sizeof(Superblock)) {
        close(g_fs_fd); pthread_mutex_unlock(&g_fs_lock); return -1;
    }

    if (g_sb.magic != NODEFS_MAGIC) {
        printf("[NodeFS] Invalid magic.\n");
        close(g_fs_fd); pthread_mutex_unlock(&g_fs_lock); return -1;
    }

    if (g_sb.version != NODEFS_VERSION) {
        printf("[NodeFS] Version mismatch. Disk: %u, Code: %u\n", g_sb.version, NODEFS_VERSION);
        close(g_fs_fd); pthread_mutex_unlock(&g_fs_lock); return -2;
    }

    g_current_node_id = g_sb.root_node_id;
    fat_init_cache();
    if (bitmap_init() < 0) {
        printf("[NodeFS] Failed to load bitmap.\n");
        close(g_fs_fd); pthread_mutex_unlock(&g_fs_lock); return -1;
    }
    
    printf("[NodeFS] Mounted. Ver: %u, Nodes: %u/%u\n", g_sb.version, g_sb.node_count, g_sb.max_nodes);
    pthread_mutex_unlock(&g_fs_lock);
    return 0;
}

int nodefs_unmount(void) {
    pthread_mutex_lock(&g_fs_lock);
    if (g_fs_fd >= 0) {
        fat_flush_all();
        bitmap_sync();
        if (g_node_bitmap) { free(g_node_bitmap); g_node_bitmap = NULL; }
        close(g_fs_fd);
        g_fs_fd = -1;
    }
    pthread_mutex_unlock(&g_fs_lock);
    return 0;
}

int nodefs_sync(void) {
    pthread_mutex_lock(&g_fs_lock);
    if (g_fs_fd < 0) { pthread_mutex_unlock(&g_fs_lock); return -1; }
    fat_flush_all();
    bitmap_sync();
    update_superblock();
    fsync(g_fs_fd);
    pthread_mutex_unlock(&g_fs_lock);
    return 0;
}

int nodefs_create_node(uint32_t parent_id, const char* name, NodeType type) {
    pthread_mutex_lock(&g_fs_lock);
    
    // 0. Check Parent Type
    Node parent;
    if (read_node(parent_id, &parent) < 0) { pthread_mutex_unlock(&g_fs_lock); return -1; }
    if (parent.type != NODE_TYPE_DIR && parent.type != NODE_TYPE_ROOT) {
        printf("[NodeFS] Parent is not a directory.\n");
        pthread_mutex_unlock(&g_fs_lock);
        return -1;
    }

    // 1. Check if name exists in parent
    if (dir_find_entry(parent_id, name) >= 0) {
        printf("[NodeFS] Name '%s' already exists.\n", name);
        pthread_mutex_unlock(&g_fs_lock);
        return -1;
    }

    // 2. Alloc ID
    int32_t new_id = bitmap_alloc_id();
    if (new_id < 0) {
        printf("[NodeFS] No free nodes.\n");
        pthread_mutex_unlock(&g_fs_lock);
        return -1;
    }

    // 3. Init Node
    Node new_node;
    memset(&new_node, 0, sizeof(Node));
    new_node.id = new_id;
    strncpy(new_node.name, name, NODE_NAME_MAX - 1);
    new_node.type = type;
    new_node.data_ptr = FAT_EOF;
    new_node.atime = new_node.mtime = new_node.ctime = get_current_time();
    new_node.uid = getuid(); new_node.gid = getgid();
    new_node.mode = (type == NODE_TYPE_DIR) ? 0755 : 0644;
    
    if (write_node(new_id, &new_node) < 0) {
        bitmap_free_id(new_id);
        pthread_mutex_unlock(&g_fs_lock);
        return -1;
    }

    // 4. Add to Parent
    if (dir_add_entry(parent_id, new_id, name, type) < 0) {
        // Rollback
        bitmap_free_id(new_id);
        pthread_mutex_unlock(&g_fs_lock);
        return -1;
    }

    g_sb.node_count++;
    update_superblock();
    
    pthread_mutex_unlock(&g_fs_lock);
    return new_id;
}

int nodefs_mkdir(uint32_t parent_id, const char* name) {
    return nodefs_create_node(parent_id, name, NODE_TYPE_DIR);
}

int nodefs_read_data(uint32_t node_id, uint64_t offset, char* buffer, size_t len) {
    pthread_mutex_lock(&g_fs_lock);
    int ret = nodefs_read_data_nolock(node_id, offset, buffer, len);
    pthread_mutex_unlock(&g_fs_lock);
    return ret;
}

int nodefs_write_data(uint32_t node_id, const char* data, size_t len) {
    pthread_mutex_lock(&g_fs_lock);
    Node node;
    if (read_node(node_id, &node) < 0) { pthread_mutex_unlock(&g_fs_lock); return -1; }

    // Overwrite: Free old chain
    if (node.data_ptr != FAT_EOF && node.data_ptr < g_sb.total_blocks) {
        fat_free_chain(node.data_ptr);
    }

    size_t blocks_needed = (len + BLOCK_SIZE - 1) / BLOCK_SIZE;
    if (blocks_needed == 0) {
        node.data_ptr = FAT_EOF;
        node.data_size = 0;
        node.mtime = node.ctime = get_current_time();
        write_node(node_id, &node);
        pthread_mutex_unlock(&g_fs_lock);
        return 0;
    }

    int64_t head_block = -1;
    int64_t prev_block = -1;
    size_t bytes_written = 0;

    for (size_t i = 0; i < blocks_needed; ++i) {
        int64_t new_block = fat_alloc();
        if (new_block < 0) {
            pthread_mutex_unlock(&g_fs_lock);
            return -1;
        }

        if (head_block == -1) head_block = new_block;
        if (prev_block != -1) fat_write_entry(prev_block, new_block);
        prev_block = new_block;

        size_t chunk = (len - bytes_written) > BLOCK_SIZE ? BLOCK_SIZE : (len - bytes_written);
        off_t offset = g_sb.data_start_ptr + (new_block * BLOCK_SIZE);
        lseek(g_fs_fd, offset, SEEK_SET);
        write(g_fs_fd, data + bytes_written, chunk);
        bytes_written += chunk;
    }
    
    if (prev_block != -1) fat_write_entry(prev_block, FAT_EOF);
    
    node.data_ptr = head_block;
    node.data_size = len;
    node.mtime = node.ctime = get_current_time();
    write_node(node_id, &node);
    
    pthread_mutex_unlock(&g_fs_lock);
    return 0;
}

int nodefs_list_dir(uint32_t dir_id) {
    pthread_mutex_lock(&g_fs_lock);
    Node dir;
    if (read_node(dir_id, &dir) < 0) { pthread_mutex_unlock(&g_fs_lock); return -1; }
    
    if (dir.type != NODE_TYPE_DIR && dir.type != NODE_TYPE_ROOT) {
        printf("Not a directory.\n");
        pthread_mutex_unlock(&g_fs_lock);
        return -1;
    }

    printf("--- Directory: %s [%u] ---\n", dir.name, dir.id);
    
    uint64_t offset = 0;
    DirectoryEntry entry;
    while (offset < dir.data_size) {
        if (nodefs_read_data_nolock(dir_id, offset, (char*)&entry, sizeof(DirectoryEntry)) != sizeof(DirectoryEntry)) break;
        
        if (entry.node_id != 0) {
             const char* type_str = (entry.type == NODE_TYPE_DIR) ? "DIR" : (entry.type == NODE_TYPE_LINK ? "LINK" : "FILE");
             
             char name_buf[64];
             strncpy(name_buf, entry.name, sizeof(name_buf)-1);
             name_buf[sizeof(name_buf)-1] = '\0';
             
             char* ext = strrchr(name_buf, '.');
             if (ext && ext != name_buf) {
                 *ext = '\0'; // Split name and ext
                 ext++; // Skip dot
                 printf("  [%u] %s [%s] [%s]\n", entry.node_id, name_buf, type_str, ext);
             } else {
                 printf("  [%u] %s [%s]\n", entry.node_id, entry.name, type_str);
             }
        }
        offset += sizeof(DirectoryEntry);
    }
    printf("--------------------------\n");
    pthread_mutex_unlock(&g_fs_lock);
    return 0;
}

int nodefs_find_node(const char* path) {
    pthread_mutex_lock(&g_fs_lock);
    if (g_fs_fd < 0) { pthread_mutex_unlock(&g_fs_lock); return -1; }

    // Start at root
    uint32_t current_id = 0; // Root is always 0
    
    // Handle root path "/"
    if (strcmp(path, "/") == 0) {
        pthread_mutex_unlock(&g_fs_lock);
        return 0;
    }

    char path_copy[256];
    strncpy(path_copy, path, 255);
    path_copy[255] = '\0';
    
    char* token = strtok(path_copy, "/");
    while (token != NULL) {
        Node dir;
        if (read_node(current_id, &dir) < 0) { pthread_mutex_unlock(&g_fs_lock); return -1; }
        if (dir.type != NODE_TYPE_DIR && dir.type != NODE_TYPE_ROOT) {
             pthread_mutex_unlock(&g_fs_lock); return -1; // Not a dir, can't traverse
        }

        int found = 0;
        uint64_t offset = 0;
        DirectoryEntry entry;
        while (offset < dir.data_size) {
            if (nodefs_read_data_nolock(current_id, offset, (char*)&entry, sizeof(DirectoryEntry)) != sizeof(DirectoryEntry)) break;
            
            if (entry.node_id != 0 && strcmp(entry.name, token) == 0) {
                current_id = entry.node_id;
                found = 1;
                break;
            }
            offset += sizeof(DirectoryEntry);
        }

        if (!found) {
            pthread_mutex_unlock(&g_fs_lock);
            return -1;
        }

        token = strtok(NULL, "/");
    }

    pthread_mutex_unlock(&g_fs_lock);
    return current_id;
}

int nodefs_unlink_node(uint32_t parent_id, uint32_t child_id) {
    pthread_mutex_lock(&g_fs_lock);
    Node parent;
    if (read_node(parent_id, &parent) < 0) { pthread_mutex_unlock(&g_fs_lock); return -1; }
    if (parent.type != NODE_TYPE_DIR && parent.type != NODE_TYPE_ROOT) {
        pthread_mutex_unlock(&g_fs_lock); return -1;
    }

    // Scan for entry with child_id
    uint64_t offset = 0;
    DirectoryEntry entry;
    int found = 0;
    uint64_t found_offset = 0;

    while (offset < parent.data_size) {
        if (nodefs_read_data_nolock(parent_id, offset, (char*)&entry, sizeof(DirectoryEntry)) != sizeof(DirectoryEntry)) break;
        
        if (entry.node_id == child_id) {
            found = 1;
            found_offset = offset;
            break;
        }
        offset += sizeof(DirectoryEntry);
    }

    if (!found) {
        pthread_mutex_unlock(&g_fs_lock);
        return -1;
    }

    // Swap-with-Last Strategy
    // 1. Read the last entry
    uint64_t last_offset = parent.data_size - sizeof(DirectoryEntry);
    DirectoryEntry last_entry;
    
    // If the entry to delete is already the last one, just truncate
    if (found_offset == last_offset) {
        parent.data_size -= sizeof(DirectoryEntry);
        // TODO: Free block if empty? For now, keep it simple.
    } else {
        // Read last entry
        if (nodefs_read_data_nolock(parent_id, last_offset, (char*)&last_entry, sizeof(DirectoryEntry)) != sizeof(DirectoryEntry)) {
            pthread_mutex_unlock(&g_fs_lock);
            return -1;
        }
        
        // Write last entry to found_offset
        // We need to write to the exact block/offset of found_offset
        uint64_t curr_block = parent.data_ptr;
        size_t blocks_to_skip = found_offset / BLOCK_SIZE;
        size_t block_offset = found_offset % BLOCK_SIZE;

        for (size_t i = 0; i < blocks_to_skip; ++i) {
            uint32_t next;
            fat_read_entry(curr_block, &next);
            curr_block = next;
        }

        off_t disk_offset = g_sb.data_start_ptr + (curr_block * BLOCK_SIZE) + block_offset;
        lseek(g_fs_fd, disk_offset, SEEK_SET);
        write(g_fs_fd, &last_entry, sizeof(DirectoryEntry));
        
        // Reduce size
        parent.data_size -= sizeof(DirectoryEntry);
    }

    // Update parent mtime
    parent.mtime = parent.ctime = get_current_time();
    write_node(parent_id, &parent);

    pthread_mutex_unlock(&g_fs_lock);
    return 0;
}

int nodefs_link(uint32_t parent_id, uint32_t child_id, const char* name) {
    pthread_mutex_lock(&g_fs_lock);
    
    // Check if name exists
    if (dir_find_entry(parent_id, name) >= 0) {
        pthread_mutex_unlock(&g_fs_lock);
        return -1;
    }
    
    // Get child type
    Node child;
    if (read_node(child_id, &child) < 0) {
        pthread_mutex_unlock(&g_fs_lock);
        return -1;
    }

    if (dir_add_entry(parent_id, child_id, name, child.type) < 0) {
        pthread_mutex_unlock(&g_fs_lock);
        return -1;
    }
    
    pthread_mutex_unlock(&g_fs_lock);
    return 0;
}

int nodefs_delete_node(uint32_t id) {
    pthread_mutex_lock(&g_fs_lock);
    Node node;
    if (read_node(id, &node) < 0) { pthread_mutex_unlock(&g_fs_lock); return -1; }
    
    if (node.data_ptr != FAT_EOF && node.data_ptr < g_sb.total_blocks) {
        fat_free_chain(node.data_ptr);
    }
    
    bitmap_free_id(id);
    
    g_sb.node_count--;
    update_superblock();
    pthread_mutex_unlock(&g_fs_lock);
    return 0;
}

uint32_t nodefs_get_current_node(void) { return g_current_node_id; }
int nodefs_set_current_node(uint32_t id) {
    pthread_mutex_lock(&g_fs_lock);
    Node node;
    if (read_node(id, &node) < 0) {
        pthread_mutex_unlock(&g_fs_lock);
        return -1;
    }
    if (node.type != NODE_TYPE_DIR && node.type != NODE_TYPE_ROOT) {
        pthread_mutex_unlock(&g_fs_lock);
        return -1;
    }
    g_current_node_id = id;
    pthread_mutex_unlock(&g_fs_lock);
    return 0;
}
