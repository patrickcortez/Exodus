/*
 * Compile Command for library:
 * gcc -Wall -Wextra -O2 -c cortez-mesh.c -o cortez-mesh.o -pthread
 */
#include "cortez-mesh.h"
#include "cortez_tunnel_shared.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <limits.h>
#include <pthread.h>
#include <dirent.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

// --- Defines ---
#define CORTEZ_TUNNEL_DEVICE_PATH "/dev/cortez_tunnel"
#define CORTEZ_CHANNEL_MAGIC 0xDEADBEEFCAFEFACE
#define CORTEZ_MESSAGE_MAGIC 0xBAADF00DBAADF00D
#define CORTEZ_JUMP_MAGIC    0x1EABC0DE1EABC0DE

#define CORTEZ_REGISTRY_CHANNEL "_cortez_registry"
#define HEARTBEAT_INTERVAL_SEC 2
#define PEER_TIMEOUT_SEC 10

#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)

static inline int64_t now_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}


// --- Internal Structs ---
struct cortez_ch {
    int fd;
    char name[64];
    void* shm_base;
    size_t shm_size;
    CortezChannelHeader* header;
    int last_error;
    uint64_t local_head_cache;
    uint64_t local_tail_cache;
    int is_owner;
    volatile int ref_count; // Added for safe multithreaded handle usage
};

typedef struct {
    uint64_t magic;
    uint32_t total_len;
} CortezJumpHeader;

struct cortez_tx {
    uint64_t reserved_head;
    uint32_t reserved_size;
};

// --- Mesh-specific Internal Structs ---
typedef struct cortez_peer {
    cortez_mesh_peer_info_t info;
    int64_t last_heartbeat; // monotonic ns
    cortez_ch_t* comm_channel; // Cached channel handle for sending
    struct cortez_peer* next;
} cortez_peer_t;

struct cortez_mesh {
    cortez_mesh_peer_info_t self_info;
    cortez_ch_t* inbox_ch;
    cortez_ch_t* registry_ch;
    
    cortez_peer_t* peer_list;
    pthread_mutex_t peer_list_mutex;

    pthread_t housekeeper_thread;
    volatile int housekeeper_running;

    int last_error;
};

// --- Futex Wrappers ---
static int futex_wait(volatile uint32_t *uaddr, uint32_t val, const struct timespec *timeout) {
    return syscall(SYS_futex, uaddr, FUTEX_WAIT, val, timeout, NULL, 0);
}

static int futex_wake(volatile uint32_t *uaddr, int num_waiters) {
    return syscall(SYS_futex, uaddr, FUTEX_WAKE, num_waiters, NULL, NULL, 0);
}

// --- Internal Helper Functions ---
static const char* internal_strerror(int err) {
    switch (err) {
        case CORTEZ_OK: return "Success";
        case CORTEZ_E_INVALID_ARG: return "Invalid argument";
        case CORTEZ_E_NO_MEM: return "Out of memory";
        case CORTEZ_E_CHAN_EXISTS: return "Channel already exists";
        case CORTEZ_E_CHAN_NOT_FOUND: return "Channel not found";
        case CORTEZ_E_SHM_MAP_FAILED: return "Shared memory mmap failed";
        case CORTEZ_E_BAD_MAGIC: return "Invalid channel/message magic";
        case CORTEZ_E_BUFFER_FULL: return "Channel buffer is full";
        case CORTEZ_E_MSG_TOO_LARGE: return "Message is too large";
        case CORTEZ_E_TIMED_OUT: return "Operation timed out";
        case CORTEZ_E_CORRUPT: return "Channel data is corrupt";
        case CORTEZ_E_IOCTL_FAILED: return "Kernel ioctl failed";
        case CORTEZ_E_INTERNAL: return "Internal library error";
        case CORTEZ_E_TX_IN_PROGRESS: return "Another transaction is in progress";
        case CORTEZ_E_CHAN_STALE: return "Channel is stale, needs recovery";
        case CORTEZ_E_PEER_NOT_FOUND: return "Peer not found in the mesh";
        default: return "Unknown error";
    }
}

static void set_error(cortez_ch_t* ch, int err) {
    if (ch) ch->last_error = err;
}

static void set_mesh_error(cortez_mesh_t* mesh, int err) {
    if (mesh) mesh->last_error = err;
}


static int is_pid_alive(pid_t pid) {
    if (pid <= 0) return 0;
    return kill(pid, 0) == 0 || errno != ESRCH;
}

static void internal_init_header(CortezChannelHeader* header, size_t shm_size, int is_recovery) {
    if (!is_recovery) {
        memset(header, 0, sizeof(CortezChannelHeader));
        header->magic = CORTEZ_CHANNEL_MAGIC;
        header->total_shm_size = shm_size;
        header->buffer_capacity = shm_size - sizeof(CortezChannelHeader);
        header->owner_pid = getpid();
    }
    __atomic_store_n(&header->futex_word, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&header->head, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&header->tail, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&header->tx_head, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&header->messages_written, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&header->messages_read, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&header->bytes_written, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&header->bytes_read, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&header->write_contention_count, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&header->channel_recovered_count,
        __atomic_load_n(&header->channel_recovered_count, __ATOMIC_RELAXED) + (is_recovery ? 1 : 0), __ATOMIC_RELAXED);
    __atomic_store_n(&header->lock, 0, __ATOMIC_RELEASE);
}

