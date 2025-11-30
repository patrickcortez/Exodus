// gcc -Wall -O2 -fPIC -c cortez-channel.c -o cortez-channel.o -pthread -lrt
#ifndef CORTEZ_CHANNEL_H
#define CORTEZ_CHANNEL_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <sys/types.h>
#include <sys/uio.h>

typedef struct cortez_channel cortez_channel_t;
typedef struct cortez_msg cortez_msg_t;
typedef struct cortez_tx cortez_tx_t;

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
    volatile uint32_t lock;
    volatile uint32_t active_connections;
    volatile uint64_t head;
    volatile uint64_t tail;
    volatile uint64_t tx_head;
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
} cortez_channel_options_t;

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
} cortez_channel_stats_t;

// Joins or creates a communication channel.
cortez_channel_t* cortez_channel_join(const char* channel_name, const cortez_channel_options_t* options);
// Leaves a channel and cleans up resources.
int cortez_channel_leave(cortez_channel_t* ch);
// Attempts to recover a stale channel if the owner process has died.
int cortez_channel_recover(cortez_channel_t* ch);
// Writes a single contiguous payload to the channel.
int cortez_channel_write(cortez_channel_t* ch, uint16_t msg_type, const void* payload, uint32_t payload_size);
// Writes scattered data (from iovec) to the channel.
int cortez_channel_writev(cortez_channel_t* ch, uint16_t msg_type, const struct iovec* iov, int iovcnt);
// Reads the next message from the channel, waiting if necessary.
cortez_msg_t* cortez_channel_read(cortez_channel_t* ch, int timeout_ms);
// Peeks at the next message without consuming it.
cortez_msg_t* cortez_channel_peek(cortez_channel_t* ch);
// Releases a message and advances the read cursor.
int cortez_channel_msg_release(cortez_channel_t* ch, cortez_msg_t* msg);
// Begins a transactional write, reserving space in the buffer.
cortez_tx_t* cortez_channel_begin_write(cortez_channel_t* ch, uint32_t total_size);
// Commits a transactional write, making the message visible.
int cortez_channel_commit_write(cortez_channel_t* ch, cortez_tx_t* tx, uint16_t msg_type, const struct iovec* iov, int iovcnt);
// Aborts a transactional write, releasing the reservation.
void cortez_channel_abort_write(cortez_channel_t* ch, cortez_tx_t* tx);
// Gets the underlying file descriptor for the channel.
int cortez_channel_get_fd(cortez_channel_t* ch);
// Retrieves current statistics for the channel.
int cortez_channel_get_stats(cortez_channel_t* ch, cortez_channel_stats_t* stats);
// Converts a cortez error code to a human-readable string.
const char* cortez_channel_strerror(int err_code);
// Gets the last error that occurred on the channel.
int cortez_channel_get_last_error(cortez_channel_t* ch);

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
    return hdr ? hdr->timestamp : (struct timespec){0, 0};
}
#endif // CORTEZ_CHANNEL_H
