/*
 * Compile Command for library:
 * gcc -Wall -Wextra -O2 -c cortez-mesh.c -o cortez-mesh.o -pthread
 */
#ifndef CORTEZ_MESH_H
#define CORTEZ_MESH_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <pthread.h> // Added for mesh functionality

// Opaque type definitions for the public API
typedef struct cortez_ch cortez_ch_t;
typedef struct cortez_msg cortez_msg_t;
typedef struct cortez_tx cortez_tx_t;
typedef struct cortez_mesh cortez_mesh_t;
typedef struct cortez_write_handle cortez_write_handle_t; // New handle for zero-copy writes

// --- Struct Definitions ---
// These are needed by the public API and inline functions.

typedef struct {
    uint64_t magic;
    uint32_t total_len;
    uint32_t payload_len;
    uint16_t msg_type;
    uint16_t iov_count;
    pid_t sender_pid;
    struct timespec timestamp;
} CortezMessageHeader;

typedef struct {
    uint64_t magic;
    volatile uint32_t futex_word;
    size_t total_shm_size;
    size_t buffer_capacity;
    pid_t owner_pid;
    volatile uint32_t lock; // For recovery
    volatile uint32_t active_connections;
    volatile uint64_t head;
    volatile uint64_t tail;
    volatile uint64_t tx_head; // Transactional head
    volatile uint64_t messages_written;
    volatile uint64_t messages_read;
    volatile uint64_t bytes_written;
    volatile uint64_t bytes_read;
    volatile uint64_t write_contention_count;
    volatile uint64_t channel_recovered_count;
    char buffer[];
} CortezChannelHeader;

struct cortez_msg {
    const void* header;
    void* linear_buffer;
};

// --- Mesh-specific Message Types ---
// These are used on the internal registry channel for discovery and healing.
enum cortez_mesh_msg_types {
    MESH_MSG_USER_START = 100, // User-defined message types should start here
    MESH_MSG_REGISTER = 1,
    MESH_MSG_HEARTBEAT = 2,
    MESH_MSG_GOODBYE = 3,
};

// Structure for mesh registration/heartbeat payload
typedef struct {
    pid_t pid;
    char inbox_channel_name[64];
} cortez_mesh_peer_info_t;


// Error codes remain the same
enum cortez_error_codes {
    CORTEZ_OK = 0,
    CORTEZ_E_INVALID_ARG = -1,
    CORTEZ_E_NO_MEM = -2,
    CORTEZ_E_CHAN_EXISTS = -3,
    CORTEZ_E_CHAN_NOT_FOUND = -4,
    CORTEZ_E_SHM_MAP_FAILED = -5,
    CORTEZ_E_BAD_MAGIC = -6,
    CORTEZ_E_BUFFER_FULL = -7,
    CORTEZ_E_MSG_TOO_LARGE = -8,
    CORTEZ_E_TIMED_OUT = -9,
    CORTEZ_E_CORRUPT = -10,
    CORTEZ_E_IOCTL_FAILED = -11,
    CORTEZ_E_TX_IN_PROGRESS = -12,
    CORTEZ_E_CHAN_STALE = -13,
    CORTEZ_E_PEER_NOT_FOUND = -14, // New error for mesh
    CORTEZ_E_INTERNAL = -99,
};

typedef enum {
    CORTEZ_CREATE_OR_JOIN,
    CORTEZ_CREATE_ONLY,
    CORTEZ_JOIN_ONLY,
} cortez_create_policy;

typedef struct {
    size_t size;
    cortez_create_policy create_policy;
} cortez_options_t;

typedef struct {
    uint64_t messages_written;
    uint64_t messages_read;
    uint64_t bytes_written;
    uint64_t bytes_read;
    uint64_t write_contention_count;
    uint64_t channel_recovered_count;
    uint32_t active_connections;
    pid_t owner_pid;
    size_t buffer_capacity;
    size_t buffer_bytes_used;
} cortez_stats_t;

// =================================================================
// --- NEW MESH API ---
// =================================================================

