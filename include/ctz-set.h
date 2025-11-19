/*
 * ctz-set.h
 * Cortez Config System v2.1 (Refactored)
 */

#ifndef CTZ_SET_H
#define CTZ_SET_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Types ---

typedef struct SetConfig SetConfig;
typedef struct SetNode SetNode;
typedef struct SetIterator SetIterator;

typedef enum {
    SET_TYPE_NULL = 0,
    SET_TYPE_STRING,
    SET_TYPE_INT,
    SET_TYPE_BOOL,
    SET_TYPE_DOUBLE,
    SET_TYPE_ARRAY,
    SET_TYPE_MAP
} SetType;

// --- Lifecycle ---

SetConfig* set_load(const char* filepath);
SetConfig* set_create(const char* filepath);
int set_save(SetConfig* config);
int set_dump(SetConfig* config, FILE* stream);
void set_free(SetConfig* config);
const char* set_get_error(SetConfig* config);

// --- DOM Navigation ---

SetNode* set_get_root(SetConfig* config);
SetNode* set_get_child(SetNode* map_node, const char* key);
SetNode* set_get_at(SetNode* array_node, size_t index);
SetNode* set_query(SetConfig* config, const char* path);

// --- Accessors ---

SetType set_node_type(SetNode* node);

const char* set_node_string(SetNode* node, const char* default_val);
long set_node_int(SetNode* node, long default_val);
double set_node_double(SetNode* node, double default_val);
int set_node_bool(SetNode* node, int default_val);
size_t set_node_size(SetNode* node);

// --- Legacy / Simplified Accessors ---

const char* set_get_string(SetConfig* config, const char* section, const char* key, const char* def);
long set_get_int(SetConfig* config, const char* section, const char* key, long def);
double set_get_double(SetConfig* config, const char* section, const char* key, double def);
int set_get_bool(SetConfig* config, const char* section, const char* key, int def);

// --- Modification ---

SetNode* set_set_child(SetNode* map_node, const char* key, SetType type);
SetNode* set_array_push(SetNode* array_node, SetType type);
void set_remove_child(SetNode* map_node, const char* key);

void set_node_set_string(SetNode* node, const char* val);
void set_node_set_int(SetNode* node, long val);
void set_node_set_double(SetNode* node, double val);
void set_node_set_bool(SetNode* node, int val);

// --- Validation ---

typedef int (*SetValidator)(const char* path, SetNode* node, char* err_out, size_t err_size);
void set_add_schema(SetConfig* config, const char* path, SetType type, int required, SetValidator validator);
int set_validate(SetConfig* config);

// --- Iteration ---

SetIterator* set_iter_create(SetNode* node);
int set_iter_next(SetIterator* iter);
const char* set_iter_key(SetIterator* iter);
SetNode* set_iter_value(SetIterator* iter);
void set_iter_free(SetIterator* iter);

// --- Database Features ---

// 1. Concurrency
void set_db_init(SetConfig* config);
void set_db_lock(SetConfig* config);
void set_db_unlock(SetConfig* config);

// 2. Persistence
int set_db_commit(SetConfig* config);

// 3. Query Engine
typedef enum {
    DB_OP_EQ,       
    DB_OP_NEQ,      
    DB_OP_GT,       
    DB_OP_LT,       
    DB_OP_CONTAINS  
} DbOp;

SetNode* set_db_select(SetConfig* cfg, const char* collection_path, const char* field, DbOp op, const void* value, size_t limit, size_t offset);

SetNode* set_db_insert(SetConfig* cfg, const char* collection_path);

#ifdef __cplusplus
}
#endif

#endif // CTZ_SET_H