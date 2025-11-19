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
    union {
        char* s_val;
        long i_val;
        double d_val;
        int b_val;
        SetMap map;
        SetArray array;
    } data;
};

struct SetConfig {
    SetNode* root;
    Arena arena;
    char* filepath;
    //char error_buf[ERR_BUF_SIZE];
    struct SetSchemaEntry* schema_head;

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

static void     set_error(SetConfig* cfg, const char* fmt, ...);
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

static CTZ_THREAD_LOCAL char tls_error_buf[ERR_BUF_SIZE];


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
    TOK_ASSIGN      // =
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
        t.type = TOK_ERROR;
        set_error(l->cfg, "Syntax Error: '{' and '}' are not supported. Use '-:' and ':-'");
        return t; 
    }
    if (c == '[') { lex_advance(l); t.type = TOK_LBRACKET; t.length = 1; return t; }
    if (c == ']') { lex_advance(l); t.type = TOK_RBRACKET; t.length = 1; return t; }
    if (c == '|') { lex_advance(l); t.type = TOK_PIPE; t.length = 1; return t; }
    if (c == ':') { lex_advance(l); t.type = TOK_COLON; t.length = 1; return t; }
    if (c == ',') { lex_advance(l); t.type = TOK_COMMA; t.length = 1; return t; }
    if (c == '=') { lex_advance(l); t.type = TOK_ASSIGN; t.length = 1; return t; }

    // --- Strings ---
    if (c == '"' || c == '\'') {
        char quote = c;
        lex_advance(l); // skip open quote
        t.start = l->src + l->pos; // start content
        
        while (l->pos < l->len) {
            char cur = lex_peek(l);
            if (cur == quote) break;
            if (cur == '\\') lex_advance(l); // skip escape char
            lex_advance(l);
        }
        
        t.length = (l->src + l->pos) - t.start;
        if (l->pos < l->len) lex_advance(l); // skip close quote
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

    // --- Identifiers / Keywords ---
    while (isalnum(lex_peek(l)) || strchr("_.-+/$", lex_peek(l))) {
        lex_advance(l);
    }
    
    t.length = (l->src + l->pos) - t.start;
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
                set_error(l->cfg, "SmartBlock: Expected string key at %d:%d", key_tok.line, key_tok.col);
                break;
            }
            
            Token colon = lex_scan_token(l);
            if (colon.type != TOK_COLON) {
                set_error(l->cfg, "SmartBlock: Expected ':' at %d:%d", colon.line, colon.col);
                break;
            }

            char* key = decode_string(&l->cfg->arena, key_tok.start, key_tok.length);
            SetNode* val = parse_value(l);
            map_put(&l->cfg->arena, &node->data.map, key, val);
        } else {
            SetNode* val = parse_value(l);
            array_push(&l->cfg->arena, &node->data.array, val);
        }
    }
    return node;
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
            while (1) {
                size_t mp = l->pos; int ml=l->line, mc=l->col;
                Token next = lex_scan_token(l);
                if (next.type == TOK_RBRACKET) break;
                if (next.type == TOK_COMMA) continue;
                
                l->pos = mp; l->line = ml; l->col = mc; 
                array_push(&l->cfg->arena, &n->data.array, parse_value(l));
            }
            return n;

        case TOK_STRING:
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

        default:
            return node_create(&l->cfg->arena, SET_TYPE_NULL);
    }
}

