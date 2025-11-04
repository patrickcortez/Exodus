/*
 * exodus-common.h
 * Shared definitions for the Exodus system.
 */
#ifndef EXODUS_COMMON_H
#define EXODUS_COMMON_H

#include "cortez-mesh.h" // Includes the user message start offset

// --- Node Names ---
#define CLOUD_DAEMON_NAME "cloud_daemon"
#define QUERY_DAEMON_NAME "query_daemon"
#define SNAPSHOT_DAEMON_NAME "snapshot_daemon"

// --- Message Types ---
// Must be offset by MESH_MSG_USER_START
enum exodus_msg_types {
    // Client -> Query Daemon
    MSG_UPLOAD_FILE = MESH_MSG_USER_START + 1,
    MSG_QUERY_WORD = MESH_MSG_USER_START + 2,
    MSG_CHANGE_WORD = MESH_MSG_USER_START + 3,
    MSG_WORD_COUNT = MESH_MSG_USER_START + 4,
    MSG_LINE_COUNT = MESH_MSG_USER_START + 5,
    MSG_CHAR_COUNT = MESH_MSG_USER_START + 6,

    // Node Management
    MSG_ADD_NODE = MESH_MSG_USER_START + 7,
    MSG_LIST_NODES = MESH_MSG_USER_START + 8,
    MSG_VIEW_NODE = MESH_MSG_USER_START + 9,
    MSG_ACTIVATE_NODE = MESH_MSG_USER_START + 10,
    MSG_DEACTIVATE_NODE = MESH_MSG_USER_START + 11,
    MSG_REMOVE_NODE = MESH_MSG_USER_START + 12,

    MSG_ATTR_NODE = MESH_MSG_USER_START + 13,   
    MSG_INFO_NODE = MESH_MSG_USER_START + 14,   
    MSG_SEARCH_ATTR = MESH_MSG_USER_START + 15, 

    MSG_LOOKUP_ITEM = MESH_MSG_USER_START + 16,
    MSG_PIN_ITEM = MESH_MSG_USER_START + 17,
    MSG_UNPIN_ITEM = MESH_MSG_USER_START + 18,
    MSG_COMMIT_NODE = MESH_MSG_USER_START + 19,

    // Cloud Daemon -> Query Daemon
    MSG_QUERY_RESPONSE = MESH_MSG_USER_START + 20,
    MSG_OPERATION_ACK = MESH_MSG_USER_START + 21,
    MSG_COUNT_RESPONSE = MESH_MSG_USER_START + 22,
    MSG_LIST_NODES_RESPONSE = MESH_MSG_USER_START + 23,
    MSG_VIEW_NODE_RESPONSE = MESH_MSG_USER_START + 24,

    MSG_INFO_NODE_RESPONSE = MESH_MSG_USER_START + 25,
    

    MSG_LOOKUP_RESPONSE = MESH_MSG_USER_START + 26,
    MSG_REBUILD_NODE = MESH_MSG_USER_START + 27, // Use 27 to leave space
        // ... after MSG_LOOKUP_RESPONSE
    MSG_SNAPSHOT_PROGRESS_FWD = MESH_MSG_USER_START + 28,
    

    // Cloud Daemon -> Snapshot Daemon
    MSG_COMMIT_NODE_CMD = MESH_MSG_USER_START + 30,
    MSG_REBUILD_NODE_CMD = MESH_MSG_USER_START + 31,
    
    // Snapshot Daemon -> Cloud Daemon
    MSG_SNAPSHOT_PROGRESS = MESH_MSG_USER_START + 32,


    // Exodus -> Daemons
    MSG_TERMINATE = MESH_MSG_USER_START + 99,

};

#define MAX_ATTR_LEN 128
#define ATTR_FLAG_AUTHOR (1 << 0)
#define ATTR_FLAG_DESC   (1 << 1)
#define ATTR_FLAG_TAG    (1 << 2)

// --- Message Payloads ---


#define MAX_WORD_LEN 64
typedef struct {
    char target_word[MAX_WORD_LEN];
    char new_word[MAX_WORD_LEN];
} change_word_req_t;

// For MSG_QUERY_RESPONSE
typedef struct {
    int count;
    char word[MAX_WORD_LEN];
    int num_sentences;
    char sentences[0];
} query_response_t;

// For MSG_OPERATION_ACK
typedef struct {
    int success; // 1 for success, 0 for failure
    char details[128];
} ack_t;

typedef struct {
    long count;
} count_response_t;

#define MAX_NODE_NAME_LEN 64
#define MAX_PATH_LEN 256

// For MSG_ADD_NODE
typedef struct {
    char node_name[MAX_NODE_NAME_LEN];
    char path[MAX_PATH_LEN];
} add_node_req_t;

// For VIEW/ACTIVATE/DEACTIVATE_NODE
typedef struct {
    char node_name[MAX_NODE_NAME_LEN];
} node_req_t;

// For LIST_NODES_RESPONSE and VIEW_NODE_RESPONSE
// This is a header for a variable-length data payload
typedef struct {
    int item_count;
    char data[0]; // Flexible array of null-terminated strings
} list_resp_t;

typedef struct {
    char node_name[MAX_NODE_NAME_LEN];
    uint8_t flags; // Use flags to indicate which attrs are set
    char author[MAX_ATTR_LEN];
    char desc[MAX_ATTR_LEN];
    char tag[MAX_ATTR_LEN];
} attr_node_req_t;

// For MSG_INFO_NODE_RESPONSE
typedef struct {
    int success; // 1 if node found, 0 otherwise
    char author[MAX_ATTR_LEN];
    char desc[MAX_ATTR_LEN];
    char tag[MAX_ATTR_LEN];
    char current_version[MAX_NODE_NAME_LEN];
} info_node_resp_t;

// For MSG_SEARCH_ATTR
typedef enum {
    SEARCH_BY_AUTHOR,
    SEARCH_BY_TAG
} search_type_t;

typedef struct {
    search_type_t type;
    char target[MAX_ATTR_LEN];
} search_attr_req_t;


// For MSG_LOOKUP_RESPONSE
typedef struct {
    char node_name[MAX_NODE_NAME_LEN];
    char item_path[MAX_PATH_LEN];
} lookup_result_t;

typedef struct {
    char item_name[MAX_PATH_LEN];
} lookup_req_t;

// For MSG_PIN_ITEM
typedef struct {
    char pin_name[MAX_NODE_NAME_LEN];
    char item_name[MAX_PATH_LEN];
} pin_req_t;

// For MSG_UNPIN_ITEM
typedef struct {
    char pin_name[MAX_NODE_NAME_LEN];
} unpin_req_t;

// For MSG_COMMIT_NODE, MSG_REBUILD_NODE, and their _CMD variants
typedef struct {
    char node_name[MAX_NODE_NAME_LEN];
    char node_path[MAX_PATH_LEN];
    char version_tag[MAX_NODE_NAME_LEN];
} snapshot_cmd_t;

// For MSG_SNAPSHOT_PROGRESS and MSG_SNAPSHOT_PROGRESS_FWD
typedef struct {
    char version_tag[MAX_NODE_NAME_LEN];
    char status_message[128];
    int is_final; // 1 if this is the last report (success or fail), 0 otherwise
} snapshot_progress_t;



#endif // EXODUS_COMMON_H
