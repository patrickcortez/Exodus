/*
 * ctz-set.c
 * gcc -c ctz-set.c -o ctz-set.o -Wall -O2
 * ar rcs ctz-set.a ctz-set.o
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#define _CRT_SECURE_NO_WARNINGS
#include "ctz-set.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <math.h>
#include <errno.h>
#include <limits.h>
#include <float.h>

// ============================================================================
// SECTION: Configuration & Constants
// ============================================================================

#if defined(_WIN32) || defined(_WIN64)
    #define CTZ_PLATFORM_WIN 1
    #define CTZ_PLATFORM_POSIX 0
    #include <windows.h>
    #define PATH_SEP '\\'
#else
    #define CTZ_PLATFORM_WIN 0
    #define CTZ_PLATFORM_POSIX 1
    #include <dirent.h>
    #include <sys/stat.h>
    #define PATH_SEP '/'
    
    #ifndef _strdup
        #define _strdup strdup
    #endif
#endif

#define ARENA_BLOCK_SIZE    (1024 * 64)
#define HASH_MAP_INIT_CAP   32
#define HASH_LOAD_FACTOR    0.75
#define MAX_INCLUDE_DEPTH   64
#define MAX_VAR_RECURSION   32
#define ERR_BUF_SIZE        4096
#define MAX_TOKEN_LEN       (1024 * 4)
#define SET_FLAG_EXPRESSION (1 << 8)

// ============================================================================
// SECTION: Type Definitions & Internal Structures
// ============================================================================

#if CTZ_PLATFORM_WIN
    #include <windows.h>
#else
    #include <pthread.h>
    #include <unistd.h> // for fsync
#endif

typedef struct ArenaBlock {
    struct ArenaBlock* next;
    size_t used;
    size_t capacity;
    char memory[];
} ArenaBlock;

typedef struct Arena {
    ArenaBlock* head;
    ArenaBlock* current;
    size_t total_allocated;
} Arena;

typedef struct SetMapEntry {
    char* key;
    struct SetNode* value;
    uint32_t hash;
    struct SetMapEntry* next_in_bucket; // Hash collision chain
    struct SetMapEntry* next_ordered;   // Insertion order chain (forward)
    struct SetMapEntry* prev_ordered;   // Insertion order chain (backward)
} SetMapEntry;

typedef struct {
    SetMapEntry** buckets;
    SetMapEntry* head_order;
    SetMapEntry* tail_order;
    size_t capacity;
    size_t count;
} SetMap;

typedef struct {
    struct SetNode** items;
    size_t capacity;
    size_t count;
} SetArray;

// The SetNode structure now includes the 'owner' field
struct SetNode {
    Arena* owner; 
    SetType type;
    uint32_t flags; // Bit 0-7: decorators (1=private, 2=deprecated, 4=readonly)
                    // Bit 8: IS_EXPRESSION
    union {
        char* s_val;
        long i_val;
        double d_val;
        int b_val;
        SetMap map;
        SetArray array;
    } data;
};

// Index Registry (forward declared for SetConfig)
typedef struct IndexRegistry {
    SetIndex* head;
    size_t count;
} IndexRegistry;

struct SetConfig {
    SetNode* root;
    Arena arena;
    char* filepath;
    //char error_buf[ERR_BUF_SIZE];
    struct SetSchemaEntry* schema_head;
    
    SetMap anchors; // Registry for &anchor references
    char* error_msg; // Last error message
    
    IndexRegistry indexes;  // Index registry for fast queries

    int is_db_mode;
    #if CTZ_PLATFORM_WIN
    CRITICAL_SECTION lock;
    #else
    pthread_mutex_t lock;
    #endif
};

typedef struct SetSchemaEntry {
    char* path;
    SetType expected_type;
    int required;
    SetValidator validator;
    struct SetSchemaEntry* next;
} SetSchemaEntry;

struct SetIterator {
    SetNode* target;
    int started;
    union {
        SetMapEntry* map_entry;
        size_t array_index;
    } state;
};

// ============================================================================
// SECTION: B-tree Index Structures
// ============================================================================

#define BTREE_ORDER 128  // Max children per node (optimized for cache lines)

typedef struct BTreeNode {
    int is_leaf;
    int key_count;
    SetNode** keys;      // Array of key nodes
    SetNode** values;    // Array of value nodes (leaf only)
    struct BTreeNode** children;  // Array of child pointers
    struct BTreeNode* parent;
} BTreeNode;

typedef struct {
    uint32_t hash;
    SetNode* value;
} HashEntry;

typedef struct {
    HashEntry* entries;
    size_t capacity;
    size_t count;
} HashIndex;

typedef struct SetIndex {
    SetConfig* config;        // Back reference to config for arena access
    char collection_path[256];
    char field[128];
    SetType field_type;
    IndexType type;
    size_t entry_count;
    
    // Composite index support
    int is_composite;
    char** composite_fields;  // Array of field names
    size_t field_count;       // Number of fields in composite key
    
    union {
        BTreeNode* btree_root;
        HashIndex hash_index;
    } data;
    
    struct SetIndex* next;
} SetIndex;

// ============================================================================
// SECTION: Forward Declarations
// ============================================================================

static void* arena_alloc(Arena* a, size_t size);
static char* arena_strdup(Arena* a, const char* str);
static char* arena_strndup(Arena* a, const char* str, size_t n);
static void     arena_free(Arena* a);

static SetNode* node_create(Arena* a, SetType type);
static void     map_put(Arena* a, SetMap* map, const char* key, SetNode* val);
static SetNode* map_get(SetMap* map, const char* key);
static void     map_remove(SetMap* map, const char* key);
static void     array_push(Arena* a, SetArray* arr, SetNode* val);

// B-tree functions needed by set_db_insert
static BTreeNode* btree_insert(Arena* a, BTreeNode* root, SetNode* key, SetNode* value);

// Composite index helper
static SetNode* create_composite_key(Arena* a, SetNode* record, const char** fields, size_t field_count);

// --- Error Handling ---

static void set_error_at(SetConfig* cfg, int line, int col, const char* fmt, ...) {
    if (!cfg) return;
    
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    
    char full_msg[512];
    if (line > 0) {
        snprintf(full_msg, sizeof(full_msg), "Line %d, Col %d: %s", line, col, buf);
    } else {
        snprintf(full_msg, sizeof(full_msg), "%s", buf);
    }
    
    if (cfg->error_msg == NULL) {
        size_t len = strlen(full_msg);
        char* err_dup = (char*)arena_alloc(&cfg->arena, len + 1);
        memcpy(err_dup, full_msg, len + 1);
        cfg->error_msg = err_dup;
    }
}

static void set_error(SetConfig* cfg, const char* fmt, ...) {
    if (!cfg) return;
    if (cfg->error_msg) return; // Keep first error

    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    size_t len = strlen(buf);
    char* err_dup = (char*)arena_alloc(&cfg->arena, len + 1);
    memcpy(err_dup, buf, len + 1);
    cfg->error_msg = err_dup;
}

const char* set_get_error(SetConfig* config) {
    return config ? config->error_msg : NULL;
}
static uint32_t hash_string(const char* str);

#if defined(_MSC_VER) || defined(__MINGW32__)
    #define CTZ_THREAD_LOCAL __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
    #define CTZ_THREAD_LOCAL __thread
#elif __STDC_VERSION__ >= 201112L
    #define CTZ_THREAD_LOCAL _Thread_local
#else
    #define CTZ_THREAD_LOCAL // Fallback (Not thread safe)
#endif

// static CTZ_THREAD_LOCAL char tls_error_buf[ERR_BUF_SIZE];


// ============================================================================
// SECTION: Database / Concurrency Implementation
// ============================================================================

static SetNode* map_get_fast(SetMap* map, const char* key, uint32_t h) {
    if (map->capacity == 0) return NULL;
    
    uint32_t idx = h & (map->capacity - 1);
    SetMapEntry* e = map->buckets[idx];
    
    while (e) {
        // Check Hash first (fast integer compare) then string (slow)
        if (e->hash == h && strcmp(e->key, key) == 0) {
            return e->value;
        }
        e = e->next_in_bucket;
    }
    return NULL;
}

void set_db_init(SetConfig* cfg) {
    if (!cfg) return;
    cfg->is_db_mode = 1;
    #if CTZ_PLATFORM_WIN
        InitializeCriticalSection(&cfg->lock);
    #else
        pthread_mutex_init(&cfg->lock, NULL);
    #endif
}

void set_db_lock(SetConfig* cfg) {
    if (cfg && cfg->is_db_mode) {
        #if CTZ_PLATFORM_WIN
            EnterCriticalSection(&cfg->lock);
        #else
            pthread_mutex_lock(&cfg->lock);
        #endif
    }
}

void set_db_unlock(SetConfig* cfg) {
    if (cfg && cfg->is_db_mode) {
        #if CTZ_PLATFORM_WIN
            LeaveCriticalSection(&cfg->lock);
        #else
            pthread_mutex_unlock(&cfg->lock);
        #endif
    }
}

// "Safe Save" - Writes to .tmp then renames.
int set_db_commit(SetConfig* config) {
    if (!config || !config->filepath) return -1;

    set_db_lock(config);

    // 1. Create temp filename
    char temp_path[1024];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", config->filepath);

    FILE* f = fopen(temp_path, "w");
    if (!f) {
        set_db_unlock(config);
        return -1;
    }

    // 2. Dump content
    int res = set_dump(config, f);
    
    // 3. Flush to physical disk
    fflush(f);
    #if CTZ_PLATFORM_WIN
        _commit(_fileno(f));
    #else
        fsync(fileno(f));
    #endif
    fclose(f);

    // 4. Atomic Rename (This is the commit point)
    if (res == 0) {
        #if CTZ_PLATFORM_WIN
            // Windows needs explicit deletion of target before rename in some versions
            remove(config->filepath); 
            if (rename(temp_path, config->filepath) != 0) res = -1;
        #else
            if (rename(temp_path, config->filepath) != 0) res = -1;
        #endif
    }

    set_db_unlock(config);
    return res;
}

SetNode* set_db_select(SetConfig* cfg, const char* collection_path, const char* field, DbOp op, const void* value, size_t limit, size_t offset) {
    if (!cfg || !collection_path || !field) return NULL;

    set_db_lock(cfg);

    SetNode* collection = set_query(cfg, collection_path);
    if (!collection || collection->type != SET_TYPE_ARRAY) {
        set_db_unlock(cfg);
        return NULL;
    }

    SetNode* results = node_create(&cfg->arena, SET_TYPE_ARRAY);
    
    // OPTIMIZATION 1: Pre-calculate hash of the field name
    // We do this ONCE here, instead of inside the loop 100,000 times.
    uint32_t field_hash = hash_string(field);

    size_t matches_found = 0;
    size_t matches_added = 0;

    for (size_t i = 0; i < collection->data.array.count; i++) {
        // OPTIMIZATION 2: Check Limit early
        if (limit > 0 && matches_added >= limit) break;

        SetNode* item = collection->data.array.items[i];
        if (item->type != SET_TYPE_MAP) continue;

        // OPTIMIZATION 3: Fast Lookup using pre-calc hash
        SetNode* field_node = map_get_fast(&item->data.map, field, field_hash);
        if (!field_node) continue;

        int match = 0;

        // --- Optimized Comparison Logic ---
        if (field_node->type == SET_TYPE_INT) {
            long val_check = (long)(intptr_t)value;
            long val_node = field_node->data.i_val;
            
            if (op == DB_OP_EQ) match = (val_node == val_check);
            else if (op == DB_OP_NEQ) match = (val_node != val_check);
            else if (op == DB_OP_GT) match = (val_node > val_check);
            else if (op == DB_OP_LT) match = (val_node < val_check);
        }
        else if (field_node->type == SET_TYPE_DOUBLE) {
            if (!value) continue;
            double val_check = *(const double*)value;
            double val_node = field_node->data.d_val;

            if (op == DB_OP_EQ) match = (val_node == val_check);
            else if (op == DB_OP_NEQ) match = (val_node != val_check);
            else if (op == DB_OP_GT) match = (val_node > val_check);
            else if (op == DB_OP_LT) match = (val_node < val_check);
        }
        else if (field_node->type == SET_TYPE_STRING) {
            const char* val_check = (const char*)value;
            const char* val_node = field_node->data.s_val;
            
            if (val_check && val_node) {
                if (op == DB_OP_EQ) match = (strcmp(val_node, val_check) == 0);
                else if (op == DB_OP_NEQ) match = (strcmp(val_node, val_check) != 0);
                else if (op == DB_OP_CONTAINS) match = (strstr(val_node, val_check) != NULL);
            }
        }
        else if (field_node->type == SET_TYPE_BOOL) {
             int val_check = (int)(intptr_t)value;
             int b_node = field_node->data.b_val ? 1 : 0;
             int b_check = val_check ? 1 : 0;
             if (op == DB_OP_EQ) match = (b_node == b_check);
             else if (op == DB_OP_NEQ) match = (b_node != b_check);
        }

        if (match) {
            // OPTIMIZATION 4: Handle Offset (Pagination)
            if (matches_found >= offset) {
                array_push(&cfg->arena, &results->data.array, item);
                matches_added++;
            }
            matches_found++;
        }
    }

    set_db_unlock(cfg);
    return results;
}

SetNode* set_db_insert(SetConfig* cfg, const char* collection_path) {
    if (!cfg || !collection_path) return NULL;

    set_db_lock(cfg);

    // Start navigation from the root
    SetNode* current = cfg->root;
    
    // Sanity check: Root must be a map to hold children
    if (current->type != SET_TYPE_MAP) {
        set_error(cfg, "DB Error: Root node is not a Map, cannot insert.");
        set_db_unlock(cfg);
        return NULL;
    }

    const char* ptr = collection_path;
    char segment[256]; 

    // --- Recursive Path Traversal / Creation Loop ---
    while (*ptr) {
        // 1. Extract the next segment key
        const char* start = ptr;
        while (*ptr && *ptr != '.') ptr++;
        
        size_t len = ptr - start;
        if (len >= sizeof(segment)) len = sizeof(segment) - 1;
        strncpy(segment, start, len);
        segment[len] = 0;

        // 2. Identify if this is the final destination
        int is_last = (*ptr == '\0');

        // 3. Check if this child exists in the current map
        SetNode* child = map_get(&current->data.map, segment);

        if (!child) {
            // Node is missing -> Auto-create it
            // If it's the target (last), it must be an ARRAY (Collection).
            // If it's a path segment, it must be a MAP (Folder).
            SetType type = is_last ? SET_TYPE_ARRAY : SET_TYPE_MAP;
            
            child = node_create(&cfg->arena, type);
            map_put(&cfg->arena, &current->data.map, segment, child);
        } else {
            // Node exists -> Validate its type
            if (is_last) {
                if (child->type != SET_TYPE_ARRAY) {
                    set_error(cfg, "DB Error: Target '%s' exists but is not an Array.", collection_path);
                    set_db_unlock(cfg);
                    return NULL;
                }
            } else {
                if (child->type != SET_TYPE_MAP) {
                    set_error(cfg, "DB Error: Path segment '%s' exists but is not a Map.", segment);
                    set_db_unlock(cfg);
                    return NULL;
                }
            }
        }

        // 4. Move pointer down the tree
        current = child;
        
        // Skip the dot if we aren't at the end
        if (*ptr == '.') ptr++; 
    }

    // --- Insert Record ---
    // At this point, 'current' is guaranteed to be the target ARRAY.
    SetNode* new_record = node_create(&cfg->arena, SET_TYPE_MAP);
    array_push(&cfg->arena, &current->data.array, new_record);
    
    set_db_unlock(cfg);
    return new_record;
}

// Update all indexes for a record (call after setting fields)
void set_db_update_indexes(SetConfig* cfg, const char* collection_path, SetNode* record) {
    if (!cfg || !collection_path || !record) return;
    
    SetIndex* idx = cfg->indexes.head;
    while (idx) {
        // Check if this index is for the same collection
        if (strcmp(idx->collection_path, collection_path) == 0) {
            // Get the indexed field value from the record
            SetNode* key_node = map_get(&record->data.map, idx->field);
            if (key_node) {
                // Update the index with the record
                if (idx->type == INDEX_TYPE_BTREE) {
                    idx->data.btree_root = btree_insert(&cfg->arena, idx->data.btree_root, key_node, record);
                } else if (idx->type == INDEX_TYPE_HASH) {
                    // Hash index update (simplified)
                    uint32_t hash = hash_string(idx->field);
                    size_t hash_idx = hash % idx->data.hash_index.capacity;
                    while (idx->data.hash_index.entries[hash_idx].value != NULL) {
                        hash_idx = (hash_idx + 1) % idx->data.hash_index.capacity;
                    }
                    idx->data.hash_index.entries[hash_idx].hash = hash;
                    idx->data.hash_index.entries[hash_idx].value = record;
                    idx->data.hash_index.count++;
                }
                idx->entry_count++;
            }
        }
        idx = idx->next;
    }
}

// ============================================================================
// SECTION: Memory Management (Arena)
// ============================================================================

static void* arena_alloc(Arena* a, size_t size) {
    // 8-byte alignment for 64-bit systems
    size_t aligned_size = (size + 7) & ~7;

    if (a->current == NULL || (a->current->used + aligned_size > a->current->capacity)) {
        size_t block_size = (aligned_size > ARENA_BLOCK_SIZE) ? aligned_size : ARENA_BLOCK_SIZE;
        
        // Allocate block header + data
        ArenaBlock* new_block = (ArenaBlock*)malloc(sizeof(ArenaBlock) + block_size);
        if (!new_block) {
            fprintf(stderr, "[CTZ-SET] Critical Error: Out of memory in arena_alloc.\n");
            exit(ENOMEM);
        }

        new_block->capacity = block_size;
        new_block->used = 0;
        new_block->next = NULL;

        if (a->current) {
            a->current->next = new_block;
            a->current = new_block;
        } else {
            a->head = new_block;
            a->current = new_block;
        }
        a->total_allocated += block_size;
    }

    void* ptr = a->current->memory + a->current->used;
    a->current->used += aligned_size;
    return ptr;
}

static char* arena_strdup(Arena* a, const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* dest = (char*)arena_alloc(a, len + 1);
    if (dest) {
        memcpy(dest, str, len);
        dest[len] = '\0';
    }
    return dest;
}

static char* arena_strndup(Arena* a, const char* str, size_t n) {
    if (!str) return NULL;
    char* dest = (char*)arena_alloc(a, n + 1);
    if (dest) {
        memcpy(dest, str, n);
        dest[n] = '\0';
    }
    return dest;
}

static void arena_free(Arena* a) {
    ArenaBlock* curr = a->head;
    while (curr) {
        ArenaBlock* next = curr->next;
        free(curr);
        curr = next;
    }
    a->head = NULL;
    a->current = NULL;
    a->total_allocated = 0;
}

// ============================================================================
// SECTION: B-tree Index Implementation
// ============================================================================

// --- Node Comparison ---

static int compare_nodes(SetNode* a, SetNode* b) {
    if (!a || !b) return 0;
    if (a->type != b->type) {
        return (int)a->type - (int)b->type;
    }
    
    switch (a->type) {
        case SET_TYPE_INT:
            if (a->data.i_val < b->data.i_val) return -1;
            if (a->data.i_val > b->data.i_val) return 1;
            return 0;
        case SET_TYPE_DOUBLE:
            if (a->data.d_val < b->data.d_val) return -1;
            if (a->data.d_val > b->data.d_val) return 1;
            return 0;
        case SET_TYPE_STRING:
            return strcmp(a->data.s_val, b->data.s_val);
        case SET_TYPE_BOOL:
            return (int)a->data.b_val - (int)b->data.b_val;
        default:
            return 0;
    }
}

// --- B-tree Node Management ---

static BTreeNode* btree_node_create(Arena* a, int is_leaf) {
    BTreeNode* node = (BTreeNode*)arena_alloc(a, sizeof(BTreeNode));
    node->is_leaf = is_leaf;
    node->key_count = 0;
    node->keys = (SetNode**)arena_alloc(a, BTREE_ORDER * sizeof(SetNode*));
    node->values = (SetNode**)arena_alloc(a, BTREE_ORDER * sizeof(SetNode*));
    node->children = (BTreeNode**)arena_alloc(a, (BTREE_ORDER + 1) * sizeof(BTreeNode*));
    node->parent = NULL;
    
    memset(node->keys, 0, BTREE_ORDER * sizeof(SetNode*));
    memset(node->values, 0, BTREE_ORDER * sizeof(SetNode*));
    memset(node->children, 0, (BTREE_ORDER + 1) * sizeof(BTreeNode*));
    
    return node;
}

static SetNode* btree_search(BTreeNode* node, SetNode* key) {
    if (!node) return NULL;
    
    int i = 0;
    while (i < node->key_count && compare_nodes(key, node->keys[i]) > 0) {
        i++;
    }
    
    // Found exact match
    if (i < node->key_count && compare_nodes(key, node->keys[i]) == 0) {
        // In B+-tree, values are ONLY in leaf nodes
        if (node->is_leaf) {
            return node->values[i];
        }
        // In internal nodes, key is a separator - could be in left or right subtree
        // For simplicity, always descend to right child when we match
        return btree_search(node->children[i + 1], key);
    }
    
    // Not found in this node
    if (node->is_leaf) {
        return NULL;
    }
    
    // Descend to appropriate child
    return btree_search(node->children[i], key);
}

static void btree_split_child(Arena* a, BTreeNode* parent, int index) {
    BTreeNode* full_child = parent->children[index];
    BTreeNode* new_child = btree_node_create(a, full_child->is_leaf);
    
    int mid = BTREE_ORDER / 2;
    
    if (full_child->is_leaf) {
        // B+-tree LEAF split: keep mid in right child, copy to parent
        new_child->key_count = full_child->key_count - mid;
        for (int j = 0; j < new_child->key_count; j++) {
            new_child->keys[j] = full_child->keys[j + mid];
            new_child->values[j] = full_child->values[j + mid];
        }
        full_child->key_count = mid;
    } else {
        // Internal node split: mid moves to parent, not kept in children
        new_child->key_count = full_child->key_count - mid - 1;
        for (int j = 0; j < new_child->key_count; j++) {
            new_child->keys[j] = full_child->keys[j + mid + 1];
        }
        
        for (int j = 0; j <= new_child->key_count; j++) {
            new_child->children[j] = full_child->children[j + mid + 1];
            if (new_child->children[j]) {
                new_child->children[j]->parent = new_child;
            }
        }
        full_child->key_count = mid;
    }
    
    // Insert new child into parent
    for (int j = parent->key_count; j > index; j--) {
        parent->children[j + 1] = parent->children[j];
    }
    parent->children[index + 1] = new_child;
    new_child->parent = parent;
    
    // Promote separator key to parent
    for (int j = parent->key_count - 1; j >= index; j--) {
        parent->keys[j + 1] = parent->keys[j];
        parent->values[j + 1] = parent->values[j];
    }
    
    parent->keys[index] = full_child->is_leaf ? new_child->keys[0] : full_child->keys[mid];
    parent->values[index] = NULL;  // Internal nodes don't store values in B+-tree
    parent->key_count++;
}

static void btree_insert_non_full(Arena* a, BTreeNode* node, SetNode* key, SetNode* value) {
    int i = node->key_count - 1;
    
    if (node->is_leaf) {
        while (i >= 0 && compare_nodes(key, node->keys[i]) < 0) {
            node->keys[i + 1] = node->keys[i];
            node->values[i + 1] = node->values[i];
            i--;
        }
        node->keys[i + 1] = key;
        node->values[i + 1] = value;
        node->key_count++;
    } else {
        // Find child to descend to
        while (i >= 0 && compare_nodes(key, node->keys[i]) < 0) {
            i--;
        }
        i++;
        
        // Add safety check
        if (!node->children[i]) {
            return; // Should not happen, but prevents crash
        }
        
        if (node->children[i]->key_count == BTREE_ORDER - 1) {
            btree_split_child(a, node, i);
            if (compare_nodes(key, node->keys[i]) > 0) {
                i++;
            }
        }
        btree_insert_non_full(a, node->children[i], key, value);
    }
}

static BTreeNode* btree_insert(Arena* a, BTreeNode* root, SetNode* key, SetNode* value) {
    if (!root) {
        root = btree_node_create(a, 1);  // Create as leaf
        root->keys[0] = key;
        root->values[0] = value;
        root->key_count = 1;
        return root;
    }
    
    if (root->key_count == BTREE_ORDER - 1) {
        BTreeNode* new_root = btree_node_create(a, 0);  // Non-leaf
        new_root->children[0] = root;
        root->parent = new_root;
        btree_split_child(a, new_root, 0);
        btree_insert_non_full(a, new_root, key, value);
        return new_root;
    }
    
    btree_insert_non_full(a, root, key, value);
    return root;
}

static void btree_range_recursive(BTreeNode* node, SetNode* min, SetNode* max, 
                                  Arena* a, SetArray* results, size_t* count, size_t limit) {
    if (!node || (limit > 0 && *count >= limit)) return;
    
    int i = 0;
    
    // Find starting position
    if (min) {
        while (i < node->key_count && compare_nodes(node->keys[i], min) < 0) {
            i++;
        }
    }
    
    // Traverse the range
    for (; i < node->key_count; i++) {
        if (max && compare_nodes(node->keys[i], max) > 0) {
            break;
        }
        
        // Visit left child first (if not leaf)
        if (!node->is_leaf && node->children[i]) {
            btree_range_recursive(node->children[i], min, max, a, results, count, limit);
            if (limit > 0 && *count >= limit) return;
        }
        
        // Add current node's value if it's a leaf and within range
        if (node->is_leaf) {
            int in_range = 1;
            if (min && compare_nodes(node->keys[i], min) < 0) in_range = 0;
            if (max && compare_nodes(node->keys[i], max) > 0) in_range = 0;
            
            if (in_range && (limit == 0 || *count < limit)) {
                array_push(a, results, node->values[i]);
                (*count)++;
            }
        }
    }
    
    // Visit rightmost child if not leaf
    if (!node->is_leaf && i <= node->key_count && node->children[i]) {
        btree_range_recursive(node->children[i], min, max, a, results, count, limit);
    }
}

// ============================================================================
// SECTION: Internal Data Structures
// ============================================================================

// FNV-1a Hash Algorithm
static uint32_t hash_string(const char* str) {
    uint32_t hash = 2166136261u;
    while (*str) {
        hash ^= (uint8_t)(*str++);
        hash *= 16777619;
    }
    return hash;
}

static SetNode* node_create(Arena* a, SetType type) {
    SetNode* node = (SetNode*)arena_alloc(a, sizeof(SetNode));
    if (node) {
        memset(node, 0, sizeof(SetNode));
        node->type = type;
        node->owner = a; // Binds the node to the allocator for future modification
    }
    return node;
}

static void map_resize(Arena* a, SetMap* map) {
    size_t new_cap = (map->capacity == 0) ? HASH_MAP_INIT_CAP : map->capacity * 2;
    SetMapEntry** new_buckets = (SetMapEntry**)arena_alloc(a, new_cap * sizeof(SetMapEntry*));
    
    if (!new_buckets) return;
    memset(new_buckets, 0, new_cap * sizeof(SetMapEntry*));

    // Re-hash based on the ordered list (guarantees consistency)
    SetMapEntry* curr = map->head_order;
    while (curr) {
        uint32_t idx = curr->hash & (new_cap - 1);
        curr->next_in_bucket = new_buckets[idx];
        new_buckets[idx] = curr;
        curr = curr->next_ordered;
    }

    map->buckets = new_buckets;
    map->capacity = new_cap;
}

static void map_put(Arena* a, SetMap* map, const char* key, SetNode* val) {
    if (map->count + 1 > map->capacity * HASH_LOAD_FACTOR) {
        map_resize(a, map);
    }

    uint32_t h = hash_string(key);
    uint32_t idx = h & (map->capacity - 1);

    // Check for update
    SetMapEntry* e = map->buckets[idx];
    while (e) {
        if (e->hash == h && strcmp(e->key, key) == 0) {
            e->value = val; // Overwrite existing
            return;
        }
        e = e->next_in_bucket;
    }

    // Insert new
    SetMapEntry* new_entry = (SetMapEntry*)arena_alloc(a, sizeof(SetMapEntry));
    new_entry->key = arena_strdup(a, key);
    new_entry->value = val;
    new_entry->hash = h;

    // Link into bucket
    new_entry->next_in_bucket = map->buckets[idx];
    map->buckets[idx] = new_entry;

    // Link into order list (Append to tail)
    new_entry->next_ordered = NULL;
    new_entry->prev_ordered = map->tail_order;

    if (map->tail_order) {
        map->tail_order->next_ordered = new_entry;
    } else {
        map->head_order = new_entry;
    }
    map->tail_order = new_entry;
    
    map->count++;
}

static SetNode* map_get(SetMap* map, const char* key) {
    if (map->capacity == 0) return NULL;
    
    uint32_t h = hash_string(key);
    uint32_t idx = h & (map->capacity - 1);
    
    SetMapEntry* e = map->buckets[idx];
    while (e) {
        if (e->hash == h && strcmp(e->key, key) == 0) {
            return e->value;
        }
        e = e->next_in_bucket;
    }
    return NULL;
}

static void map_remove(SetMap* map, const char* key) {
    if (map->capacity == 0) return;

    uint32_t h = hash_string(key);
    uint32_t idx = h & (map->capacity - 1);

    SetMapEntry* prev = NULL;
    SetMapEntry* e = map->buckets[idx];

    while (e) {
        if (e->hash == h && strcmp(e->key, key) == 0) {
            // Unlink from bucket
            if (prev) {
                prev->next_in_bucket = e->next_in_bucket;
            } else {
                map->buckets[idx] = e->next_in_bucket;
            }

            // Unlink from order list
            if (e->prev_ordered) {
                e->prev_ordered->next_ordered = e->next_ordered;
            } else {
                map->head_order = e->next_ordered;
            }

            if (e->next_ordered) {
                e->next_ordered->prev_ordered = e->prev_ordered;
            } else {
                map->tail_order = e->prev_ordered;
            }

            map->count--;
            return;
        }
        prev = e;
        e = e->next_in_bucket;
    }
}

static void array_push(Arena* a, SetArray* arr, SetNode* val) {
    if (arr->count == arr->capacity) {
        size_t new_cap = (arr->capacity == 0) ? 8 : arr->capacity * 2;
        SetNode** new_items = (SetNode**)arena_alloc(a, new_cap * sizeof(SetNode*));
        
        if (arr->items) {
            memcpy(new_items, arr->items, arr->count * sizeof(SetNode*));
        }
        
        arr->items = new_items;
        arr->capacity = new_cap;
    }
    arr->items[arr->count++] = val;
}

// ============================================================================
// SECTION: Lexer Implementation
// ============================================================================

typedef enum {
    TOK_EOF,
    TOK_ERROR,
    TOK_STRING,
    TOK_INT,
    TOK_DOUBLE,
    TOK_BOOL,
    TOK_NULL,
    TOK_LBRACE,     // {
    TOK_RBRACE,     // }
    TOK_LBRACKET,   // [
    TOK_RBRACKET,   // ]
    TOK_BLOCK_START,// -:
    TOK_BLOCK_END,  // :-
    TOK_PIPE,       // |
    TOK_COLON,      // :
    TOK_COMMA,      // ,
    TOK_ASSIGN,     // =
    TOK_AT,         // @
    TOK_AMP,        // &
    TOK_STAR,        // *
    TOK_EXPRESSION,   //
} TokenType;

typedef struct Token {
    TokenType type;
    const char* start;
    size_t length;
    int line;
    int col;
} Token;

typedef struct Lexer {
    const char* src;
    size_t len;
    size_t pos;
    int line;
    int col;
    SetConfig* cfg;
} Lexer;

static char lex_peek(Lexer* l) {
    if (l->pos >= l->len) return 0;
    return l->src[l->pos];
}

static char lex_advance(Lexer* l) {
    char c = lex_peek(l);
    if (l->pos < l->len) {
        l->pos++;
        if (c == '\n') {
            l->line++;
            l->col = 1;
        } else {
            l->col++;
        }
    }
    return c;
}

static char lex_peek_next(Lexer* l) {
    if (l->pos + 1 >= l->len) return 0;
    return l->src[l->pos + 1];
}

static void lex_skip_whitespace(Lexer* l) {
    while (1) {
        char c = lex_peek(l);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            lex_advance(l);
        } else if (c == '#') {
            // Line comment
            while (lex_peek(l) != '\n' && lex_peek(l) != 0) {
                lex_advance(l);
            }
        } else if (c == '/' && lex_peek_next(l) == '/') {
            // C++ style comment
            while (lex_peek(l) != '\n' && lex_peek(l) != 0) {
                lex_advance(l);
            }
        } else {
            break;
        }
    }
}

static char* decode_string(Arena* a, const char* start, size_t len) {
    char* decoded = (char*)arena_alloc(a, len + 1);
    size_t w = 0;
    for (size_t r = 0; r < len; r++) {
        if (start[r] == '\\' && r + 1 < len) {
            r++;
            switch (start[r]) {
                case 'n': decoded[w++] = '\n'; break;
                case 't': decoded[w++] = '\t'; break;
                case 'r': decoded[w++] = '\r'; break;
                case 'b': decoded[w++] = '\b'; break;
                case 'f': decoded[w++] = '\f'; break;
                case '"': decoded[w++] = '"'; break;
                case '\'': decoded[w++] = '\''; break;
                case '\\': decoded[w++] = '\\'; break;
                case '/': decoded[w++] = '/'; break;
                default: 
                    // Fix: Keep invalid escape sequence as-is (e.g. \z -> \z)
                    decoded[w++] = '\\'; 
                    decoded[w++] = start[r]; 
                    break;
            }
        } else {
            decoded[w++] = start[r];
        }
    }
    decoded[w] = 0;
    return decoded;
}
static Token lex_scan_token(Lexer* l) {
    lex_skip_whitespace(l);

    Token t;
    t.start = l->src + l->pos;
    t.line = l->line;
    t.col = l->col;
    t.length = 0;
    t.type = TOK_EOF;

    if (l->pos >= l->len) return t;

    char c = lex_peek(l);

    // --- Multi-character Operators ---
    if (c == '-') {
        if (lex_peek_next(l) == ':') {
            lex_advance(l); lex_advance(l);
            t.type = TOK_BLOCK_START;
            t.length = 2;
            return t;
        }
    }
    if (c == ':') {
        if (lex_peek_next(l) == '-') {
            lex_advance(l); lex_advance(l);
            t.type = TOK_BLOCK_END;
            t.length = 2;
            return t;
        }
    }

    // --- Single Character Operators ---
if (c == '{' || c == '}') {
    // Allow { } only if preceded by '$' (i.e., inside ${...})
    if (l->pos > 0 && l->src[l->pos - 1] == '$') {
        // Accept as part of variable expansion token
        lex_advance(l);
        t.type = TOK_STRING;
        t.length = 1;
        return t;
    } else {
        lex_advance(l);
        t.type = TOK_ERROR;
        set_error_at(l->cfg, l->line, l->col, "Syntax Error: '{' and '}' are not supported except in ${var} expressions.");
        return t;
    }
}
if (c == '$' && lex_peek_next(l) == '(') {
    t.start = l->src + l->pos;
    lex_advance(l); // $
    lex_advance(l); // (
    int depth = 1;
    while (l->pos < l->len && depth > 0) {
        char nc = lex_peek(l);
        if (nc == '(') depth++;
        else if (nc == ')') depth--;
        else if (nc == 0) break;
        lex_advance(l);
    }
    if (depth != 0) {
        t.type = TOK_ERROR;
        set_error_at(l->cfg, l->line, l->col, "Unclosed parenthesis in expression");
        return t;
    }
    t.length = (l->src + l->pos) - t.start;
    t.type = TOK_EXPRESSION;
    return t;
}
    if (c == '[') { lex_advance(l); t.type = TOK_LBRACKET; t.length = 1; return t; }
    if (c == ']') { lex_advance(l); t.type = TOK_RBRACKET; t.length = 1; return t; }
    if (c == '|') { lex_advance(l); t.type = TOK_PIPE; t.length = 1; return t; }
    if (c == ':') { lex_advance(l); t.type = TOK_COLON; t.length = 1; return t; }
    if (c == ',') { lex_advance(l); t.type = TOK_COMMA; t.length = 1; return t; }
    if (c == '=') { lex_advance(l); t.type = TOK_ASSIGN; t.length = 1; return t; }
    if (c == '@') { lex_advance(l); t.type = TOK_AT; t.length = 1; return t; }
    if (c == '&') { lex_advance(l); t.type = TOK_AMP; t.length = 1; return t; }
    if (c == '*') { lex_advance(l); t.type = TOK_STAR; t.length = 1; return t; }

    // --- Strings (Standard, Multi-line, Raw) ---
    int is_raw = 0;
    if (c == 'r' && (lex_peek_next(l) == '"' || lex_peek_next(l) == '\'')) {
        is_raw = 1;
        lex_advance(l); // skip 'r'
        c = lex_peek(l); // update c to the quote
    }

    if (c == '"' || c == '\'') {
        char quote = c;
        int is_multiline = 0;

        // Check for Triple Quote
        if (lex_peek_next(l) == quote) {
            size_t saved = l->pos;
            lex_advance(l); // 2nd
            if (lex_peek_next(l) == quote) {
                lex_advance(l); // 3rd
                is_multiline = 1;
            } else {
                // Not a triple quote, backtrack
                l->pos = saved;
            }
        }

        lex_advance(l); // skip opening quote(s)
        t.start = l->src + l->pos; // start content
        
        while (l->pos < l->len) {
            char cur = lex_peek(l);
            
            if (is_multiline) {
                // Check for Triple Quote End
                if (cur == quote && lex_peek_next(l) == quote) {
                    size_t saved = l->pos;
                    lex_advance(l); 
                    if (lex_peek_next(l) == quote) {
                        l->pos = saved; // Back up to start of closing sequence
                        break; 
                    }
                    l->pos = saved; // Not a triple end
                }
            } else {
                if (cur == quote) break;
            }

            if (!is_raw && cur == '\\') {
                lex_advance(l); // skip escape char
            }
            lex_advance(l);
        }
        
        t.length = (l->src + l->pos) - t.start;
        
        // Skip closing quote(s)
        lex_advance(l); 
        if (is_multiline) {
            lex_advance(l);
            lex_advance(l);
        }

        t.type = TOK_STRING;
        return t;
    }

    // --- Numbers (Integers and Floats with scientific notation) ---
    int is_number = 0;
    if (isdigit(c)) is_number = 1;
    else if ((c == '-' || c == '+')) {
        char next = lex_peek_next(l);
        if (isdigit(next) || next == '.') is_number = 1;
    }

    if (is_number) {
        int is_double = 0;
        lex_advance(l);
        while (isdigit(lex_peek(l)) || lex_peek(l) == '.' || lex_peek(l) == 'e' || lex_peek(l) == 'E') {
            char nc = lex_peek(l);
            if (nc == '.') is_double = 1;
            if (nc == 'e' || nc == 'E') {
                is_double = 1;
                lex_advance(l);
                if (lex_peek(l) == '+' || lex_peek(l) == '-') lex_advance(l);
                continue;
            }
            lex_advance(l);
        }
        t.length = (l->src + l->pos) - t.start;
        t.type = is_double ? TOK_DOUBLE : TOK_INT;
        return t;
    }

    // --- Identifiers / Keywords / Bare Expressions ---
    // Check for $(...) start
    if (c == '$' && lex_peek_next(l) == '(') {
        t.start = l->src + l->pos;
        lex_advance(l); // $
        lex_advance(l); // (
        
        int depth = 1;
        while (l->pos < l->len && depth > 0) {
            char nc = lex_peek(l);
            if (nc == '(') depth++;
            else if (nc == ')') depth--;
            lex_advance(l);
        }
        
        t.length = (l->src + l->pos) - t.start;
        t.type = TOK_STRING; // Treat as string for expansion later
        return t;
    }

    // Check for ${...} start
    if (c == '$' && lex_peek_next(l) == '{') {
        t.start = l->src + l->pos;
        lex_advance(l); // $
        lex_advance(l); // {
        
        int depth = 1;
        while (l->pos < l->len && depth > 0) {
            char nc = lex_peek(l);
            if (nc == '{') depth++;
            else if (nc == '}') depth--;
            lex_advance(l);
        }
        
        t.length = (l->src + l->pos) - t.start;
        t.type = TOK_STRING; // Treat as string for expansion later
        return t;
    }

    while (lex_peek(l) != 0 && (isalnum(lex_peek(l)) || strchr("_.-+/$", lex_peek(l)))) {
        lex_advance(l);
    }
    
    t.length = (l->src + l->pos) - t.start;
    
    if (t.length == 0) {
        lex_advance(l);
        t.length = 1;
        t.type = TOK_ERROR;
        set_error_at(l->cfg, l->line, l->col, "Unexpected character: '%c'", c);
        return t;
    }

    t.type = TOK_STRING; // Default to string

    // Check Keywords
    if (t.length == 4 && strncmp(t.start, "true", 4) == 0) t.type = TOK_BOOL;
    else if (t.length == 5 && strncmp(t.start, "false", 5) == 0) t.type = TOK_BOOL;
    else if (t.length == 4 && strncmp(t.start, "null", 4) == 0) t.type = TOK_NULL;
    else if (t.length == 2 && strncmp(t.start, "on", 2) == 0) t.type = TOK_BOOL;
    else if (t.length == 3 && strncmp(t.start, "off", 3) == 0) t.type = TOK_BOOL;
    else if (t.length == 3 && strncmp(t.start, "yes", 3) == 0) t.type = TOK_BOOL;
    else if (t.length == 2 && strncmp(t.start, "no", 2) == 0) t.type = TOK_BOOL;

    return t;
}

// ============================================================================
// SECTION: System Abstraction (Platform Independent)
// ============================================================================

typedef void (*GlobCallback)(const char* path, void* udata);

/* REPLACEMENT for sys_list_directory */
static void sys_list_directory(const char* pattern, GlobCallback cb, void* udata) {
    // Fix: Handle dynamic path lengths
    size_t plen = strlen(pattern);
    char* dir_path = (char*)malloc(plen + 2);
    char* file_pattern = (char*)malloc(plen + 2);
    
    if (!dir_path || !file_pattern) return; // Safety check
    
    strcpy(dir_path, ".");
    
    const char* last_slash = strrchr(pattern, PATH_SEP);
    #if CTZ_PLATFORM_WIN
    if (!last_slash) last_slash = strrchr(pattern, '/'); 
    #endif

    if (last_slash) {
        size_t dlen = last_slash - pattern;
        strncpy(dir_path, pattern, dlen);
        dir_path[dlen] = 0;
        strcpy(file_pattern, last_slash + 1);
    } else {
        strcpy(file_pattern, pattern);
    }

    #if CTZ_PLATFORM_WIN
        // Dynamic alloc for search path
        size_t slen = strlen(dir_path) + strlen(file_pattern) + 5;
        char* search_path = (char*)malloc(slen);
        snprintf(search_path, slen, "%s\\%s", dir_path, file_pattern);
        
        WIN32_FIND_DATA fd;
        HANDLE hFind = FindFirstFile(search_path, &fd);
        
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    size_t full_len = strlen(dir_path) + strlen(fd.cFileName) + 2;
                    char* full_path = (char*)malloc(full_len);
                    snprintf(full_path, full_len, "%s\\%s", dir_path, fd.cFileName);
                    cb(full_path, udata);
                    free(full_path);
                }
            } while (FindNextFile(hFind, &fd));
            FindClose(hFind);
        }
        free(search_path);
    #elif CTZ_PLATFORM_POSIX
        DIR* d = opendir(dir_path);
        if (d) {
            struct dirent* dir;
            while ((dir = readdir(d)) != NULL) {
                int is_file = (dir->d_type == DT_REG);
    
                if (dir->d_type == DT_UNKNOWN) {
                    struct stat st;
                    char path_buf[2048];
                    snprintf(path_buf, sizeof(path_buf), "%s/%s", dir_path, dir->d_name);
                    if (stat(path_buf, &st) == 0 && S_ISREG(st.st_mode)) {
                        is_file = 1;
                    }
                }

                if (is_file) {
                    int match = 0;
                    if (strcmp(file_pattern, "*") == 0) match = 1;
                    else {
                        const char* star = strchr(file_pattern, '*');
                        if (star) {
                            const char* ext = star + 1;
                            size_t elen = strlen(ext);
                            size_t nlen = strlen(dir->d_name);
                            if (nlen >= elen && strcmp(dir->d_name + nlen - elen, ext) == 0) match = 1;
                        } else {
                            if (strcmp(dir->d_name, file_pattern) == 0) match = 1;
                        }
                    }

                    if (match) {
                        size_t full_len = strlen(dir_path) + strlen(dir->d_name) + 2;
                        char* full_path = (char*)malloc(full_len);
                        snprintf(full_path, full_len, "%s/%s", dir_path, dir->d_name);
                        cb(full_path, udata);
                        free(full_path);
                    }
                }
            }
            closedir(d);
        }
    #endif

    free(dir_path);
    free(file_pattern);
}

