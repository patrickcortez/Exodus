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
uint32_t set_node_flags(SetNode* node);

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

// 4. Index Management (Phase 1: Performance Enhancement)
typedef enum {
    INDEX_TYPE_HASH,    // Exact match lookups (O(1))
    INDEX_TYPE_BTREE    // Range queries, sorting (O(log n))
} IndexType;

typedef struct SetIndex SetIndex;

// Create index on a collection field
SetIndex* set_index_create(SetConfig* cfg, const char* collection_path, const char* field, IndexType type);

// Create composite index on multiple fields (e.g., for (user_id, status) queries)
SetIndex* set_index_create_composite(SetConfig* cfg, const char* collection_path, const char** fields, size_t field_count, IndexType type);

// Drop an index
void set_index_drop(SetIndex* index);

// Rebuild index (after bulk inserts)
void set_index_rebuild(SetIndex* index);

// Query with single field
SetNode* set_index_query(SetIndex* index, DbOp op, const void* value, int return_single);

// Query composite index with multiple values (must match field order)
SetNode* set_index_query_composite(SetIndex* index, const void** values, size_t value_count);

// Range query (BTREE only)
SetNode* set_index_range(SetIndex* index, const void* min, const void* max, size_t limit);

// Get index statistics
typedef struct {
    size_t entry_count;
    size_t memory_usage;
    size_t depth;  // BTREE only
    double fill_factor;
} IndexStats;

void set_index_stats(SetIndex* index, IndexStats* stats);

// 5. Aggregation Functions (Phase 2: Query Operations)
typedef enum {
    AGG_COUNT,
    AGG_SUM,
    AGG_AVG,
    AGG_MIN,
    AGG_MAX
} AggregateOp;

// Aggregate a field across all records in collection
double set_aggregate(SetConfig* cfg, const char* collection_path, const char* field, AggregateOp op);

// Aggregate with filtering (uses WHERE clause logic)
double set_aggregate_where(SetConfig* cfg, const char* collection_path, const char* field, 
                          AggregateOp op, const char* filter_field, DbOp filter_op, const void* filter_value);

// GROUP BY: Returns map of {group_value -> aggregated_value}
SetNode* set_group_by(SetConfig* cfg, const char* collection_path, 
                     const char* group_field, const char* agg_field, AggregateOp op);

// HAVING: Filter GROUP BY results based on aggregate condition
SetNode* set_having(SetConfig* cfg, SetNode* grouped_results, const char* agg_field, DbOp op, double value);

// ORDER BY: Sort results by field (ascending/descending)
SetNode* set_order_by(SetNode* collection, const char* field, int ascending);

// LIMIT/OFFSET: Paginate results
SetNode* set_limit(SetNode* collection, size_t limit, size_t offset);

// 6. JOIN Operations (Phase 3: Multi-Collection Queries)
typedef enum {
    JOIN_INNER,  // Only matching records from both sides
    JOIN_LEFT,   // All from left + matching from right (NULL for non-matches)
    JOIN_RIGHT   // All from right + matching from left (NULL for non-matches)
} JoinType;

// Join two collections on matching field values
// Result: Array of maps with fields from both collections
SetNode* set_join(SetConfig* cfg, 
                 const char* left_collection, const char* left_field,
                 const char* right_collection, const char* right_field,
                 JoinType join_type);

// Join with custom field mapping (prefix left/right to avoid conflicts)
SetNode* set_join_as(SetConfig* cfg,
                    const char* left_collection, const char* left_field, const char* left_prefix,
                    const char* right_collection, const char* right_field, const char* right_prefix,
                    JoinType join_type);

#ifdef __cplusplus
}
#endif

#endif // CTZ_SET_H