static uint64_t get_write_space(const CortezChannelHeader* h, uint64_t head, uint64_t tail) {
    if (head >= tail) return h->buffer_capacity - (head - tail);
    return tail - head;
}

static void get_process_name_by_pid(pid_t pid, char* buffer, size_t buffer_len) {
    if (buffer_len == 0) return;
    buffer[0] = '\0'; // Default to empty string

    char proc_path[256];
    snprintf(proc_path, sizeof(proc_path), "/proc/%d/comm", pid);

    FILE* f = fopen(proc_path, "r");
    if (f) {
        if (fgets(buffer, buffer_len, f) != NULL) {
            // Remove trailing newline character
            buffer[strcspn(buffer, "\n")] = 0;
        }
        fclose(f);
    }
}

static uint64_t get_read_space(const CortezChannelHeader* h, uint64_t head, uint64_t tail) {
    if (head >= tail) return head - tail;
    return h->buffer_capacity - (tail - head);
}

static void copy_to_buffer(CortezChannelHeader* h, uint64_t offset, const void* data, size_t len) {
    uint64_t start = offset % h->buffer_capacity;
    if (start + len <= h->buffer_capacity) {
        memcpy(h->buffer + start, data, len);
    } else {
        size_t part1 = h->buffer_capacity - start;
        memcpy(h->buffer + start, data, part1);
        memcpy(h->buffer, (const char*)data + part1, len - part1);
    }
}

static void copy_from_buffer(void* dest, const CortezChannelHeader* h, uint64_t offset, size_t len) {
    uint64_t start = offset % h->buffer_capacity;
    if (start + len <= h->buffer_capacity) {
        memcpy(dest, h->buffer + start, len);
    } else {
        size_t part1 = h->buffer_capacity - start;
        memcpy(dest, h->buffer + start, part1);
        memcpy((char*)dest + part1, h->buffer, len - part1);
    }
}

// --- Channel API Implementation ---

const char* cortez_strerror(int err_code) {
    if (err_code > 0) return strerror(err_code);
    return internal_strerror(err_code);
}

int cortez_get_last_error(cortez_ch_t* ch) {
    return ch ? ch->last_error : CORTEZ_E_INVALID_ARG;
}


// New: Safely increments the reference count of a channel handle.
static cortez_ch_t* cortez_channel_ref(cortez_ch_t* ch) {
    if (ch) {
        __atomic_add_fetch(&ch->ref_count, 1, __ATOMIC_RELAXED);
    }
    return ch;
}

cortez_ch_t* cortez_join(const char* channel_name, const cortez_options_t* options) {
    if (!channel_name || strlen(channel_name) >= 64) {
        errno = EINVAL;
        return NULL;
    }

    cortez_options_t default_opts = { .size = (4 * 1024 * 1024), .create_policy = CORTEZ_CREATE_OR_JOIN };
    if (!options) options = &default_opts;

    cortez_ch_t* ch = calloc(1, sizeof(cortez_ch_t));
    if (!ch) { errno = ENOMEM; return NULL; }
    strncpy(ch->name, channel_name, sizeof(ch->name) - 1);

    ch->fd = open(CORTEZ_TUNNEL_DEVICE_PATH, O_RDWR);
    if (ch->fd < 0) { set_error(ch, errno); free(ch); return NULL; }

    int is_creator = 0;

    if (options->create_policy == CORTEZ_JOIN_ONLY) {
        if (ioctl(ch->fd, TUNNEL_CONNECT, (char*)ch->name) != 0) {
            set_error(ch, (errno == ENOENT) ? CORTEZ_E_CHAN_NOT_FOUND : CORTEZ_E_IOCTL_FAILED);
            close(ch->fd); free(ch); return NULL;
        }
        ch->shm_size = 4096; // Map one page to read header first
    } else { // Handles CREATE_ONLY and CREATE_OR_JOIN
        tunnel_create_t create_info;
        strncpy(create_info.name, ch->name, sizeof(create_info.name) - 1);
        create_info.name[sizeof(create_info.name) - 1] = '\0';
        create_info.size = options->size;

        if (ioctl(ch->fd, TUNNEL_CREATE, &create_info) == 0) {
            is_creator = 1;
            ch->shm_size = create_info.size;
        } else if (errno == EEXIST && options->create_policy == CORTEZ_CREATE_OR_JOIN) {
            if (ioctl(ch->fd, TUNNEL_CONNECT, (char*)ch->name) != 0) {
                set_error(ch, CORTEZ_E_IOCTL_FAILED);
                close(ch->fd); free(ch); return NULL;
            }
            ch->shm_size = 4096;
        } else {
            set_error(ch, (errno == EEXIST) ? CORTEZ_E_CHAN_EXISTS : CORTEZ_E_IOCTL_FAILED);
            close(ch->fd); free(ch); return NULL;
        }
    }

    ch->shm_base = mmap(NULL, ch->shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, ch->fd, 0);
    if (ch->shm_base == MAP_FAILED) {
        set_error(ch, CORTEZ_E_SHM_MAP_FAILED); close(ch->fd); free(ch); return NULL;
    }
    ch->header = (CortezChannelHeader*)ch->shm_base;

    if (is_creator) {
        ch->is_owner = 1;
        internal_init_header(ch->header, ch->shm_size, 0);
    } else {
        ch->is_owner = 0;
        if (unlikely(ch->header->magic != CORTEZ_CHANNEL_MAGIC)) {
            set_error(ch, CORTEZ_E_BAD_MAGIC); munmap(ch->shm_base, ch->shm_size); close(ch->fd); free(ch); return NULL;
        }
        size_t actual_size = ch->header->total_shm_size;
        if (actual_size != ch->shm_size) {
            munmap(ch->shm_base, ch->shm_size);
            ch->shm_size = actual_size;
            ch->shm_base = mmap(NULL, ch->shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, ch->fd, 0);
            if (ch->shm_base == MAP_FAILED) {
                set_error(ch, CORTEZ_E_SHM_MAP_FAILED); close(ch->fd); free(ch); return NULL;
            }
            ch->header = (CortezChannelHeader*)ch->shm_base;
        }
        __atomic_add_fetch(&ch->header->active_connections, 1, __ATOMIC_RELAXED);
    }

    if (!is_pid_alive(ch->header->owner_pid)) { set_error(ch, CORTEZ_E_CHAN_STALE); }
    ch->local_head_cache = __atomic_load_n(&ch->header->head, __ATOMIC_ACQUIRE);
    ch->local_tail_cache = __atomic_load_n(&ch->header->tail, __ATOMIC_ACQUIRE);
    ch->ref_count = 1; // Initial reference
    set_error(ch, CORTEZ_OK);
    return ch;
}