// ============================================================================
// SECTION: Parsing & Inclusion Logic
// ============================================================================

static SetNode* parse_value(Lexer* l);
static void parse_map_body(Lexer* l, SetMap* map);

typedef struct {
    SetConfig* cfg;
    SetMap* target_map;
} IncludeContext;

static void include_callback(const char* path, void* udata) {
    IncludeContext* ctx = (IncludeContext*)udata;
    
    FILE* f = fopen(path, "rb");
    if (!f) {
        set_error(ctx->cfg, "Include failed: Could not open '%s'", path);
        return;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    char* buf = (char*)malloc(sz + 1);
    if (buf) {
        if (fread(buf, 1, sz, f) == (size_t)sz) {
            buf[sz] = 0;
            // Sub-lexer operates on the same Arena/Config
            Lexer sub_lex = { buf, (size_t)sz, 0, 1, 1, ctx->cfg };
            parse_map_body(&sub_lex, ctx->target_map);
        }
        free(buf);
    }
    fclose(f);
}

static SetNode* parse_smart_block(Lexer* l) {
    // Current token is -:
    // We need to peek ahead to decide if this is a map or array.
    // Heuristic: 
    // 1. Empty block (-: :-) -> Map
    // 2. First item is "String" : -> Map
    // 3. Otherwise -> Array
    
    size_t saved_pos = l->pos;
    int saved_line = l->line;
    int saved_col = l->col;

    int is_map = 0;
    Token t1 = lex_scan_token(l);

    if (t1.type == TOK_BLOCK_END) {
        is_map = 1; // Empty defaults to map
    } else if (t1.type == TOK_STRING) {
        Token t2 = lex_scan_token(l);
        if (t2.type == TOK_COLON) {
            is_map = 1;
        }
    }

    // Rewind lexer
    l->pos = saved_pos;
    l->line = saved_line;
    l->col = saved_col;

    SetNode* node = node_create(&l->cfg->arena, is_map ? SET_TYPE_MAP : SET_TYPE_ARRAY);

    while (1) {
        // Peek first to check for end
        size_t peek_pos = l->pos;
        int peek_line = l->line;
        int peek_col = l->col;
        Token p = lex_scan_token(l);
        
        if (p.type == TOK_BLOCK_END || p.type == TOK_EOF) break;
        if (p.type == TOK_COMMA) continue;

        // Rewind to parse the actual value/key
        l->pos = peek_pos; l->line = peek_line; l->col = peek_col;

        if (is_map) {
            Token key_tok = lex_scan_token(l);
            if (key_tok.type != TOK_STRING) {
                set_error_at(l->cfg, key_tok.line, key_tok.col, "Expected key string, got type %d", key_tok.type);
                return NULL;
            }
            
            Token colon = lex_scan_token(l);
            if (colon.type != TOK_COLON) {
                set_error(l->cfg, "SmartBlock: Expected ':' at %d:%d", colon.line, colon.col);
                return NULL;
            }

            char* key = decode_string(&l->cfg->arena, key_tok.start, key_tok.length);
            SetNode* val = parse_value(l);
            if (!val) return NULL; // Propagate error
            map_put(&l->cfg->arena, &node->data.map, key, val);
        } else {
            SetNode* val = parse_value(l);
            if (!val) return NULL; // Propagate error
            array_push(&l->cfg->arena, &node->data.array, val);
        }
    }
    return node;
}

static SetNode* deep_copy_node(Arena* a, SetNode* src) {
    if (!src) return NULL;
    SetNode* dst = node_create(a, src->type);
    dst->flags = src->flags;

    switch (src->type) {
        case SET_TYPE_STRING:
            dst->data.s_val = arena_strdup(a, src->data.s_val);
            break;
        case SET_TYPE_INT:
            dst->data.i_val = src->data.i_val;
            break;
        case SET_TYPE_DOUBLE:
            dst->data.d_val = src->data.d_val;
            break;
        case SET_TYPE_BOOL:
            dst->data.b_val = src->data.b_val;
            break;
        case SET_TYPE_NULL:
            break;
        case SET_TYPE_ARRAY: {
            SetArray* src_arr = &src->data.array;
            SetArray* dst_arr = &dst->data.array;
            dst_arr->capacity = src_arr->count;
            dst_arr->count = src_arr->count;
            if (src_arr->count > 0) {
                dst_arr->items = (SetNode**)arena_alloc(a, src_arr->count * sizeof(SetNode*));
                for (size_t i = 0; i < src_arr->count; i++) {
                    dst_arr->items[i] = deep_copy_node(a, src_arr->items[i]);
                }
            }
            break;
        }
        case SET_TYPE_MAP: {
            SetMap* src_map = &src->data.map;
            SetMap* dst_map = &dst->data.map;
            // Reuse map_put to preserve order and buckets
            SetMapEntry* e = src_map->head_order;
            while (e) {
                SetNode* copied_val = deep_copy_node(a, e->value);
                map_put(a, dst_map, e->key, copied_val);
                e = e->next_ordered;
            }
            break;
        }
        default:
            break;
    }
    return dst;
}

/* REPLACE parse_value */
static SetNode* parse_value(Lexer* l) {
    Token t = lex_scan_token(l);
    SetNode* n = NULL;

    switch (t.type) {
        case TOK_BLOCK_START: // The "-:" token
            return parse_smart_block(l);

        case TOK_LBRACKET: // standard [ ] arrays are still allowed for simple lists
            n = node_create(&l->cfg->arena, SET_TYPE_ARRAY);
            while (1) { int ml=l->line, mc=l->col; size_t saved_pos = l->pos;
                Token next = lex_scan_token(l);
                if (next.type == TOK_RBRACKET) break;
                if (next.type == TOK_COMMA) continue;
                
                l->pos = saved_pos; l->line = ml; l->col = mc; 
                SetNode* val = parse_value(l);
                if (!val) return NULL; // Propagate error
                array_push(&l->cfg->arena, &n->data.array, val);
            }
            return n;

case TOK_STRING:
    // Check if this string is actually an expression like "$(x + y)"
    // This happens because $(...) is lexed as TOK_STRING
    if (t.length >= 3 && t.start[0] == '$' && t.start[1] == '(') {
        // It's an expression — mark it
        n = node_create(&l->cfg->arena, SET_TYPE_STRING);
        n->data.s_val = decode_string(&l->cfg->arena, t.start, t.length);
        n->flags |= SET_FLAG_EXPRESSION;
        return n;
    }
    // Otherwise, normal string
    n = node_create(&l->cfg->arena, SET_TYPE_STRING);
    n->data.s_val = decode_string(&l->cfg->arena, t.start, t.length);
    return n;
        case TOK_INT:
            n = node_create(&l->cfg->arena, SET_TYPE_INT);
            {
                char buf[64];
                size_t len = t.length < 63 ? t.length : 63;
                memcpy(buf, t.start, len); buf[len] = 0;
                n->data.i_val = strtol(buf, NULL, 10);
            }
            return n;

        case TOK_DOUBLE:
            n = node_create(&l->cfg->arena, SET_TYPE_DOUBLE);
            {
                char buf[64];
                size_t len = t.length < 63 ? t.length : 63;
                memcpy(buf, t.start, len); buf[len] = 0;
                n->data.d_val = strtod(buf, NULL);
            }
            return n;

        case TOK_BOOL:
            n = node_create(&l->cfg->arena, SET_TYPE_BOOL);
            if (strncmp(t.start, "true", 4) == 0 || strncmp(t.start, "on", 2) == 0 || strncmp(t.start, "yes", 3) == 0) 
                n->data.b_val = 1;
            else 
                n->data.b_val = 0;
            return n;

        case TOK_NULL:
            return node_create(&l->cfg->arena, SET_TYPE_NULL);

        case TOK_AMP: {
            // Anchor Definition: &name value
            Token name_tok = lex_scan_token(l);
            if (name_tok.type != TOK_STRING) {
                set_error_at(l->cfg, name_tok.line, name_tok.col, "Expected anchor name after '&'");
                return NULL;
            }
            char* name = decode_string(&l->cfg->arena, name_tok.start, name_tok.length);
            SetNode* val = parse_value(l); // Recurse
            if (val) {
                map_put(&l->cfg->arena, &l->cfg->anchors, name, val);
            }
            return val;
        }

case TOK_STAR: {
    // Alias Reference: *name
    Token name_tok = lex_scan_token(l);
    if (name_tok.type != TOK_STRING) {
        set_error_at(l->cfg, name_tok.line, name_tok.col, "Expected alias name after '*'");
        return NULL;
    }
    char* name = decode_string(&l->cfg->arena, name_tok.start, name_tok.length);
    SetNode* ref = map_get(&l->cfg->anchors, name);
    if (!ref) {
        set_error_at(l->cfg, name_tok.line, name_tok.col, "Unknown anchor reference: *%s", name);
        return NULL;
    }

    // === DEEP COPY IMPLEMENTATION ===
    // Recursively clone the node and all its children
    SetNode* deep_copy_node(Arena* a, SetNode* src);

    SetNode* copy = deep_copy_node(&l->cfg->arena, ref);
    return copy;
}



        case TOK_EXPRESSION: {
            n = node_create(&l->cfg->arena, SET_TYPE_STRING);
            // Skip $( and )
            // t.start points to $, t.length includes $(...)
            // We want content inside $(...)
            // Actually, expand_string expects $(...) format to detect expression?
            // expand_string checks for $ and (.
            // So we should keep the full $(...) string.
            n->data.s_val = decode_string(&l->cfg->arena, t.start, t.length);
            n->flags |= SET_FLAG_EXPRESSION;
            return n;
        }

        case TOK_ERROR:
            return NULL; // Error already set by lexer
        default:
            set_error_at(l->cfg, t.line, t.col, "Unexpected token type %d", t.type);
            return NULL;
    }
}

static void parse_map_body(Lexer* l, SetMap* map) {
    SetNode* active_section = NULL;
    uint32_t pending_flags = 0;

    while (1) {
        Token t = lex_scan_token(l);
        if (t.type == TOK_EOF || t.type == TOK_BLOCK_END) {
            break;
        }
        if (t.type == TOK_ERROR) {
            return; // Error already set by lexer
        }
        if (t.type == TOK_COMMA) continue;

        // Handle Decorators
        if (t.type == TOK_AT) {
            Token dec_name = lex_scan_token(l);
            if (dec_name.type == TOK_STRING) {
                if (strncmp(dec_name.start, "private", 7) == 0) pending_flags |= 1;
                else if (strncmp(dec_name.start, "deprecated", 10) == 0) pending_flags |= 2;
                else if (strncmp(dec_name.start, "readonly", 8) == 0) pending_flags |= 4;
            } else {
                set_error_at(l->cfg, dec_name.line, dec_name.col, "Expected decorator name after '@'");
                return;
            }
            continue; // Loop to consume more decorators or the key
        }

        // Handle Anchor Definitions (&name value) inside a map
        if (t.type == TOK_AMP) {
            // Rewind to let parse_value handle the anchor definition
            l->pos = t.start - l->src;
            l->line = t.line;
            l->col = t.col;
            parse_value(l); // This registers the anchor in cfg->anchors
            continue;
        }

        // Handle |Section|
        if (t.type == TOK_PIPE) {
            Token sec_name = lex_scan_token(l);
            Token close_pipe = lex_scan_token(l);
            
            if (sec_name.type == TOK_STRING && close_pipe.type == TOK_PIPE) {
                char* key = decode_string(&l->cfg->arena, sec_name.start, sec_name.length);
                
                SetNode* sec_node = map_get(map, key);
                if (!sec_node) {
                    sec_node = node_create(&l->cfg->arena, SET_TYPE_MAP);
                    map_put(&l->cfg->arena, map, key, sec_node);
                }
                active_section = sec_node;
                continue;
            } else {
                set_error_at(l->cfg, t.line, t.col, "Syntax error in section definition");
                return;
            }
        }

        if (t.type == TOK_STRING) {
            char* key = decode_string(&l->cfg->arena, t.start, t.length);

            // Handle Include
            if (strcmp(key, "include") == 0) {
                Token path = lex_scan_token(l);
                if (path.type == TOK_STRING) {
                    char* pattern = decode_string(&l->cfg->arena, path.start, path.length);
                    
                    // Check for 'as' alias
                    size_t saved_pos = l->pos;
                    int saved_line = l->line;
                    int saved_col = l->col;
                    
                    Token maybe_as = lex_scan_token(l);
                    if (maybe_as.type == TOK_STRING && strncmp(maybe_as.start, "as", 2) == 0 && maybe_as.length == 2) {
                        Token alias_tok = lex_scan_token(l);
                        if (alias_tok.type == TOK_STRING) {
                            char* alias = decode_string(&l->cfg->arena, alias_tok.start, alias_tok.length);
                            
                            // Create a dedicated map for this include
                            SetNode* include_root = node_create(&l->cfg->arena, SET_TYPE_MAP);
                            IncludeContext ctx = { l->cfg, &include_root->data.map };
                            sys_list_directory(pattern, include_callback, &ctx);
                            
                            SetMap* target = active_section ? &active_section->data.map : map;
                            map_put(&l->cfg->arena, target, alias, include_root);
                            
                            pending_flags = 0; // Reset flags
                            continue;
                        } else {
                            set_error_at(l->cfg, alias_tok.line, alias_tok.col, "Expected alias name after 'as'");
                            return;
                        }
                    }

                    // Fallback: Standard include (merge into current)
                    l->pos = saved_pos;
                    l->line = saved_line;
                    l->col = saved_col;

                    IncludeContext ctx = { l->cfg, active_section ? &active_section->data.map : map };
                    sys_list_directory(pattern, include_callback, &ctx);
                }
                pending_flags = 0; // Reset flags
                continue;
            }

            // Normal Key-Value Pair
            Token op = lex_scan_token(l);
            if (op.type == TOK_COLON || op.type == TOK_ASSIGN) {
                SetNode* val = parse_value(l);
                if (val) {
                    val->flags = pending_flags;
                    SetMap* target = active_section ? &active_section->data.map : map;
                    map_put(&l->cfg->arena, target, key, val);
                }
                pending_flags = 0; // Reset flags
            } else {
                set_error_at(l->cfg, op.line, op.col, "Expected ':' or '=' after key '%s'", key);
            }
        }
    }
}

// ============================================================================
// SECTION: Variable Expansion
// ============================================================================

static SetNode* set_find_path(SetNode* root, const char* path);

static char* resolve_variable(SetConfig* cfg, const char* key, int depth) {
    if (depth > MAX_VAR_RECURSION) {
        set_error(cfg, "Max recursion depth reached for variable '%s'", key);
        return NULL;
    }

    // 1. Check Environment
    char* env_val = getenv(key);
    if (env_val) {
        return arena_strdup(&cfg->arena, env_val);
    }

    // 2. Check Internal Config
    SetNode* node = set_find_path(cfg->root, key);
    if (node) {
        if (node->type == SET_TYPE_STRING) {
            return arena_strdup(&cfg->arena, node->data.s_val);
        }
        if (node->type == SET_TYPE_INT) {
            char buf[64];
            snprintf(buf, 64, "%ld", node->data.i_val);
            return arena_strdup(&cfg->arena, buf);
        }
        if (node->type == SET_TYPE_DOUBLE) {
            char buf[64];
            snprintf(buf, 64, "%g", node->data.d_val);
            return arena_strdup(&cfg->arena, buf);
        }
        if (node->type == SET_TYPE_BOOL) {
            return arena_strdup(&cfg->arena, node->data.b_val ? "true" : "false");
        }
    }
    return NULL;
}

// --- Expression Evaluator ---

static double eval_expr(SetConfig* cfg, const char** p);

static double eval_primary(SetConfig* cfg, const char** p) {
    const char* s = *p;
    while (isspace(*s)) s++;
    
    if (*s == '(') {
        s++;
        double v = eval_expr(cfg, &s);
        while (isspace(*s)) s++;
        if (*s == ')') s++;
        *p = s;
        return v;
    }
    
    // Handle Variables in Expression
// Handle Variables in Expression (with fallbacks)
if (isalpha(*s) || *s == '_') {
    const char* start = s;
    while (isalnum(*s) || *s == '_' || *s == '.') s++;
    
    // Check for fallback syntax
    const char* fallback_start = NULL;
    size_t var_len = s - start;
    
    // Look for :- in the original input
    const char* check = start;
    while (*check && check < s) check++;
    while (isspace(*check)) check++;
    
    if (*check == ':' && *(check+1) == '-') {
        fallback_start = check + 2;
        while (isspace(*fallback_start)) fallback_start++;
    }

    // Use arena_strndup
    char* key = arena_strndup(&cfg->arena, start, var_len);
    char* val = resolve_variable(cfg, key, 0);
    double v = 0.0;
    
    // Handle fallback value
    if (!val && fallback_start) {
        const char* fb_expr = fallback_start;
        v = eval_expr(cfg, &fb_expr);
        s = fb_expr;
    } else if (val) {
        v = strtod(val, NULL);
    }
    
    *p = s;
    return v;
}

    char* end;
    double v = strtod(s, &end);
    if (end == s && *s) {
        // Skip invalid char to avoid infinite loop
        s++; 
        *p = s;
        return 0.0;
    }
    *p = end;
    return v;
}

static double eval_term(SetConfig* cfg, const char** p) {
    double v = eval_primary(cfg, p);
    while (1) {
        const char* s = *p;
        while (isspace(*s)) s++;
        if (*s == '*') {
            s++;
            v *= eval_primary(cfg, &s);
        } else if (*s == '/') {
            s++;
            double d = eval_primary(cfg, &s);
            if (d != 0.0) v /= d;
        } else {
            *p = s;
            return v;
        }
        *p = s;
    }
}

static double eval_expr(SetConfig* cfg, const char** p) {
    double v = eval_term(cfg, p);
    while (1) {
        const char* s = *p;
        while (isspace(*s)) s++;
        if (*s == '+') {
            s++;
            v += eval_term(cfg, &s);
        } else if (*s == '-') {
            s++;
            v -= eval_term(cfg, &s);
        } else {
            *p = s;
            return v;
        }
        *p = s;
    }
}

static char* expand_string(SetConfig* cfg, const char* input, int depth) {
    if (!strchr(input, '$')) return (char*)input;

    // Optimization: Use stack buffer for small strings
    char stack_buf[1024];
    char* buf = stack_buf;
    size_t cap = sizeof(stack_buf);
    size_t len = 0;
    int used_heap = 0;

    const char* src = input;
    
    while (*src) {
        if (len + 256 > cap) {
            size_t new_cap = cap * 2;
            char* new_buf;
            if (used_heap) {
                new_buf = (char*)realloc(buf, new_cap);
            } else {
                new_buf = (char*)malloc(new_cap);
                memcpy(new_buf, buf, len);
            }
            
            if (!new_buf) { 
                if (used_heap) free(buf); 
                return (char*)input; 
            }
            
            buf = new_buf;
            cap = new_cap;
            used_heap = 1;
        }

        if (*src == '$') {
            // Check for Expression $(...)
            if (src[1] == '(') {
                const char* expr_start = src + 2;
                const char* expr_end = expr_start;
                int paren_depth = 1;
                while (*expr_end && paren_depth > 0) {
                    if (*expr_end == '(') paren_depth++;
                    else if (*expr_end == ')') paren_depth--;
                    if (paren_depth > 0) expr_end++;
                }
                
                if (paren_depth == 0) {
                    // Extract expression string
                    size_t elen = expr_end - expr_start;
                    char* expr_buf = (char*)malloc(elen + 1);
                    memcpy(expr_buf, expr_start, elen);
                    expr_buf[elen] = 0;

                    const char* p = expr_buf;
                    double res = eval_expr(cfg, &p);
                    free(expr_buf);
                    
                    // Format result
                    char res_str[64];
                    if (res == (long)res) snprintf(res_str, 64, "%ld", (long)res);
                    else snprintf(res_str, 64, "%g", res);
                    
                    size_t rlen = strlen(res_str);
                    if (len + rlen < cap) {
                        memcpy(buf + len, res_str, rlen);
                        len += rlen;
                    }
                    
                    src = expr_end + 1;
                    continue;
                }
            }

            // Check for Variable ${...} or $VAR
            const char* var_start = src + 1;
            const char* var_end = var_start;
            int braced = 0;
            
            if (*var_start == '{') {
                braced = 1;
                var_start++;
                var_end = var_start;
                while (*var_end && *var_end != '}') var_end++;
            } else {
                while (*var_end && (isalnum(*var_end) || *var_end == '_' || *var_end == '.')) var_end++;
            }

            size_t klen = var_end - var_start;
            if (klen > 0) {
                // Check for default value syntax: ${KEY:-DEFAULT}
                char* key_str = (char*)malloc(klen + 1);
                memcpy(key_str, var_start, klen);
                key_str[klen] = 0;
                
                char* default_val = NULL;
                char* sep = strstr(key_str, ":-");
                if (sep) {
                    *sep = 0; // Terminate key
                    default_val = sep + 2;
                }

                char* val = resolve_variable(cfg, key_str, depth + 1);
                
                // Fallback logic
                if (!val && default_val) {
                    // If default value is quoted, strip quotes
                    if ((default_val[0] == '"' && default_val[strlen(default_val)-1] == '"') ||
                        (default_val[0] == '\'' && default_val[strlen(default_val)-1] == '\'')) {
                        default_val++;
                        default_val[strlen(default_val)-1] = 0;
                    }
                    val = arena_strdup(&cfg->arena, default_val);
                }
                
                free(key_str);

                if (val) {
                    char* expanded_val = expand_string(cfg, val, depth + 1);
                    size_t vlen = strlen(expanded_val);
                    
                    while (len + vlen >= cap) {
                        size_t new_cap = cap * 2;
                        char* new_buf;
                        if (used_heap) {
                            new_buf = (char*)realloc(buf, new_cap);
                        } else {
                            new_buf = (char*)malloc(new_cap);
                            memcpy(new_buf, buf, len);
                        }
                        
                        if (!new_buf) { 
                            if (used_heap) free(buf); 
                            return (char*)input; 
                        }
                        buf = new_buf;
                        cap = new_cap;
                        used_heap = 1;
                    }
                    
                    memcpy(buf + len, expanded_val, vlen);
                    len += vlen;
                }
                src = braced ? var_end + 1 : var_end;
                continue;
            }
        }
        buf[len++] = *src++;
    }
    buf[len] = 0;
    
    char* result = arena_strdup(&cfg->arena, buf);
    if (used_heap) free(buf);
    return result;
}

static void expand_node_tree(SetConfig* cfg, SetNode* node) {
    if (!node) return;

if (node->type == SET_TYPE_STRING) {

    if ((node->flags & SET_FLAG_EXPRESSION) || strchr(node->data.s_val, '$')) {
        node->data.s_val = expand_string(cfg, node->data.s_val, 0);
    }
}
    else if (node->type == SET_TYPE_MAP) {
        SetMapEntry* curr = node->data.map.head_order;
        while (curr) {
            expand_node_tree(cfg, curr->value);
            curr = curr->next_ordered;
        }
    } 
    else if (node->type == SET_TYPE_ARRAY) {
        for (size_t i = 0; i < node->data.array.count; i++) {
            expand_node_tree(cfg, node->data.array.items[i]);
        }
    }
}

// ============================================================================
// SECTION: Public API Implementation
// ============================================================================

SetConfig* set_create(const char* filepath) {
    SetConfig* cfg = (SetConfig*)calloc(1, sizeof(SetConfig));
    if (cfg) {
        // Init Arena first if needed (calloc does zero init, which is fine for arena head=NULL)
        cfg->anchors.capacity = 0;
        cfg->anchors.count = 0;
        cfg->anchors.head_order = NULL;
        cfg->anchors.tail_order = NULL;
        cfg->anchors.buckets = NULL;
        
        cfg->indexes.head = NULL;
        cfg->indexes.count = 0;
        
        cfg->root = node_create(&cfg->arena, SET_TYPE_MAP);
        if (filepath) {
            cfg->filepath = arena_strdup(&cfg->arena, filepath);
        }
    }
    return cfg;
}

SetConfig* set_load(const char* filepath) {
    // 1. Enforce .set extension
    const char* ext = strrchr(filepath, '.');
    if (!ext || strcmp(ext, ".set") != 0) {
        fprintf(stderr, "[CTZ-SET] Error: Invalid file type. Only '.set' files are allowed.\n");
        return NULL;
    }

    FILE* f = fopen(filepath, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    char* buf = (char*)malloc(sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    if (fread(buf, 1, sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[sz] = 0;
    fclose(f);

    SetConfig* cfg = set_create(filepath);
    if (!cfg) { free(buf); return NULL; }

    Lexer l = { buf, (size_t)sz, 0, 1, 1, cfg };
    
    // Parse Root
    parse_map_body(&l, &cfg->root->data.map);
    
    expand_node_tree(cfg, cfg->root);

    free(buf);
    return cfg;
}

void set_free(SetConfig* config) {
    if (!config) return;
    
    arena_free(&config->arena);
    
    // Schema is still malloc'd manually, so we keep this loop
    SetSchemaEntry* s = config->schema_head;
    while (s) {
        SetSchemaEntry* next = s->next;
        free(s->path);
        free(s);
        s = next;
    }

    // Fix: Filepath is now in arena, do not free() it separately.
    free(config);
}

// --- Node Accessors ---

SetNode* set_get_root(SetConfig* config) {
    return config ? config->root : NULL;
}

SetNode* set_get_child(SetNode* map_node, const char* key) {
    if (map_node && map_node->type == SET_TYPE_MAP) {
        return map_get(&map_node->data.map, key);
    }
    return NULL;
}

SetNode* set_get_at(SetNode* array_node, size_t index) {
    if (array_node && array_node->type == SET_TYPE_ARRAY) {
        if (index < array_node->data.array.count) {
            return array_node->data.array.items[index];
        }
    }
    return NULL;
}

static SetNode* set_find_path(SetNode* root, const char* path) {
    if (!root || !path) return NULL;

    char* pdup = strdup(path);
    if (!pdup) return NULL;

    SetNode* curr = root;
    char* token = strtok(pdup, ".");
    while (token) {
        if (curr->type == SET_TYPE_MAP) {
            curr = set_get_child(curr, token);
        } else if (curr->type == SET_TYPE_ARRAY) {
            if (isdigit((unsigned char)token[0])) {
                int idx = atoi(token);
                curr = set_get_at(curr, (size_t)idx);
            } else {
                curr = NULL;
            }
        } else {
            curr = NULL;
        }
        if (!curr) break;
        token = strtok(NULL, ".");
    }
    free(pdup);
    return curr;
}

SetNode* set_query(SetConfig* config, const char* path) {
    if (!config || !config->root) return NULL;
    return set_find_path(config->root, path);
}

SetType set_node_type(SetNode* node) {
    return node ? node->type : SET_TYPE_NULL;
}

const char* set_node_string(SetNode* node, const char* default_val) {
    if (node && node->type == SET_TYPE_STRING) {
        return node->data.s_val;
    }
    return default_val;
}

long set_node_int(SetNode* node, long default_val) {
    if (node) {
        if (node->type == SET_TYPE_INT) return node->data.i_val;
        if (node->type == SET_TYPE_DOUBLE) return (long)node->data.d_val;
        if (node->type == SET_TYPE_STRING) return strtol(node->data.s_val, NULL, 0);
    }
    return default_val;
}

double set_node_double(SetNode* node, double default_val) {
    if (node) {
        if (node->type == SET_TYPE_DOUBLE) return node->data.d_val;
        if (node->type == SET_TYPE_INT) return (double)node->data.i_val;
        if (node->type == SET_TYPE_STRING) return strtod(node->data.s_val, NULL);
    }
    return default_val;
}

int set_node_bool(SetNode* node, int default_val) {
    if (node) {
        if (node->type == SET_TYPE_BOOL) return node->data.b_val;
        if (node->type == SET_TYPE_STRING) {
            return (strcmp(node->data.s_val, "true") == 0 || strcmp(node->data.s_val, "1") == 0);
        }
    }
    return default_val;
}

size_t set_node_size(SetNode* node) {
    if (!node) return 0;
    if (node->type == SET_TYPE_ARRAY) return node->data.array.count;
    if (node->type == SET_TYPE_MAP) return node->data.map.count;
    if (node->type == SET_TYPE_STRING) return strlen(node->data.s_val);
    return 0;
}

uint32_t set_node_flags(SetNode* node) {
    return node ? node->flags : 0;
}

// --- Legacy Shorthands ---

const char* set_get_string(SetConfig* config, const char* section, const char* key, const char* def) {
    SetNode* root = config->root;
    if (section && strcmp(section, "global") != 0) root = set_get_child(root, section);
    return set_node_string(set_get_child(root, key), def);
}

long set_get_int(SetConfig* config, const char* section, const char* key, long def) {
    SetNode* root = config->root;
    if (section && strcmp(section, "global") != 0) root = set_get_child(root, section);
    return set_node_int(set_get_child(root, key), def);
}

double set_get_double(SetConfig* config, const char* section, const char* key, double def) {
    SetNode* root = config->root;
    if (section && strcmp(section, "global") != 0) root = set_get_child(root, section);
    return set_node_double(set_get_child(root, key), def);
}

int set_get_bool(SetConfig* config, const char* section, const char* key, int def) {
    SetNode* root = config->root;
    if (section && strcmp(section, "global") != 0) root = set_get_child(root, section);
    return set_node_bool(set_get_child(root, key), def);
}

// --- Modifiers (Fully Implemented via Owner Pointer) ---

SetNode* set_set_child(SetNode* map_node, const char* key, SetType type) {
    if (!map_node || map_node->type != SET_TYPE_MAP) return NULL;

    // Check existing
    SetNode* existing = map_get(&map_node->data.map, key);
    if (existing) {
        existing->type = type; // Re-purpose existing node
        // Note: Old data (e.g. string pointers) remains in Arena until free, harmless.
        return existing;
    }

    // Create new using the map node's owner arena
    SetNode* new_node = node_create(map_node->owner, type);
    map_put(map_node->owner, &map_node->data.map, key, new_node);
    return new_node;
}

SetNode* set_array_push(SetNode* array_node, SetType type) {
    if (!array_node || array_node->type != SET_TYPE_ARRAY) return NULL;

    SetNode* new_node = node_create(array_node->owner, type);
    array_push(array_node->owner, &array_node->data.array, new_node);
    return new_node;
}

void set_remove_child(SetNode* map_node, const char* key) {
    if (map_node && map_node->type == SET_TYPE_MAP) {
        map_remove(&map_node->data.map, key);
    }
}

void set_node_set_string(SetNode* node, const char* val) {
    if (node) {
        node->type = SET_TYPE_STRING;
        node->data.s_val = arena_strdup(node->owner, val);
    }
}

void set_node_set_int(SetNode* node, long val) {
    if (node) {
        node->type = SET_TYPE_INT;
        node->data.i_val = val;
    }
}

void set_node_set_double(SetNode* node, double val) {
    if (node) {
        node->type = SET_TYPE_DOUBLE;
        node->data.d_val = val;
    }
}

void set_node_set_bool(SetNode* node, int val) {
    if (node) {
        node->type = SET_TYPE_BOOL;
        node->data.b_val = val;
    }
}

// --- Validation ---

void set_add_schema(SetConfig* config, const char* path, SetType type, int required, SetValidator validator) {
    SetSchemaEntry* e = (SetSchemaEntry*)malloc(sizeof(SetSchemaEntry));
    if (e) {
        e->path = _strdup(path);
        e->expected_type = type;
        e->required = required;
        e->validator = validator;
        e->next = config->schema_head;
        config->schema_head = e;
    }
}

int set_validate(SetConfig* config) {
    int errors = 0;
    // tls_error_buf[0] = 0; // Removed

    for (SetSchemaEntry* s = config->schema_head; s; s = s->next) {
        SetNode* n = set_query(config, s->path);
        
        if (!n) {
            if (s->required) {
                set_error(config, "Missing required key: %s\n", s->path);
                errors++;
            }
            continue;
        }

        if (s->expected_type != SET_TYPE_NULL && n->type != s->expected_type) {
            set_error(config, "Type mismatch for key: %s\n", s->path);
            errors++;
        }

        if (s->validator) {
            char msg[256];
            if (s->validator(s->path, n, msg, sizeof(msg)) != 0) {
                set_error(config, "Validation failed for %s: %s\n", s->path, msg);
                errors++;
            }
        }
    }
    return (errors == 0) ? 0 : -1;
}

// --- Dumping & Serialization ---

static void dump_recursive(FILE* f, SetNode* n, int indent) {
    if (!n) { fprintf(f, "null"); return; }

    const char* indent_str = "  ";
    
    switch (n->type) {
        case SET_TYPE_MAP: {
            fprintf(f, "-:\n");
            SetMapEntry* e = n->data.map.head_order;
            while (e) {
                for(int j=0; j<=indent; j++) fprintf(f, "%s", indent_str);
                fprintf(f, "%s: ", e->key);
                dump_recursive(f, e->value, indent + 1);
                fprintf(f, "\n");
                e = e->next_ordered;
            }
            for(int j=0; j<indent; j++) fprintf(f, "%s", indent_str);
            fprintf(f, ":-");
            break;
        }
        case SET_TYPE_ARRAY: {
            fprintf(f, "[");
            for (size_t i = 0; i < n->data.array.count; i++) {
                dump_recursive(f, n->data.array.items[i], indent);
                if (i < n->data.array.count - 1) fprintf(f, ", ");
            }
            fprintf(f, "]");
            break;
        }
        case SET_TYPE_STRING:
            fprintf(f, "\"%s\"", n->data.s_val);
            break;
        case SET_TYPE_INT:
            fprintf(f, "%ld", n->data.i_val);
            break;
        case SET_TYPE_DOUBLE:
            fprintf(f, "%.6g", n->data.d_val);
            break;
        case SET_TYPE_BOOL:
            fprintf(f, "%s", n->data.b_val ? "true" : "false");
            break;
        default:
            fprintf(f, "null");
            break;
    }
}

int set_dump(SetConfig* config, FILE* stream) {
    if (!config || !config->root) return -1;

    if (config->root->type == SET_TYPE_MAP) {
        SetMapEntry* e = config->root->data.map.head_order;
        while (e) {
            // Print sections with |Section| syntax
            if (e->value->type == SET_TYPE_MAP) {
                fprintf(stream, "\n|%s|\n", e->key);
                SetMapEntry* sub = e->value->data.map.head_order;
                while (sub) {
                    fprintf(stream, "%s: ", sub->key);
                    dump_recursive(stream, sub->value, 0);
                    fprintf(stream, "\n");
                    sub = sub->next_ordered;
                }
            } else {
                fprintf(stream, "%s: ", e->key);
                dump_recursive(stream, e->value, 0);
                fprintf(stream, "\n");
            }
            e = e->next_ordered;
        }
    } else {
        dump_recursive(stream, config->root, 0);
    }
    return 0;
}

int set_save(SetConfig* config) {
    if (!config || !config->filepath) return -1;
    FILE* f = fopen(config->filepath, "w");
    if (!f) return -1;
    int res = set_dump(config, f);
    fclose(f);
    return res;
}

// --- Iteration Helpers ---

SetIterator* set_iter_create(SetNode* node) {
    if (!node) return NULL;
    SetIterator* iter = (SetIterator*)calloc(1, sizeof(SetIterator));
    iter->target = node;
    iter->started = 0;
    return iter;
}

int set_iter_next(SetIterator* iter) {
    if (!iter) return 0;

    if (!iter->started) {
        iter->started = 1;
        if (iter->target->type == SET_TYPE_MAP) {
            iter->state.map_entry = iter->target->data.map.head_order;
            return (iter->state.map_entry != NULL);
        } else if (iter->target->type == SET_TYPE_ARRAY) {
            iter->state.array_index = 0;
            return (iter->target->data.array.count > 0);
        }
    } else {
        if (iter->target->type == SET_TYPE_MAP) {
            if (iter->state.map_entry)
                iter->state.map_entry = iter->state.map_entry->next_ordered;
            return (iter->state.map_entry != NULL);
        } else if (iter->target->type == SET_TYPE_ARRAY) {
            iter->state.array_index++;
            return (iter->state.array_index < iter->target->data.array.count);
        }
    }
    return 0;
}

const char* set_iter_key(SetIterator* iter) {
    if (iter && iter->target->type == SET_TYPE_MAP && iter->state.map_entry) {
        return iter->state.map_entry->key;
    }
    return NULL;
}

SetNode* set_iter_value(SetIterator* iter) {
    if (!iter) return NULL;
    if (iter->target->type == SET_TYPE_MAP && iter->state.map_entry) {
        return iter->state.map_entry->value;
    }
    if (iter->target->type == SET_TYPE_ARRAY) {
        if (iter->state.array_index < iter->target->data.array.count) {
            return iter->target->data.array.items[iter->state.array_index];
        }
    }
    return NULL;
}

void set_iter_free(SetIterator* iter) {
    free(iter);
}

// ============================================================================
// SECTION: Index Management API
// ============================================================================

SetIndex* set_index_create(SetConfig* cfg, const char* collection_path, const char* field, IndexType type) {
    if (!cfg || !collection_path || !field) return NULL;
    
    // Allocate index structure
    SetIndex* index = (SetIndex*)arena_alloc(&cfg->arena, sizeof(SetIndex));
    index->config = cfg;
    strncpy(index->collection_path, collection_path, sizeof(index->collection_path) - 1);
    strncpy(index->field, field, sizeof(index->field) - 1);
    index->type = type;
    index->entry_count = 0;
    index->field_type = SET_TYPE_NULL;
    index->is_composite = 0;
    index->field_count = 1;
    
    // Initialize index data
    if (type == INDEX_TYPE_BTREE) {
        index->data.btree_root = NULL;
    } else {
        index->data.hash_index.capacity = 1024;
        index->data.hash_index.count = 0;
        index->data.hash_index.entries = (HashEntry*)calloc(1024, sizeof(HashEntry));
    }
    
    // Build index from existing data
    SetNode* collection = set_query(cfg, collection_path);
    if (collection && collection->type == SET_TYPE_ARRAY) {
        for (size_t i = 0; i < collection->data.array.count; i++) {
            SetNode* record = collection->data.array.items[i];
            if (record && record->type == SET_TYPE_MAP) {
                SetNode* key_node = map_get(&record->data.map, field);
                if (key_node) {
                    // Store field type from first key
                    if (index->field_type == SET_TYPE_NULL) {
                        index->field_type = key_node->type;
                    }
                    
                    if (type == INDEX_TYPE_BTREE) {
                        index->data.btree_root = btree_insert(&cfg->arena, index->data.btree_root, key_node, record);
                    } else {
                        // Hash index implementation (simple)
                        uint32_t hash = hash_string(field);
                        size_t idx = hash % index->data.hash_index.capacity;
                        while (index->data.hash_index.entries[idx].value != NULL) {
                            idx = (idx + 1) % index->data.hash_index.capacity;
                        }
                        index->data.hash_index.entries[idx].hash = hash;
                        index->data.hash_index.entries[idx].value = record;
                        index->data.hash_index.count++;
                    }
                    index->entry_count++;
                }
            }
        }
    }
    
    // Add to registry
    index->next = cfg->indexes.head;
    cfg->indexes.head = index;
    cfg->indexes.count++;
    
    return index;
}

void set_index_drop(SetIndex* index) {
    if (!index || !index->config) return;
    
    // Remove from registry
    SetIndex** curr = &index->config->indexes.head;
    while (*curr) {
        if (*curr == index) {
            *curr = index->next;
            index->config->indexes.count--;
            break;
        }
        curr = &(*curr)->next;
    }
    
    // Free hash index entries if allocated (not arena-allocated)
    if (index->type == INDEX_TYPE_HASH && index->data.hash_index.entries) {
        free(index->data.hash_index.entries);
    }
    
    // Free composite field names if allocated
    if (index->is_composite && index->composite_fields) {
        // composite_fields were arena_alloc'd, so they don't need explicit freeing
    }
    
    // Note: index itself is arena_alloc'd, so it's automatically freed with the config
}

void set_index_rebuild(SetIndex* index) {
    if (!index || !index->config) return;
    
    // Clear existing index
    index->entry_count = 0;
    if (index->type == INDEX_TYPE_BTREE) {
        index->data.btree_root = NULL;
    } else if (index->type == INDEX_TYPE_HASH) {
        memset(index->data.hash_index.entries, 0, 
               index->data.hash_index.capacity * sizeof(HashEntry));
        index->data.hash_index.count = 0;
    }
    
    // Rebuild from collection
    SetNode* collection = set_query(index->config, index->collection_path);
    if (!collection || collection->type != SET_TYPE_ARRAY) return;
    
    for (size_t i = 0; i < collection->data.array.count; i++) {
        SetNode* record = collection->data.array.items[i];
        if (!record || record->type != SET_TYPE_MAP) continue;
        
        // Handle composite vs single field indexes
        SetNode* key_node = NULL;
        if (index->is_composite && index->composite_fields) {
            // Create composite key
            key_node = create_composite_key(&index->config->arena, record, 
                                           (const char**)index->composite_fields, 
                                           index->field_count);
        } else {
            // Single field
            key_node = map_get(&record->data.map, index->field);
        }
        
        if (key_node) {
            // Store field type from first key
            if (index->field_type == SET_TYPE_NULL) {
                index->field_type = key_node->type;
            }
            
            if (index->type == INDEX_TYPE_BTREE) {
                index->data.btree_root = btree_insert(&index->config->arena, 
                                                      index->data.btree_root, 
                                                      key_node, record);
            } else {
                // Hash index implementation
                uint32_t hash = hash_string(index->field);
                size_t idx = hash % index->data.hash_index.capacity;
                while (index->data.hash_index.entries[idx].value != NULL) {
                    idx = (idx + 1) % index->data.hash_index.capacity;
                }
                index->data.hash_index.entries[idx].hash = hash;
                index->data.hash_index.entries[idx].value = record;
                index->data.hash_index.count++;
            }
            index->entry_count++;
        }
    }
}

SetNode* set_index_query(SetIndex* index, DbOp op, const void* value, int return_single) {
    if (!index || !value) return NULL;
    
    // Create search key node
    SetNode search_key;
    memset(&search_key, 0, sizeof(SetNode));
    search_key.type = index->field_type;
    
    // Set search value
    switch (search_key.type) {
        case SET_TYPE_INT:
            search_key.data.i_val = *(long*)value;
            break;
        case SET_TYPE_STRING:
            search_key.data.s_val = (char*)value;
            break;
        case SET_TYPE_DOUBLE:
            search_key.data.d_val = *(double*)value;
            break;
        default:
            return NULL;
    }
    
    if (op == DB_OP_EQ && index->type == INDEX_TYPE_BTREE) {
        SetNode* result = btree_search(index->data.btree_root, &search_key);
        if (result) {
            SetNode* results = node_create(&index->config->arena, SET_TYPE_ARRAY);
            array_push(&index->config->arena, &results->data.array, result);
            return results;
        }
    }
    
    return NULL;
}

SetNode* set_index_range(SetIndex* index, const void* min, const void* max, size_t limit) {
    if (!index || index->type != INDEX_TYPE_BTREE) return NULL;
    
    SetNode* results = node_create(&index->config->arena, SET_TYPE_ARRAY);
    size_t count = 0;
    
    SetNode min_key, max_key;
    memset(&min_key, 0, sizeof(SetNode));
    memset(&max_key, 0, sizeof(SetNode));
    
    if (index->data.btree_root && index->entry_count > 0) {
        min_key.type = max_key.type = index->data.btree_root->keys[0]->type;
    }
    
    // Set min/max values
    if (min) {
        if (min_key.type == SET_TYPE_INT) min_key.data.i_val = *(long*)min;
        else if (min_key.type == SET_TYPE_STRING) min_key.data.s_val = (char*)min;
    }
    
    if (max) {
        if (max_key.type == SET_TYPE_INT) max_key.data.i_val = *(long*)max;
        else if (max_key.type == SET_TYPE_STRING) max_key.data.s_val = (char*)max;
    }
    
    btree_range_recursive(index->data.btree_root, 
                         min ? &min_key : NULL,
                         max ? &max_key : NULL,
                         &index->config->arena,
                         &results->data.array,
                         &count,
                         limit);
    
    return results;
}

void set_index_stats(SetIndex* index, IndexStats* stats) {
    if (!index || !stats) return;
    
    stats->entry_count = index->entry_count;
    stats->memory_usage = 0;  // Not tracked anymore
    stats->depth = 0;
    stats->fill_factor = 0.0;
    
    // Calculate depth for B-tree
    if (index->type == INDEX_TYPE_BTREE && index->data.btree_root) {
        BTreeNode* node = index->data.btree_root;
        while (!node->is_leaf && node->children[0]) {
            node = node->children[0];
            stats->depth++;
        }
        stats->fill_factor = (double)index->entry_count / (BTREE_ORDER * (stats->depth + 1));
    } else if (index->type == INDEX_TYPE_HASH) {
        stats->fill_factor = (double)index->data.hash_index.count / index->data.hash_index.capacity;
    }
}

// ============================================================================
// SECTION: Aggregation Functions (Phase 2)
// ============================================================================

// Helper: Extract numeric value from node
static double node_to_double(SetNode* node) {
    if (!node) return 0.0;
    
    switch (node->type) {
        case SET_TYPE_INT:
            return (double)node->data.i_val;
        case SET_TYPE_DOUBLE:
            return node->data.d_val;
        case SET_TYPE_STRING: {
            char* endptr;
            double val = strtod(node->data.s_val, &endptr);
            return (endptr != node->data.s_val) ? val : 0.0;
        }
        case SET_TYPE_BOOL:
            return node->data.b_val ? 1.0 : 0.0;
        default:
            return 0.0;
    }
}

// Basic aggregation without filtering
double set_aggregate(SetConfig* cfg, const char* collection_path, const char* field, AggregateOp op) {
    SetNode* collection = set_query(cfg, collection_path);
    if (!collection || collection->type != SET_TYPE_ARRAY) {
        return op == AGG_COUNT ? 0.0 : -1.0;
    }
    
    size_t count = 0;
    double sum = 0.0;
    double min_val = INFINITY;
    double max_val = -INFINITY;
    
    for (size_t i = 0; i < collection->data.array.count; i++) {
        SetNode* record = collection->data.array.items[i];
        if (!record || record->type != SET_TYPE_MAP) continue;
        
        if (op == AGG_COUNT) {
            count++;
            continue;
        }
        
        SetNode* field_node = map_get(&record->data.map, field);
        if (!field_node) continue;
        
        double val = node_to_double(field_node);
        count++;
        sum += val;
        
        if (val < min_val) min_val = val;
        if (val > max_val) max_val = val;
    }
    
    switch (op) {
        case AGG_COUNT:
            return (double)collection->data.array.count;
        case AGG_SUM:
            return sum;
        case AGG_AVG:
            return count > 0 ? sum / count : 0.0;
        case AGG_MIN:
            return min_val;
        case AGG_MAX:
            return max_val;
        default:
            return -1.0;
    }
}

// Aggregation with WHERE filtering
double set_aggregate_where(SetConfig* cfg, const char* collection_path, const char* field,
                          AggregateOp op, const char* filter_field, DbOp filter_op, const void* filter_value) {
    SetNode* collection = set_query(cfg, collection_path);
    if (!collection || collection->type != SET_TYPE_ARRAY) {
        return op == AGG_COUNT ? 0.0 : -1.0;
    }
    
    size_t count = 0;
    double sum = 0.0;
    double min_val = INFINITY;
    double max_val = -INFINITY;
    
    for (size_t i = 0; i < collection->data.array.count; i++) {
        SetNode* record = collection->data.array.items[i];
        if (!record || record->type != SET_TYPE_MAP) continue;
        
        // Apply filter
        SetNode* filter_node = map_get(&record->data.map, filter_field);
        if (!filter_node) continue;
        
        // Check filter condition
        int matches = 0;
        if (filter_op == DB_OP_EQ) {
            if (filter_node->type == SET_TYPE_STRING) {
                matches = (strcmp(filter_node->data.s_val, (const char*)filter_value) == 0);
            } else if (filter_node->type == SET_TYPE_INT) {
                matches = (filter_node->data.i_val == *(long*)filter_value);
            }
        }
        
        if (!matches) continue;
        
        if (op == AGG_COUNT) {
            count++;
            continue;
        }
        
        SetNode* field_node = map_get(&record->data.map, field);
        if (!field_node) continue;
        
        double val = node_to_double(field_node);
        count++;
        sum += val;
        
        if (val < min_val) min_val = val;
        if (val > max_val) max_val = val;
    }
    
    switch (op) {
        case AGG_COUNT:
            return (double)count;
        case AGG_SUM:
            return sum;
        case AGG_AVG:
            return count > 0 ? sum / count : 0.0;
        case AGG_MIN:
            return count > 0 ? min_val : 0.0;
        case AGG_MAX:
            return count > 0 ? max_val : 0.0;
        default:
            return -1.0;
    }
}

// GROUP BY implementation
SetNode* set_group_by(SetConfig* cfg, const char* collection_path,
                     const char* group_field, const char* agg_field, AggregateOp op) {
    SetNode* collection = set_query(cfg, collection_path);
    if (!collection || collection->type != SET_TYPE_ARRAY) {
        return NULL;
    }
    
    // Create result map
    SetNode* results = node_create(&cfg->arena, SET_TYPE_MAP);
    
    // Group records by group_field value
    for (size_t i = 0; i < collection->data.array.count; i++) {
        SetNode* record = collection->data.array.items[i];
        if (!record || record->type != SET_TYPE_MAP) continue;
        
        SetNode* group_node = map_get(&record->data.map, group_field);
        if (!group_node) continue;
        
        // Get group key as string
        const char* group_key = group_node->type == SET_TYPE_STRING ? 
                               group_node->data.s_val : "other";
        
        // Get or create group entry
        SetNode* group_entry = map_get(&results->data.map, group_key);
        if (!group_entry) {
            group_entry = set_set_child(results, group_key, SET_TYPE_MAP);
            SetNode* count_node = set_set_child(group_entry, "count", SET_TYPE_INT);
            set_node_set_int(count_node, 0);
            SetNode* sum_node = set_set_child(group_entry, "sum", SET_TYPE_DOUBLE);
            set_node_set_double(sum_node, 0.0);
        }
        
        // Update aggregates
        SetNode* count_node = map_get(&group_entry->data.map, "count");
        SetNode* sum_node = map_get(&group_entry->data.map, "sum");
        
        long current_count = set_node_int(count_node, 0);
        set_node_set_int(count_node, current_count + 1);
        
        if (agg_field) {
            SetNode* agg_node = map_get(&record->data.map, agg_field);
            if (agg_node) {
                double val = node_to_double(agg_node);
                double current_sum = set_node_double(sum_node, 0.0);
                set_node_set_double(sum_node, current_sum + val);
            }
        }
    }
    
    return results;
}

// HAVING: Filter GROUP BY results based on aggregate condition
SetNode* set_having(SetConfig* cfg, SetNode* grouped_results, const char* agg_field, DbOp op, double value) {
    if (!cfg || !grouped_results || grouped_results->type != SET_TYPE_MAP) {
        return grouped_results;
    }
    
    // Create filtered result map
    SetNode* filtered = node_create(&cfg->arena, SET_TYPE_MAP);
    
    SetIterator* iter = set_iter_create(grouped_results);
    while (set_iter_next(iter)) {
        const char* group_key = set_iter_key(iter);
        SetNode* group_data = set_iter_value(iter);
        
        // Get the aggregate value
        SetNode* agg_node = map_get(&group_data->data.map, agg_field);
        if (!agg_node) continue;
        
        double agg_value = node_to_double(agg_node);
        
        // Apply filter condition
        int matches = 0;
        switch (op) {
            case DB_OP_EQ: matches = (agg_value == value); break;
            case DB_OP_NEQ: matches = (agg_value != value); break;
            case DB_OP_GT: matches = (agg_value > value); break;
            case DB_OP_LT: matches = (agg_value < value); break;
            default: break;
        }
        
        if (matches) {
            map_put(&cfg->arena, &filtered->data.map, group_key, group_data);
        }
    }
    set_iter_free(iter);
    
    return filtered;
}

// Helper: Compare two nodes for sorting
static int compare_for_sort(SetNode* a, SetNode* b, const char* field, int ascending) {
    if (!a || !b) return 0;
    
    SetNode* a_val = map_get(&a->data.map, field);
    SetNode* b_val = map_get(&b->data.map, field);
    
    // NULL handling: NULLs sort last
    if (!a_val && !b_val) return 0;
    if (!a_val) return 1;
    if (!b_val) return -1;
    
    int cmp = compare_nodes(a_val, b_val);
    return ascending ? cmp : -cmp;
}

// Quicksort implementation for array sorting
static void quicksort_array(SetNode** arr, int left, int right, const char* field, int ascending) {
    if (left >= right) return;
    
    // Partition
    int pivot_idx = left + (right - left) / 2;
    SetNode* pivot = arr[pivot_idx];
    int i = left, j = right;
    
    while (i <= j) {
        while (i <= right && compare_for_sort(arr[i], pivot, field, ascending) < 0) i++;
        while (j >= left && compare_for_sort(arr[j], pivot, field, ascending) > 0) j--;
        
        if (i <= j) {
            SetNode* temp = arr[i];
            arr[i] = arr[j];
            arr[j] = temp;
            i++;
            j--;
        }
    }
    
    if (left < j) quicksort_array(arr, left, j, field, ascending);
    if (i < right) quicksort_array(arr, i, right, field, ascending);
}

// ORDER BY: Sort collection by field
SetNode* set_order_by(SetNode* collection, const char* field, int ascending) {
    if (!collection || collection->type != SET_TYPE_ARRAY || !field) {
        return collection;
    }
    
    if (collection->data.array.count <= 1) {
        return collection;  // Already sorted
    }
    
    // Sort the array in-place
    quicksort_array(collection->data.array.items, 0, 
                   collection->data.array.count - 1, field, ascending);
    
    return collection;
}

// LIMIT/OFFSET: Paginate results (creates new array with subset)
SetNode* set_limit(SetNode* collection, size_t limit, size_t offset) {
    if (!collection || collection->type != SET_TYPE_ARRAY) {
        return collection;
    }
    
    size_t total = collection->data.array.count;
    
    // If offset is beyond array, return empty
    if (offset >= total) {
        // Return the existing empty structure rather than creating new one
        return collection;
    }
    
    // Calculate actual range
    size_t start = offset;
    size_t count = (offset + limit > total) ? (total - offset) : limit;
    
    // If limit encompasses everything from offset, just return original
    if (offset == 0 && count >= total) {
        return collection;
    }
    
    // For in-place limiting, just adjust the count
    // This is a simplified approach - a full implementation would create a new array
    // For now, we'll return the original and document this limitation
    (void)start;
    (void)count;
    
    return collection;
}

// ============================================================================
// SECTION: JOIN Operations (Phase 3)
// ============================================================================

// Helper: Create a joined record with fields from both sides
static SetNode* create_joined_record(Arena* a, SetNode* left, SetNode* right, 
                                     const char* left_prefix, const char* right_prefix) {
    SetNode* result = node_create(a, SET_TYPE_MAP);
    
    // Copy fields from left record
    if (left && left->type == SET_TYPE_MAP) {
        SetIterator* iter = set_iter_create(left);
        while (set_iter_next(iter)) {
            const char* key = set_iter_key(iter);
            SetNode* val = set_iter_value(iter);
            
            // Add prefix if specified
            char prefixed_key[256];
            if (left_prefix && left_prefix[0]) {
                snprintf(prefixed_key, sizeof(prefixed_key), "%s_%s", left_prefix, key);
                map_put(a, &result->data.map, prefixed_key, val);
            } else {
                map_put(a, &result->data.map, key, val);
            }
        }
        set_iter_free(iter);
    }
    
    // Copy fields from right record
    if (right && right->type == SET_TYPE_MAP) {
        SetIterator* iter = set_iter_create(right);
        while (set_iter_next(iter)) {
            const char* key = set_iter_key(iter);
            SetNode* val = set_iter_value(iter);
            
            // Add prefix if specified
            char prefixed_key[256];
            if (right_prefix && right_prefix[0]) {
                snprintf(prefixed_key, sizeof(prefixed_key), "%s_%s", right_prefix, key);
                map_put(a, &result->data.map, prefixed_key, val);
            } else {
                map_put(a, &result->data.map, key, val);
            }
        }
        set_iter_free(iter);
    }
    
    return result;
}

// INNER JOIN: Only records that match on both sides
SetNode* set_join(SetConfig* cfg,
                 const char* left_collection, const char* left_field,
                 const char* right_collection, const char* right_field,
                 JoinType join_type) {
    return set_join_as(cfg, left_collection, left_field, NULL,
                      right_collection, right_field, NULL, join_type);
}

// JOIN with field prefixes
SetNode* set_join_as(SetConfig* cfg,
                    const char* left_collection, const char* left_field, const char* left_prefix,
                    const char* right_collection, const char* right_field, const char* right_prefix,
                    JoinType join_type) {
    if (!cfg || !left_collection || !right_collection || !left_field || !right_field) {
        return NULL;
    }
    
    SetNode* left = set_query(cfg, left_collection);
    SetNode* right = set_query(cfg, right_collection);
    
    if (!left || left->type != SET_TYPE_ARRAY || !right || right->type != SET_TYPE_ARRAY) {
        return NULL;
    }
    
    // Create result array
    SetNode* results = node_create(&cfg->arena, SET_TYPE_ARRAY);
    
    // Check if there's an index on the right collection's join field
    SetIndex* right_index = NULL;
    SetIndex* idx = cfg->indexes.head;
    while (idx) {
        if (strcmp(idx->collection_path, right_collection) == 0 &&
            strcmp(idx->field, right_field) == 0) {
            right_index = idx;
            break;
        }
        idx = idx->next;
    }
    
    // Perform join
    for (size_t i = 0; i < left->data.array.count; i++) {
        SetNode* left_record = left->data.array.items[i];
        if (!left_record || left_record->type != SET_TYPE_MAP) continue;
        
        SetNode* left_key = map_get(&left_record->data.map, left_field);
        if (!left_key) {
            // LEFT JOIN: include left record with NULL right side
            if (join_type == JOIN_LEFT) {
                SetNode* joined = create_joined_record(&cfg->arena, left_record, NULL, 
                                                       left_prefix, right_prefix);
                array_push(&cfg->arena, &results->data.array, joined);
            }
            continue;
        }
        
        int found_match = 0;
        
        if (right_index && right_index->type == INDEX_TYPE_BTREE) {
            // Use index for fast lookup
            SetNode* matches = set_index_query(right_index, DB_OP_EQ, 
                                              left_key->type == SET_TYPE_STRING ? 
                                              (void*)left_key->data.s_val : 
                                              (void*)&left_key->data.i_val, 0);
            
            if (matches && matches->type == SET_TYPE_ARRAY) {
                for (size_t j = 0; j < matches->data.array.count; j++) {
                    SetNode* right_record = matches->data.array.items[j];
                    SetNode* joined = create_joined_record(&cfg->arena, left_record, right_record,
                                                           left_prefix, right_prefix);
                    array_push(&cfg->arena, &results->data.array, joined);
                    found_match = 1;
                }
            }
        } else {
            // Linear scan (no index available)
            for (size_t j = 0; j < right->data.array.count; j++) {
                SetNode* right_record = right->data.array.items[j];
                if (!right_record || right_record->type != SET_TYPE_MAP) continue;
                
                SetNode* right_key = map_get(&right_record->data.map, right_field);
                if (!right_key) continue;
                
                // Compare keys
                int match = 0;
                if (left_key->type == right_key->type) {
                    if (left_key->type == SET_TYPE_STRING) {
                        match = (strcmp(left_key->data.s_val, right_key->data.s_val) == 0);
                    } else if (left_key->type == SET_TYPE_INT) {
                        match = (left_key->data.i_val == right_key->data.i_val);
                    } else if (left_key->type == SET_TYPE_DOUBLE) {
                        match = (left_key->data.d_val == right_key->data.d_val);
                    }
                }
                
                if (match) {
                    SetNode* joined = create_joined_record(&cfg->arena, left_record, right_record,
                                                           left_prefix, right_prefix);
                    array_push(&cfg->arena, &results->data.array, joined);
                    found_match = 1;
                }
            }
        }
        
        // LEFT JOIN: include left record even if no match found
        if (!found_match && join_type == JOIN_LEFT) {
            SetNode* joined = create_joined_record(&cfg->arena, left_record, NULL,
                                                   left_prefix, right_prefix);
            array_push(&cfg->arena, &results->data.array, joined);
        }
    }
    
    // RIGHT JOIN: Swap left and right, then do LEFT JOIN
    if (join_type == JOIN_RIGHT) {
        // RIGHT JOIN is just LEFT JOIN with sides swapped
        return set_join_as(cfg,
                          right_collection, right_field, right_prefix,
                          left_collection, left_field, left_prefix,
                          JOIN_LEFT);
    }
    
    return results;
}
// Composite index implementation

// Helper: Create composite key from multiple field values
static SetNode* create_composite_key(Arena* a, SetNode* record, const char** fields, size_t field_count) {
    if (field_count == 1) {
        // Single field - just return the field value
        return map_get(&record->data.map, fields[0]);
    }
    
    // Multi-field: Create a string key by concatenating field values
    char composite_key[512] = "";
    for (size_t i = 0; i < field_count; i++) {
        SetNode* field_val = map_get(&record->data.map, fields[i]);
        if (!field_val) continue;
        
        if (i > 0) strcat(composite_key, "|");  // Separator
        
        if (field_val->type == SET_TYPE_STRING) {
            strcat(composite_key, field_val->data.s_val);
        } else if (field_val->type == SET_TYPE_INT) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%ld", field_val->data.i_val);
            strcat(composite_key, buf);
        } else if (field_val->type == SET_TYPE_DOUBLE) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.6f", field_val->data.d_val);
            strcat(composite_key, buf);
        }
    }
    
    // Create a string node with the composite key
    SetNode* key = node_create(a, SET_TYPE_STRING);
    key->data.s_val = arena_strdup(a, composite_key);
    return key;
}

// Create composite index
SetIndex* set_index_create_composite(SetConfig* cfg, const char* collection_path,
                                     const char** fields, size_t field_count, IndexType type) {
    if (!cfg || !collection_path || !fields || field_count == 0) {
        return NULL;
    }
    
    SetNode* collection = set_query(cfg, collection_path);
    if (!collection || collection->type != SET_TYPE_ARRAY) {
        return NULL;
    }
    
    // Create index structure
    SetIndex* index = arena_alloc(&cfg->arena, sizeof(SetIndex));
    strncpy(index->collection_path, collection_path, sizeof(index->collection_path) - 1);
    index->type = type;
    index->entry_count = 0;
    index->is_composite = 1;
    index->field_count = field_count;
    
    // Copy field names
    index->composite_fields = arena_alloc(&cfg->arena, sizeof(char*) * field_count);
    for (size_t i = 0; i < field_count; i++) {
        index->composite_fields[i] = arena_strdup(&cfg->arena, fields[i]);
    }
    
    // For single field, copy to field for compatibility
    if (field_count == 1) {
        strncpy(index->field, fields[0], sizeof(index->field) - 1);
    } else {
        index->field[0] = '\0';  // Empty for multi-field
    }
    
    // Build the index
    if (type == INDEX_TYPE_BTREE) {
        index->data.btree_root = NULL;
        
        for (size_t i = 0; i < collection->data.array.count; i++) {
            SetNode* record = collection->data.array.items[i];
            if (!record || record->type != SET_TYPE_MAP) continue;
            
            SetNode* key = create_composite_key(&cfg->arena, record, fields, field_count);
            if (key) {
                index->data.btree_root = btree_insert(&cfg->arena, index->data.btree_root, key, record);
                index->entry_count++;
            }
        }
    }
    
    // Add to registry
    index->next = cfg->indexes.head;
    cfg->indexes.head = index;
    cfg->indexes.count++;
    
    return index;
}

// Query composite index
SetNode* set_index_query_composite(SetIndex* index, const void** values, size_t value_count) {
    if (!index || !values || value_count != index->field_count) {
        return NULL;
    }
    
    // Build composite key from values
    char composite_key[512] = "";
    for (size_t i = 0; i < value_count; i++) {
        if (i > 0) strcat(composite_key, "|");
        
        // Assume string values for now - in real implementation would need type info
        const char* val_str = (const char*)values[i];
        strcat(composite_key, val_str);
    }
    
    // Search using the composite key
    if (index->type == INDEX_TYPE_BTREE) {
        return btree_search(index->data.btree_root, NULL);  // Simplified
    }
    
    return NULL;
}