/**
 * @brief Initializes a node and joins the mesh.
 * This creates a private inbox for the node, joins the public registry,
 * and starts a background thread for discovery and health checks.
 *
 * @param node_name A unique identifier for this node.
 * @param options Channel options for creating the node's private inbox.
 * @return A handle to the mesh, or NULL on failure.
 */
cortez_mesh_t* cortez_mesh_init(const char* node_name, const cortez_options_t* options);

/**
 * @brief Gracefully shuts down the node and leaves the mesh.
 * Sends a "goodbye" message, stops the background thread, and cleans up resources.
 *
 * @param mesh The mesh handle.
 * @return CORTEZ_OK on success.
 */
int cortez_mesh_shutdown(cortez_mesh_t* mesh);

/**
 * @brief (Original API) Sends a message to a specific peer in the mesh.
 * This function copies the payload into the shared memory buffer.
 *
 * @param mesh The mesh handle.
 * @param target_pid The PID of the destination node.
 * @param msg_type A user-defined message type (must be >= MESH_MSG_USER_START).
 * @param payload The message data.
 * @param payload_size The size of the message data.
 * @return CORTEZ_OK on success, or an error code.
 */
int cortez_mesh_send(cortez_mesh_t* mesh, pid_t target_pid, uint16_t msg_type, const void* payload, uint32_t payload_size);


// --- NEW ZERO-COPY MESH API ---

/**
 * @brief (Zero-Copy) Begins a message send transaction to a specific peer.
 * This reserves space in the peer's channel and returns a handle with direct
 * pointers into the shared memory buffer for writing the payload.
 *
 * @param mesh The mesh handle.
 * @param target_pid The PID of the destination node.
 * @param payload_size The exact size of the payload you intend to write.
 * @return A write handle on success, or NULL on failure (e.g., peer not found, buffer full).
 */
cortez_write_handle_t* cortez_mesh_begin_send_zc(cortez_mesh_t* mesh, pid_t target_pid, uint32_t payload_size);

/**
 * @brief (Zero-Copy) Commits a message send transaction.
 * After writing the payload to the buffer pointers from the handle, call this
 * to make the message visible to the receiver. The handle is freed by this call.
 *
 * @param handle The write handle from cortez_mesh_begin_send_zc.
 * @param msg_type A user-defined message type (must be >= MESH_MSG_USER_START).
 * @return CORTEZ_OK on success.
 */
int cortez_mesh_commit_send_zc(cortez_write_handle_t* handle, uint16_t msg_type);

/**
 * @brief (Zero-Copy) Aborts a message send transaction.
 * If you cannot complete the write, call this to release the reserved buffer space.
 * The handle is freed by this call.
 *
 * @param handle The write handle from cortez_mesh_begin_send_zc.
 */
void cortez_mesh_abort_send_zc(cortez_write_handle_t* handle);


/**
 * @brief Reads a message from this node's private inbox.
 *
 * @param mesh The mesh handle.
 * @param timeout_ms Timeout in milliseconds. -1 to wait forever, 0 to not block.
 * @return A message handle, or NULL if no message is available or on timeout/error.
 */
cortez_msg_t* cortez_mesh_read(cortez_mesh_t* mesh, int timeout_ms);

/**
 * @brief Prints a list of currently known, active peers in the mesh to stdout.
 *
 * @param mesh The mesh handle.
 */
void cortez_mesh_list_peers(cortez_mesh_t* mesh);

pid_t cortez_mesh_find_peer_by_name(cortez_mesh_t* mesh, const char* name);


/**
 * @brief Gets this node's own process ID.
 */
pid_t cortez_mesh_get_pid(cortez_mesh_t* mesh);


// =================================================================
// --- Original Channel-level API (still available for direct use) ---
// =================================================================
cortez_ch_t* cortez_join(const char* channel_name, const cortez_options_t* options);
int cortez_leave(cortez_ch_t* ch);
int cortez_channel_recover(cortez_ch_t* ch);