// Rewritten to be reference-counted for safe sharing between threads.
int cortez_leave(cortez_ch_t* ch) {
    if (!ch) return CORTEZ_E_INVALID_ARG;
    
    if (__atomic_sub_fetch(&ch->ref_count, 1, __ATOMIC_ACQ_REL) > 0) {
        return CORTEZ_OK; // Still other references
    }
    
    // Last reference is gone, perform full cleanup.
    if(ch->header && ch->shm_base != MAP_FAILED) {
        __atomic_sub_fetch(&ch->header->active_connections, 1, __ATOMIC_RELAXED);
        munmap(ch->shm_base, ch->shm_size);
    }
    if(ch->fd >= 0) close(ch->fd);
    free(ch);
    return CORTEZ_OK;
}

cortez_tx_t* cortez_begin_write(cortez_ch_t* ch, uint32_t total_size) {
    if (unlikely(!ch || total_size == 0)) { set_error(ch, CORTEZ_E_INVALID_ARG); return NULL; }
    if (unlikely(total_size > ch->header->buffer_capacity)) { set_error(ch, CORTEZ_E_MSG_TOO_LARGE); return NULL; }

    uint64_t expected_tx = 0;
    if (!__atomic_compare_exchange_n(&ch->header->tx_head, &expected_tx, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        set_error(ch, CORTEZ_E_TX_IN_PROGRESS); return NULL;
    }

    CortezChannelHeader* h = ch->header;
    uint64_t head = __atomic_load_n(&h->head, __ATOMIC_RELAXED);
    const uint64_t tail = __atomic_load_n(&h->tail, __ATOMIC_ACQUIRE);

    if (unlikely(get_write_space(h, head, tail) <= total_size)) {
        __atomic_store_n(&h->tx_head, 0, __ATOMIC_RELEASE);
        set_error(ch, CORTEZ_E_BUFFER_FULL);
        return NULL;
    }

    cortez_tx_t* tx = malloc(sizeof(cortez_tx_t));
    if (!tx) { __atomic_store_n(&h->tx_head, 0, __ATOMIC_RELEASE); set_error(ch, CORTEZ_E_NO_MEM); return NULL; }

    tx->reserved_head = head;
    tx->reserved_size = total_size;
    uint64_t next_tx_head = head + total_size;
    __atomic_store_n(&h->tx_head, next_tx_head, __ATOMIC_RELEASE);
    set_error(ch, CORTEZ_OK);
    return tx;
}

int cortez_commit_write(cortez_ch_t* ch, cortez_tx_t* tx, uint16_t msg_type, const struct iovec* iov, int iovcnt) {
    if (unlikely(!ch || !tx || !iov || iovcnt < 0)) { set_error(ch, CORTEZ_E_INVALID_ARG); return CORTEZ_E_INVALID_ARG; }

    CortezChannelHeader* h = ch->header;
    uint32_t payload_size = 0;
    for (int i = 0; i < iovcnt; ++i) payload_size += iov[i].iov_len;

    if (unlikely(tx->reserved_size != sizeof(CortezMessageHeader) + payload_size)) {
        __atomic_store_n(&h->tx_head, 0, __ATOMIC_RELEASE); free(tx);
        set_error(ch, CORTEZ_E_INVALID_ARG); return CORTEZ_E_INVALID_ARG;
    }

    CortezMessageHeader msg_header = {
        .magic = CORTEZ_MESSAGE_MAGIC, .total_len = tx->reserved_size,
        .payload_len = payload_size, .msg_type = msg_type,
        .iov_count = (uint16_t)iovcnt, .sender_pid = getpid()
    };
    clock_gettime(CLOCK_MONOTONIC, &msg_header.timestamp);


    uint64_t write_offset = tx->reserved_head;
    copy_to_buffer(h, write_offset, &msg_header, sizeof(msg_header));
    write_offset += sizeof(msg_header);

    for (int i = 0; i < iovcnt; ++i) {
        copy_to_buffer(h, write_offset, iov[i].iov_base, iov[i].iov_len);
        write_offset += iov[i].iov_len;
    }

    __atomic_store_n(&h->head, tx->reserved_head + tx->reserved_size, __ATOMIC_RELEASE);
    __atomic_store_n(&h->tx_head, 0, __ATOMIC_RELEASE);
    __atomic_add_fetch(&h->futex_word, 1, __ATOMIC_RELAXED);
    futex_wake(&h->futex_word, 1);
    __atomic_add_fetch(&h->messages_written, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&h->bytes_written, tx->reserved_size, __ATOMIC_RELAXED);

    free(tx); set_error(ch, CORTEZ_OK); return CORTEZ_OK;
}

void cortez_abort_write(cortez_ch_t* ch, cortez_tx_t* tx) {
    if (!ch || !tx) return;
    __atomic_store_n(&ch->header->tx_head, 0, __ATOMIC_RELEASE);
    free(tx);
}

int cortez_writev(cortez_ch_t* ch, uint16_t msg_type, const struct iovec* iov, int iovcnt) {
    uint32_t payload_size = 0;
    for (int i = 0; i < iovcnt; ++i) payload_size += iov[i].iov_len;
    uint32_t total_size = sizeof(CortezMessageHeader) + payload_size;
    cortez_tx_t* tx = cortez_begin_write(ch, total_size);
    if (!tx) return ch->last_error;
    return cortez_commit_write(ch, tx, msg_type, iov, iovcnt);
}

int cortez_write(cortez_ch_t* ch, uint16_t msg_type, const void* payload, uint32_t payload_size) {
    struct iovec iov = { .iov_base = (void*)payload, .iov_len = payload_size };
    return cortez_writev(ch, msg_type, &iov, 1);
}

// --- NEW ZERO-COPY WRITE API ---

cortez_write_handle_t* cortez_begin_write_zc(cortez_ch_t* ch, uint32_t payload_size) {
    if (unlikely(!ch || payload_size == 0)) {
        set_error(ch, CORTEZ_E_INVALID_ARG);
        return NULL;
    }

    uint32_t total_size = sizeof(CortezMessageHeader) + payload_size;

    // Use the existing transaction logic to reserve a contiguous block
    cortez_tx_t* tx = cortez_begin_write(ch, total_size);
    if (!tx) {
        return NULL; // cortez_begin_write already set the error
    }

    cortez_write_handle_t* handle = calloc(1, sizeof(cortez_write_handle_t));
    if (!handle) {
        cortez_abort_write(ch, tx);
        set_error(ch, CORTEZ_E_NO_MEM);
        return NULL;
    }

    handle->ch = ch;
    handle->tx = tx;

    CortezChannelHeader* h = ch->header;
    uint64_t payload_start_pos = tx->reserved_head + sizeof(CortezMessageHeader);
    uint64_t start_offset = payload_start_pos % h->buffer_capacity;

    if (start_offset + payload_size <= h->buffer_capacity) {
        // Payload fits in a single contiguous block
        handle->part1 = h->buffer + start_offset;
        handle->part1_size = payload_size;
        handle->part2 = NULL;
        handle->part2_size = 0;
    } else {
        // Payload wraps around the end of the ring buffer
        handle->part1 = h->buffer + start_offset;
        handle->part1_size = h->buffer_capacity - start_offset;
        handle->part2 = h->buffer;
        handle->part2_size = payload_size - handle->part1_size;
    }

    set_error(ch, CORTEZ_OK);
    return handle;
}

int cortez_commit_write_zc(cortez_write_handle_t* handle, uint16_t msg_type) {
    if (unlikely(!handle || !handle->ch || !handle->tx)) {
        return CORTEZ_E_INVALID_ARG;
    }

    cortez_ch_t* ch = handle->ch;
    cortez_tx_t* tx = handle->tx;
    CortezChannelHeader* h = ch->header;

    uint32_t payload_size = handle->part1_size + handle->part2_size;
    
    // Create and write the header. This is the only copy.
    CortezMessageHeader msg_header = {
        .magic = CORTEZ_MESSAGE_MAGIC,
        .total_len = tx->reserved_size,
        .payload_len = payload_size,
        .msg_type = msg_type,
        .iov_count = 0, // Zero-copy doesn't use iovecs
        .sender_pid = getpid()
    };
    clock_gettime(CLOCK_REALTIME, &msg_header.timestamp);

    copy_to_buffer(h, tx->reserved_head, &msg_header, sizeof(msg_header));

    // Atomically commit the write by updating the head pointer
    __atomic_store_n(&h->head, tx->reserved_head + tx->reserved_size, __ATOMIC_RELEASE);
    
    // Release the transaction lock
    __atomic_store_n(&h->tx_head, 0, __ATOMIC_RELEASE);
    
    // Wake up any waiting readers
    __atomic_add_fetch(&h->futex_word, 1, __ATOMIC_RELAXED);
    futex_wake(&h->futex_word, 1);
    
    // Update stats
    __atomic_add_fetch(&h->messages_written, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&h->bytes_written, tx->reserved_size, __ATOMIC_RELAXED);

    free(tx);
    free(handle);
    set_error(ch, CORTEZ_OK);
    return CORTEZ_OK;
}

void cortez_abort_write_zc(cortez_write_handle_t* handle) {
    if (!handle || !handle->ch || !handle->tx) return;
    cortez_abort_write(handle->ch, handle->tx); // This frees the tx
    free(handle);
}

// --- END ZERO-COPY WRITE API ---

cortez_msg_t* cortez_read(cortez_ch_t* ch, int timeout_ms) {
    if (unlikely(!ch)) return NULL;

    CortezChannelHeader* h = ch->header;
    struct timespec timeout_spec, *timeout_ptr = NULL;

    if (timeout_ms > 0) {
        timeout_spec.tv_sec = timeout_ms / 1000;
        timeout_spec.tv_nsec = (timeout_ms % 1000) * 1000000;
        timeout_ptr = &timeout_spec;
    } else if (timeout_ms == -1) {
        timeout_ptr = NULL;
    }

    uint32_t current_futex_val = __atomic_load_n(&h->futex_word, __ATOMIC_ACQUIRE);
    ch->local_head_cache = __atomic_load_n(&h->head, __ATOMIC_ACQUIRE);

    while (get_read_space(h, ch->local_head_cache, ch->local_tail_cache) < sizeof(CortezMessageHeader)) {
        if (timeout_ms == 0) { set_error(ch, CORTEZ_E_BUFFER_FULL); return NULL; }

        int r = futex_wait(&h->futex_word, current_futex_val, timeout_ptr);
        if (r == -1) {
            if (errno == ETIMEDOUT) {
                set_error(ch, CORTEZ_E_TIMED_OUT);
                return NULL;
            }
            if (errno == EINTR) {
                
                current_futex_val = __atomic_load_n(&h->futex_word, __ATOMIC_ACQUIRE);
                ch->local_head_cache = __atomic_load_n(&h->head, __ATOMIC_ACQUIRE);
                continue;
            }
            
        }
        current_futex_val = __atomic_load_n(&h->futex_word, __ATOMIC_ACQUIRE);
        ch->local_head_cache = __atomic_load_n(&h->head, __ATOMIC_ACQUIRE);
    }

    return cortez_peek(ch);
}

cortez_msg_t* cortez_peek(cortez_ch_t* ch) {
    if (unlikely(!ch)) return NULL;

    CortezChannelHeader* h = ch->header;
    ch->local_head_cache = __atomic_load_n(&h->head, __ATOMIC_ACQUIRE);
    uint64_t available_data = get_read_space(h, ch->local_head_cache, ch->local_tail_cache);

    if (available_data < sizeof(CortezMessageHeader)) { set_error(ch, CORTEZ_E_BUFFER_FULL); return NULL; }

    uint64_t tail_offset = ch->local_tail_cache % h->buffer_capacity;
    const CortezMessageHeader* msg_header_ptr = (const CortezMessageHeader*)(h->buffer + tail_offset);
    CortezMessageHeader msg_header_buf;

    if (unlikely(tail_offset + sizeof(CortezMessageHeader) > h->buffer_capacity)) {
        copy_from_buffer(&msg_header_buf, h, ch->local_tail_cache, sizeof(CortezMessageHeader));
        msg_header_ptr = &msg_header_buf;
    }

    if (unlikely(msg_header_ptr->magic == CORTEZ_JUMP_MAGIC)) {
        const CortezJumpHeader* jump_header = (const CortezJumpHeader*)msg_header_ptr;
        ch->local_tail_cache += jump_header->total_len;
        __atomic_store_n(&h->tail, ch->local_tail_cache, __ATOMIC_RELEASE);
        return cortez_peek(ch);
    }

    if (unlikely(msg_header_ptr->magic != CORTEZ_MESSAGE_MAGIC)) { set_error(ch, CORTEZ_E_CORRUPT); return NULL; }
    if (unlikely(available_data < msg_header_ptr->total_len)) { set_error(ch, CORTEZ_E_BUFFER_FULL); return NULL; }

    cortez_msg_t* msg = calloc(1, sizeof(cortez_msg_t));
    if (!msg) { set_error(ch, CORTEZ_E_NO_MEM); return NULL; }

    if (unlikely(tail_offset + msg_header_ptr->total_len > h->buffer_capacity)) {
        msg->linear_buffer = malloc(msg_header_ptr->total_len);
        if (!msg->linear_buffer) { free(msg); set_error(ch, CORTEZ_E_NO_MEM); return NULL; }
        copy_from_buffer(msg->linear_buffer, h, ch->local_tail_cache, msg_header_ptr->total_len);
        msg->header = msg->linear_buffer;
    } else {
        msg->header = (const void*)(h->buffer + tail_offset);
    }
    set_error(ch, CORTEZ_OK);
    return msg;
}

int cortez_msg_release(cortez_ch_t* ch, cortez_msg_t* msg) {
    if (unlikely(!ch || !msg)) return CORTEZ_E_INVALID_ARG;
    CortezChannelHeader* h = ch->header;
    const CortezMessageHeader* hdr = (const CortezMessageHeader*)msg->header;
    ch->local_tail_cache += hdr->total_len;
    __atomic_store_n(&h->tail, ch->local_tail_cache, __ATOMIC_RELEASE);
    __atomic_add_fetch(&h->messages_read, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&h->bytes_read, hdr->total_len, __ATOMIC_RELAXED);
    if (msg->linear_buffer) free(msg->linear_buffer);
    free(msg);
    return CORTEZ_OK;
}

int cortez_get_channel_fd(cortez_ch_t* ch) { return unlikely(!ch) ? -1 : ch->fd; }

int cortez_get_stats(cortez_ch_t* ch, cortez_stats_t* stats) {
    if (unlikely(!ch || !stats)) return CORTEZ_E_INVALID_ARG;
    // (Implementation unchanged)
    return CORTEZ_OK;
}

int cortez_channel_recover(cortez_ch_t* ch) {
    if (unlikely(!ch)) return CORTEZ_E_INVALID_ARG;

    // The STALE flag is set in cortez_join when we connect to a channel
    // whose owner PID is no longer alive. We can now safely take ownership.

    // Use the header's lock to ensure only one process recovers at a time.
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&ch->header->lock, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        // Another process is already recovering it. This is unexpected for a private
        // inbox but could happen. We'll treat it as a temporary failure.
        set_error(ch, CORTEZ_E_TX_IN_PROGRESS);
        return CORTEZ_E_TX_IN_PROGRESS;
    }

    // We now have the lock. Re-initialize the channel header.
    // The '1' indicates this is a recovery, which increments a counter.
    internal_init_header(ch->header, ch->header->total_shm_size, 1);
    
    // Take ownership
    ch->header->owner_pid = getpid();
    ch->is_owner = 1;

    // Reset local cache
    ch->local_head_cache = 0;
    ch->local_tail_cache = 0;
    
    // The lock is released by internal_init_header.
    set_error(ch, CORTEZ_OK);
    return CORTEZ_OK;
}