static void parse_map_body(Lexer* l, SetMap* map) {
    SetNode* active_section = NULL;

    while (1) {
        Token t = lex_scan_token(l);
        if (t.type == TOK_EOF) break;
        if (t.type == TOK_COMMA) continue;

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
                set_error(l->cfg, "Syntax error in section definition at line %d", t.line);
            }
        }

        if (t.type == TOK_STRING) {
            char* key = decode_string(&l->cfg->arena, t.start, t.length);

            // Handle Include
            if (strcmp(key, "include") == 0) {
                Token path = lex_scan_token(l);
                if (path.type == TOK_STRING) {
                    char* pattern = decode_string(&l->cfg->arena, path.start, path.length);
                    IncludeContext ctx = { l->cfg, active_section ? &active_section->data.map : map };
                    sys_list_directory(pattern, include_callback, &ctx);
                }
                continue;
            }

            Token op = lex_scan_token(l);
            if (op.type == TOK_COLON || op.type == TOK_ASSIGN) {
                SetNode* val = parse_value(l);
                SetMap* target = active_section ? &active_section->data.map : map;
                map_put(&l->cfg->arena, target, key, val);
            } else {
                set_error(l->cfg, "Expected ':' or '=' after key '%s' at line %d", key, t.line);
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

static char* expand_string(SetConfig* cfg, const char* input, int depth) {
    if (!strchr(input, '$')) return (char*)input;

    // Fix: Dynamic growing buffer instead of fixed ERR_BUF_SIZE
    size_t cap = 1024;
    size_t len = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) return (char*)input; // Fallback on allocation failure

    const char* src = input;
    
    while (*src) {
        // Resize check
        if (len + 256 > cap) { // Ensure reasonable headroom
            cap *= 2;
            char* tmp = (char*)realloc(buf, cap);
            if (!tmp) { free(buf); return (char*)input; }
            buf = tmp;
        }

        if (*src == '$') {
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
                char* key = arena_strndup(&cfg->arena, var_start, klen);
                char* val = resolve_variable(cfg, key, depth + 1);
                
                if (val) {
                    // Recurse
                    char* expanded_val = expand_string(cfg, val, depth + 1);
                    size_t vlen = strlen(expanded_val);
                    
                    // Resize for value
                    while (len + vlen >= cap) {
                        cap *= 2;
                        char* tmp = (char*)realloc(buf, cap);
                        if (!tmp) { free(buf); return (char*)input; }
                        buf = tmp;
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
    
    // Move result to Arena and free temp heap buffer
    char* result = arena_strdup(&cfg->arena, buf);
    free(buf);
    return result;
}

static void expand_node_tree(SetConfig* cfg, SetNode* node) {
    if (!node) return;

    if (node->type == SET_TYPE_STRING) {
        node->data.s_val = expand_string(cfg, node->data.s_val, 0);
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
    
    SetNode* cur = root;
    const char* ptr = path;
    char token[256]; // Reasonable limit for a single key segment
    
    while (*ptr && cur) {
        const char* start = ptr;
        // Parse until dot or bracket or end
        while (*ptr && *ptr != '.' && *ptr != '[') ptr++;
        
        size_t len = ptr - start;
        if (len >= sizeof(token)) len = sizeof(token) - 1;
        
        if (len > 0) {
            strncpy(token, start, len);
            token[len] = 0;
            cur = set_get_child(cur, token);
        }

        if (*ptr == '.') {
            ptr++; // Skip dot
        } 
        else if (*ptr == '[') {
            ptr++; // Skip opening bracket
            char* end_ptr;
            errno = 0;
            long idx = strtol(ptr, &end_ptr, 10);
            
            // Fix: Check for parsing errors, negative indices, and closing bracket
            if (errno != 0 || idx < 0 || *end_ptr != ']') {
                return NULL; // Invalid index syntax
            }
            
            cur = set_get_at(cur, (size_t)idx);
            ptr = end_ptr;
            if (*ptr == ']') ptr++; // Skip closing bracket
            if (*ptr == '.') ptr++; // Skip optional dot after bracket
        }
    }
    return cur;
}

SetNode* set_query(SetConfig* config, const char* path) {
    if (!config || !config->root) return NULL;
    return set_find_path(config->root, path);
}

// --- Type & Value Accessors ---

SetType set_node_type(SetNode* node) {
    return node ? node->type : SET_TYPE_NULL;
}

const char* set_node_string(SetNode* node, const char* default_val) {
    if (node && node->type == SET_TYPE_STRING) return node->data.s_val;
    return default_val;
}

long set_node_int(SetNode* node, long default_val) {
    if (!node) return default_val;
    if (node->type == SET_TYPE_INT) return node->data.i_val;
    if (node->type == SET_TYPE_DOUBLE) return (long)node->data.d_val;
    return default_val;
}

double set_node_double(SetNode* node, double default_val) {
    if (!node) return default_val;
    if (node->type == SET_TYPE_DOUBLE) return node->data.d_val;
    if (node->type == SET_TYPE_INT) return (double)node->data.i_val;
    return default_val;
}

int set_node_bool(SetNode* node, int default_val) {
    if (node && node->type == SET_TYPE_BOOL) return node->data.b_val;
    return default_val;
}

size_t set_node_size(SetNode* node) {
    if (!node) return 0;
    if (node->type == SET_TYPE_ARRAY) return node->data.array.count;
    if (node->type == SET_TYPE_MAP) return node->data.map.count;
    return 0;
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
    tls_error_buf[0] = 0;

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

    if (n->type == SET_TYPE_MAP) {
        fprintf(f, "-:\n");
        SetMapEntry* e = n->data.map.head_order;
        while (e) {
            for(int i=0; i<=indent; i++) fprintf(f, "  ");
            fprintf(f, "%s: ", e->key);
            dump_recursive(f, e->value, indent + 1);
            fprintf(f, "\n");
            e = e->next_ordered;
        }
        for(int i=0; i<indent; i++) fprintf(f, "  ");
        fprintf(f, ":-");
    } 
    else if (n->type == SET_TYPE_ARRAY) {
        fprintf(f, "-: ");
        for (size_t i = 0; i < n->data.array.count; i++) {
            dump_recursive(f, n->data.array.items[i], indent + 1);
            if (i < n->data.array.count - 1) fprintf(f, ", ");
        }
        fprintf(f, " :-");
    } 
    else if (n->type == SET_TYPE_STRING) {
        fprintf(f, "\"%s\"", n->data.s_val);
    } 
    else if (n->type == SET_TYPE_INT) {
        fprintf(f, "%ld", n->data.i_val);
    } 
    else if (n->type == SET_TYPE_DOUBLE) {
        fprintf(f, "%g", n->data.d_val);
    } 
    else if (n->type == SET_TYPE_BOOL) {
        fprintf(f, "%s", n->data.b_val ? "true" : "false");
    } 
    else {
        fprintf(f, "null");
    }
}

int set_dump(SetConfig* config, FILE* stream) {
    if (!config || !config->root) return -1;

    // Pretty print root level maps
    if (config->root->type == SET_TYPE_MAP) {
        SetMapEntry* e = config->root->data.map.head_order;
        while (e) {
            // If the value is a map, print it as a section |Section|
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

// --- Internal Utilities ---

/* REPLACEMENT: set_error (Writes to thread-local buffer) */
static void set_error(SetConfig* cfg, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(tls_error_buf, ERR_BUF_SIZE, fmt, args);
    tls_error_buf[ERR_BUF_SIZE - 1] = 0; 
    va_end(args);
}

const char* set_get_error(SetConfig* config) {
    (void)config; 
    return tls_error_buf;
}