#ifndef NODEFS_H
#define NODEFS_H

#include <stdint.h>
#include <stddef.h>

#define NODEFS_MAGIC 0x4E4F4445 // "NODE"
#define NODEFS_VERSION 4        // Version 4: Scalable Directories & Bitmap Allocation
#define NODE_NAME_MAX 32
#define BLOCK_SIZE 4096
#define FAT_EOF 0xFFFFFFFF
#define FAT_FREE 0x00000000

// Scalability Limits
#define MAX_NODES 65536         // Increased from 4096

typedef enum {
    NODE_TYPE_FREE = 0,
    NODE_TYPE_ROOT = 1,
    NODE_TYPE_FILE = 2,
    NODE_TYPE_DIR  = 3,
    NODE_TYPE_DEVICE = 4,
    NODE_TYPE_LINK = 5,         // Restored for compatibility
    NODE_TYPE_DELETED = 0
} NodeType;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t disk_size;
    uint32_t root_node_id;
    uint32_t node_count;      // Active nodes
    uint32_t max_nodes;       // Capacity of node table
    uint64_t fat_ptr;         // Pointer to File Allocation Table
    uint64_t total_blocks;
    uint64_t data_start_ptr;
    uint64_t next_free_block; // Cache for next fit allocation
    uint64_t node_bitmap_ptr; // Pointer to Node Allocation Bitmap
} Superblock;

// Directory Entry (stored in data blocks of DIR nodes)
typedef struct {
    uint32_t node_id;
    uint8_t type;
    char name[NODE_NAME_MAX];
} DirectoryEntry;

typedef struct {
    uint32_t id;
    char name[NODE_NAME_MAX]; // Kept for recovery/debug, but name is primarily in parent DirEntry
    NodeType type;
    
    // Removed fixed links array
    
    uint64_t data_ptr; // Offset to data block (Content for FILE, DirEntries for DIR)
    uint64_t data_size;
    
    // Metadata
    uint64_t atime; // Access time
    uint64_t mtime; // Modification time
    uint64_t ctime; // Change time
    uint32_t uid;   // User ID
    uint32_t gid;   // Group ID
    uint16_t mode;  // Permissions
    uint8_t padding[16]; // Reserved/Padding (Aligned to 128 bytes approx)
} Node;

// API
int nodefs_format(const char* path, size_t size_mb);
int nodefs_mount(const char* path);
int nodefs_unmount(void);
int nodefs_sync(void);

// Node Operations
int nodefs_create_node(uint32_t parent_id, const char* name, NodeType type);
int nodefs_write_data(uint32_t node_id, const char* data, size_t len);
int nodefs_read_data(uint32_t node_id, uint64_t offset, char* buffer, size_t len);
int nodefs_delete_node(uint32_t id);
int nodefs_unlink_node(uint32_t parent_id, uint32_t child_id);
int nodefs_link(uint32_t parent_id, uint32_t child_id, const char* name); // New link API

// Directory Operations
int nodefs_mkdir(uint32_t parent_id, const char* name);
int nodefs_list_dir(uint32_t dir_id); // Replaces list_nodes
int nodefs_find_node(const char* path); // Supports paths now

// Helpers
uint32_t nodefs_get_current_node(void);
int nodefs_set_current_node(uint32_t id);

int nedit_run(uint32_t node_id);

#endif // NODEFS_H