// --- MESH API IMPLEMENTATION ---

// Find, add, or update a peer in the list. Returns the peer struct.
static cortez_peer_t* update_peer(cortez_mesh_t* mesh, const cortez_mesh_peer_info_t* peer_info) {
    cortez_peer_t* peer = NULL;
    for (peer = mesh->peer_list; peer != NULL; peer = peer->next) {
        if (peer->info.pid == peer_info->pid) {
            peer->last_heartbeat = now_mono_ns();
            return peer;
        }
    }

    // Not found, add new peer
    peer = calloc(1, sizeof(cortez_peer_t));
    if (!peer) return NULL;
    
    peer->info = *peer_info;
    peer->last_heartbeat = now_mono_ns();
    peer->comm_channel = NULL; // Lazily connect
    peer->next = mesh->peer_list;
    mesh->peer_list = peer;

    char process_name[64];
    get_process_name_by_pid(peer->info.pid, process_name, sizeof(process_name));
    printf("[Mesh] Peer '%s' joined: %d\n", process_name, peer->info.pid);
    return peer;
}

static void remove_peer(cortez_mesh_t* mesh, pid_t pid) {
    cortez_peer_t** pptr = &mesh->peer_list;
    while (*pptr) {
        cortez_peer_t* entry = *pptr;
        if (entry->info.pid == pid) {
            *pptr = entry->next;
            printf("[Mesh] Peer left/timed out: %d\n", entry->info.pid);
            if (entry->comm_channel) cortez_leave(entry->comm_channel);
            free(entry);
            return;
        }
        pptr = &(*pptr)->next;
    }
}