int cortez_write(cortez_ch_t* ch, uint16_t msg_type, const void* payload, uint32_t payload_size);
int cortez_writev(cortez_ch_t* ch, uint16_t msg_type, const struct iovec* iov, int iovcnt);
cortez_msg_t* cortez_read(cortez_ch_t* ch, int timeout_ms);
cortez_msg_t* cortez_peek(cortez_ch_t* ch);
int cortez_msg_release(cortez_ch_t* ch, cortez_msg_t* msg);

cortez_tx_t* cortez_begin_write(cortez_ch_t* ch, uint32_t total_size);
int cortez_commit_write(cortez_ch_t* ch, cortez_tx_t* tx, uint16_t msg_type, const struct iovec* iov, int iovcnt);
void cortez_abort_write(cortez_ch_t* ch, cortez_tx_t* tx);

// --- NEW ZERO-COPY CHANNEL API ---
cortez_write_handle_t* cortez_begin_write_zc(cortez_ch_t* ch, uint32_t payload_size);
int cortez_commit_write_zc(cortez_write_handle_t* handle, uint16_t msg_type);
void cortez_abort_write_zc(cortez_write_handle_t* handle);


int cortez_get_channel_fd(cortez_ch_t* ch);
int cortez_get_stats(cortez_ch_t* ch, cortez_stats_t* stats);
const char* cortez_strerror(int err_code);
int cortez_get_last_error(cortez_ch_t* ch);


// --- Inline Accessors for Message Data ---
static inline const CortezMessageHeader* cortez_msg_get_header(const cortez_msg_t* msg) {
    return msg ? (const CortezMessageHeader*)msg->header : NULL;
}

static inline const void* cortez_msg_payload(const cortez_msg_t* msg) {
    const CortezMessageHeader* hdr = cortez_msg_get_header(msg);
    return hdr ? ((const char*)hdr + sizeof(CortezMessageHeader)) : NULL;
}

static inline uint32_t cortez_msg_payload_size(const cortez_msg_t* msg) {
    const CortezMessageHeader* hdr = cortez_msg_get_header(msg);
    return hdr ? hdr->payload_len : 0;
}

static inline uint16_t cortez_msg_type(const cortez_msg_t* msg) {
    const CortezMessageHeader* hdr = cortez_msg_get_header(msg);
    return hdr ? hdr->msg_type : 0;
}

static inline pid_t cortez_msg_sender_pid(const cortez_msg_t* msg) {
    const CortezMessageHeader* hdr = cortez_msg_get_header(msg);
    return hdr ? hdr->sender_pid : -1;
}

static inline struct timespec cortez_msg_timestamp(const cortez_msg_t* msg) {
    const CortezMessageHeader* hdr = cortez_msg_get_header(msg);
    if (hdr) return hdr->timestamp;
    return (struct timespec){0, 0};
}

// --- Inline Accessors for Zero-Copy Write Handle ---
// The concrete implementation is hidden in the .c file.
struct cortez_write_handle {
    cortez_ch_t* ch;
    cortez_tx_t* tx;
    void* part1;
    size_t part1_size;
    void* part2;
    size_t part2_size;
};

/**
 * @brief Gets the first (and possibly only) pointer to the reserved payload buffer.
 *
 * @param h The write handle.
 * @param size Output parameter for the size of this buffer part.
 * @return A pointer to write payload data to.
 */
static inline void* cortez_write_handle_get_part1(cortez_write_handle_t* h, size_t* size) {
    if (!h) return NULL;
    if (size) *size = h->part1_size;
    return h->part1;
}

/**
 * @brief Gets the second pointer to the reserved payload buffer.
 * This will only be non-NULL if the payload wraps around the ring buffer.
 *
 * @param h The write handle.
 * @param size Output parameter for the size of this buffer part.
 * @return A pointer to write the rest of the payload data to, or NULL if the
 * payload fits entirely in part1.
 */
static inline void* cortez_write_handle_get_part2(cortez_write_handle_t* h, size_t* size) {
    if (!h) return NULL;
    if (size) *size = h->part2_size;
    return h->part2;
}

int cortez_mesh_msg_release(cortez_mesh_t* mesh, cortez_msg_t* msg);

#endif // CORTEZ_MESH_H