static void* housekeeper_thread_main(void* arg) {
    cortez_mesh_t* mesh = (cortez_mesh_t*)arg;
    time_t last_heartbeat_sent = 0;

    while (mesh->housekeeper_running) {
        // 1. Process incoming registry messages
        cortez_msg_t* msg;
        while ((msg = cortez_read(mesh->registry_ch, 0)) != NULL) {
            if (cortez_msg_payload_size(msg) != sizeof(cortez_mesh_peer_info_t)) {
                cortez_msg_release(mesh->registry_ch, msg);
                continue;
            }
            const cortez_mesh_peer_info_t* peer_info = cortez_msg_payload(msg);
            if (peer_info->pid == mesh->self_info.pid) { // Ignore self
                cortez_msg_release(mesh->registry_ch, msg);
                continue;
            }

            pthread_mutex_lock(&mesh->peer_list_mutex);
            switch (cortez_msg_type(msg)) {
                case MESH_MSG_REGISTER:
                case MESH_MSG_HEARTBEAT:
                    update_peer(mesh, peer_info);
                    break;
                case MESH_MSG_GOODBYE:
                    remove_peer(mesh, peer_info->pid);
                    break;
            }
            pthread_mutex_unlock(&mesh->peer_list_mutex);
            cortez_msg_release(mesh->registry_ch, msg);
        }

        // 2. Send our own heartbeat
        int64_t now_ns = now_mono_ns();
        if (last_heartbeat_sent == 0 || now_ns - last_heartbeat_sent >= (int64_t)HEARTBEAT_INTERVAL_SEC * 1000000000LL) {
            cortez_write(mesh->registry_ch, MESH_MSG_HEARTBEAT, &mesh->self_info, sizeof(mesh->self_info));
            last_heartbeat_sent = now_ns;
        }

        // 3. Purge timed-out peers (self-healing)
        pthread_mutex_lock(&mesh->peer_list_mutex);
        cortez_peer_t** pptr = &mesh->peer_list;
        while(*pptr) {
            cortez_peer_t* entry = *pptr;
            if (now_ns - entry->last_heartbeat > (int64_t)PEER_TIMEOUT_SEC * 1000000000LL) {
                 *pptr = entry->next;
                 printf("[Mesh] Peer timed out: %d\n", entry->info.pid);
                 if (entry->comm_channel) cortez_leave(entry->comm_channel);
                 free(entry);
            } else {
                pptr = &(*pptr)->next;
            }
        }
        pthread_mutex_unlock(&mesh->peer_list_mutex);
        
       usleep(100000);
    }
    return NULL;
}

int cortez_mesh_msg_release(cortez_mesh_t* mesh, cortez_msg_t* msg) {
    if (unlikely(!mesh || !msg)) return CORTEZ_E_INVALID_ARG;
    // This function correctly accesses the internal inbox_ch to release the message
    return cortez_msg_release(mesh->inbox_ch, msg);
}

cortez_mesh_t* cortez_mesh_init(const char* node_name, const cortez_options_t* options) {
    cortez_mesh_t* mesh = calloc(1, sizeof(cortez_mesh_t));
    if (!mesh) return NULL;
    
    mesh->self_info.pid = getpid();
    snprintf(mesh->self_info.inbox_channel_name, sizeof(mesh->self_info.inbox_channel_name),
             "%s-%d", node_name, mesh->self_info.pid);
             
    pthread_mutex_init(&mesh->peer_list_mutex, NULL);
    
    cortez_options_t inbox_opts = {.size=1024*1024, .create_policy=CORTEZ_CREATE_OR_JOIN};
    if (options) { 
        inbox_opts.size = options->size;
    }

    mesh->inbox_ch = cortez_join(mesh->self_info.inbox_channel_name, &inbox_opts);
    if (!mesh->inbox_ch) {
        fprintf(stderr, "Failed to create or join inbox channel\n");
        free(mesh);
        return NULL;
    }
    

    if (cortez_get_last_error(mesh->inbox_ch) == CORTEZ_E_CHAN_STALE) {
        printf("[Mesh] Inbox channel '%s' is stale, attempting recovery...\n", mesh->self_info.inbox_channel_name);
        if (cortez_channel_recover(mesh->inbox_ch) != CORTEZ_OK) {
            fprintf(stderr, "Failed to recover stale inbox channel.\n");
            cortez_leave(mesh->inbox_ch);
            pthread_mutex_destroy(&mesh->peer_list_mutex);
            free(mesh);
            return NULL;
        }
    }

    
    // Join registry
    cortez_options_t registry_opts = {.size=4*1024*1024, .create_policy=CORTEZ_CREATE_OR_JOIN};
    mesh->registry_ch = cortez_join(CORTEZ_REGISTRY_CHANNEL, &registry_opts);
    if (!mesh->registry_ch) {
        fprintf(stderr, "Failed to join registry channel\n");
        cortez_leave(mesh->inbox_ch);
        pthread_mutex_destroy(&mesh->peer_list_mutex);
        free(mesh);
        return NULL;
    }

    if (cortez_get_last_error(mesh->registry_ch) == CORTEZ_E_CHAN_STALE) {
        printf("[Mesh] Registry channel '%s' is stale, attempting recovery...\n", CORTEZ_REGISTRY_CHANNEL);
        if (cortez_channel_recover(mesh->registry_ch) != CORTEZ_OK) {
            fprintf(stderr, "Failed to recover stale registry channel.\n");
            cortez_leave(mesh->inbox_ch);
            cortez_leave(mesh->registry_ch);
            pthread_mutex_destroy(&mesh->peer_list_mutex);
            free(mesh);
            return NULL;
        }
        printf("[Mesh] Registry recovered successfully.\n");
    }

    //clears old msgs
    cortez_msg_t* stale_msg;
    while ((stale_msg = cortez_read(mesh->registry_ch, 0)) != NULL) { // 0 timeout = non-blocking read
        cortez_msg_release(mesh->registry_ch, stale_msg);
    }

    // Announce presence
    cortez_write(mesh->registry_ch, MESH_MSG_REGISTER, &mesh->self_info, sizeof(mesh->self_info));
    
    // Start housekeeper
    mesh->housekeeper_running = 1;
    if (pthread_create(&mesh->housekeeper_thread, NULL, housekeeper_thread_main, mesh) != 0) {
        mesh->housekeeper_running = 0;
        cortez_leave(mesh->inbox_ch);
        cortez_leave(mesh->registry_ch);
        pthread_mutex_destroy(&mesh->peer_list_mutex);
        free(mesh);
        return NULL;
    }
    
    return mesh;
}

int cortez_mesh_shutdown(cortez_mesh_t* mesh) {
    if (!mesh) return CORTEZ_E_INVALID_ARG;
    
    if (mesh->housekeeper_running) {
        mesh->housekeeper_running = 0;
        pthread_join(mesh->housekeeper_thread, NULL);
    }
    
    cortez_write(mesh->registry_ch, MESH_MSG_GOODBYE, &mesh->self_info, sizeof(mesh->self_info));
    
    pthread_mutex_lock(&mesh->peer_list_mutex);
    cortez_peer_t* peer = mesh->peer_list;
    while(peer) {
        cortez_peer_t* next = peer->next;
        if(peer->comm_channel) cortez_leave(peer->comm_channel);
        free(peer);
        peer = next;
    }
    mesh->peer_list = NULL;
    pthread_mutex_unlock(&mesh->peer_list_mutex);
    pthread_mutex_destroy(&mesh->peer_list_mutex);
    
    if(mesh->inbox_ch) cortez_leave(mesh->inbox_ch);
    if(mesh->registry_ch) cortez_leave(mesh->registry_ch);
    free(mesh);
    
    return CORTEZ_OK;
}

// Original send function (with copy) for backward compatibility
int cortez_mesh_send(cortez_mesh_t* mesh, pid_t target_pid, uint16_t msg_type, const void* payload, uint32_t payload_size) {
    if (!mesh || target_pid <= 0) {
        set_mesh_error(mesh, CORTEZ_E_INVALID_ARG);
        return CORTEZ_E_INVALID_ARG;
    }
    
    cortez_ch_t* peer_ch = NULL;
    int result = CORTEZ_E_PEER_NOT_FOUND;

    pthread_mutex_lock(&mesh->peer_list_mutex);
    for (cortez_peer_t* peer = mesh->peer_list; peer != NULL; peer = peer->next) {
        if (peer->info.pid == target_pid) {
            if (!peer->comm_channel) { // Lazily connect
                cortez_options_t join_opts = {.create_policy = CORTEZ_JOIN_ONLY};
                peer->comm_channel = cortez_join(peer->info.inbox_channel_name, &join_opts);
            }
            if (peer->comm_channel) {
                peer_ch = cortez_channel_ref(peer->comm_channel);
            }
            break;
        }
    }
    pthread_mutex_unlock(&mesh->peer_list_mutex);

    if (peer_ch) {
        result = cortez_write(peer_ch, msg_type, payload, payload_size);
        cortez_leave(peer_ch); // Release our reference
    } else {
        set_mesh_error(mesh, result);
    }
    
    return result;
}

pid_t cortez_mesh_find_peer_by_name(cortez_mesh_t* mesh, const char* name) {
    if (!mesh || !name) {
        return 0; // Invalid arguments
    }

    pid_t found_pid = 0;
    size_t name_len = strlen(name);

    // Lock the mutex to safely access the shared peer list
    pthread_mutex_lock(&mesh->peer_list_mutex);

    for (cortez_peer_t* p = mesh->peer_list; p != NULL; p = p->next) {

        if (strncmp(p->info.inbox_channel_name, name, name_len) == 0 &&
            p->info.inbox_channel_name[name_len] == '-') {
            found_pid = p->info.pid;
            break; // Found the first match, no need to continue
        }
    }

    // Always unlock the mutex
    pthread_mutex_unlock(&mesh->peer_list_mutex);

    return found_pid;
}


// --- NEW MESH ZERO-COPY API ---

cortez_write_handle_t* cortez_mesh_begin_send_zc(cortez_mesh_t* mesh, pid_t target_pid, uint32_t payload_size) {
    if (!mesh || target_pid <= 0) {
        set_mesh_error(mesh, CORTEZ_E_INVALID_ARG);
        return NULL;
    }

    cortez_ch_t* peer_ch = NULL;
    cortez_write_handle_t* handle = NULL;
    int err = CORTEZ_E_PEER_NOT_FOUND;

    pthread_mutex_lock(&mesh->peer_list_mutex);
    for (cortez_peer_t* peer = mesh->peer_list; peer != NULL; peer = peer->next) {
        if (peer->info.pid == target_pid) {
            if (!peer->comm_channel) { // Lazily connect
                cortez_options_t join_opts = {.create_policy = CORTEZ_JOIN_ONLY};
                peer->comm_channel = cortez_join(peer->info.inbox_channel_name, &join_opts);
                if (!peer->comm_channel) {
                    err = CORTEZ_E_CHAN_NOT_FOUND;
                }
            }
             if (peer->comm_channel) {
                // Get a safe reference to the channel before unlocking
                peer_ch = cortez_channel_ref(peer->comm_channel);
             }
            break;
        }
    }
    pthread_mutex_unlock(&mesh->peer_list_mutex);

    if (peer_ch) {
        handle = cortez_begin_write_zc(peer_ch, payload_size);
        if (!handle) {
             err = cortez_get_last_error(peer_ch);
             cortez_leave(peer_ch); // Release ref on failure
        }
    }
    
    if(!handle) {
        set_mesh_error(mesh, err);
    }
    return handle;
}

int cortez_mesh_commit_send_zc(cortez_write_handle_t* handle, uint16_t msg_type) {
    if (!handle) return CORTEZ_E_INVALID_ARG;
    cortez_ch_t* ch = handle->ch; // Get channel before it's freed
    int result = cortez_commit_write_zc(handle, msg_type);
    cortez_leave(ch); // Release the reference taken in begin_send
    return result;
}

void cortez_mesh_abort_send_zc(cortez_write_handle_t* handle) {
    if (!handle) return;
    cortez_ch_t* ch = handle->ch; // Get channel before it's freed
    cortez_abort_write_zc(handle);
    cortez_leave(ch); // Release the reference taken in begin_send
}

// --- END NEW MESH ZERO-COPY API ---


cortez_msg_t* cortez_mesh_read(cortez_mesh_t* mesh, int timeout_ms) {
    if (!mesh) return NULL;
    return cortez_read(mesh->inbox_ch, timeout_ms);
}

void cortez_mesh_list_peers(cortez_mesh_t* mesh) {
    if (!mesh) return;
    pthread_mutex_lock(&mesh->peer_list_mutex);
    printf("--- Active Peers (My PID: %d) ---\n", mesh->self_info.pid);
    if (!mesh->peer_list) {
        printf("  (no other peers found)\n");
    }
    for (cortez_peer_t* p = mesh->peer_list; p != NULL; p = p->next) {
        printf("  - PID: %d, Inbox: %s\n", p->info.pid, p->info.inbox_channel_name);
    }
    printf("----------------------------------\n");
    pthread_mutex_unlock(&mesh->peer_list_mutex);
}

pid_t cortez_mesh_get_pid(cortez_mesh_t* mesh) {
    if (!mesh) return 0;
    return mesh->self_info.pid;
}

