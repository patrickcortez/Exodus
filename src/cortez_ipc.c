/* tools-src/cortez_ipc.c
 * gcc -Wall -c cortez_ipc.c -o cortez_ipc.o -Iinclude
 */

#include "cortez_ipc.h"
#include "cortez_tunnel_shared.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/wait.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sched.h> 
#include <stddef.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

// Path to tunnel device driver
#define TUNNEL_DEVICE "/dev/cortez_tunnel"
// Arbitrary limit for a single IPC payload
#define MAX_IPC_SIZE (4 * 1024 * 1024)

// Defines the layout of our shared memory region
typedef struct {
    volatile uint32_t data_len;
    volatile uint8_t data_ready; // 0 = Not ready, 1 = Ready for reading
    char data[];                 // Flexible array member for payload
} CortezTunnelLayout;


int cortez_ipc_send(const char *target_exe, ...) {
    char executable_path[PATH_MAX];

    // --- SIBLING FINDER LOGIC ---
    if (strchr(target_exe, '/') == NULL || strncmp(target_exe, "./", 2) == 0) {
        const char *exe_name = (strncmp(target_exe, "./", 2) == 0) ? target_exe + 2 : target_exe;
        char self_path[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
        if (len != -1) {
            self_path[len] = '\0';
            char *last_slash = strrchr(self_path, '/');
            if (last_slash) {
                *last_slash = '\0';
                snprintf(executable_path, sizeof(executable_path), "%s/%s", self_path, exe_name);
            } else { strncpy(executable_path, exe_name, sizeof(executable_path) - 1); }
        } else {
            perror("cortez_ipc (readlink)");
            strncpy(executable_path, exe_name, sizeof(executable_path) - 1);
        }
    } else { strncpy(executable_path, target_exe, sizeof(executable_path) - 1); }
    executable_path[sizeof(executable_path) - 1] = '\0';
    // --- END SIBLING FINDER LOGIC ---


    // 1. First pass (Dry Run): Calculate total size without copying data.
    size_t total_data_size = 0;
    va_list args;
    va_start(args, target_exe);
    while (1) {
        CortezDataType type = va_arg(args, int);
        if (type == 0) break;

        total_data_size += 1 + sizeof(uint32_t); // Type (1 byte) + Length (4 bytes)

        switch (type) {
            case CORTEZ_TYPE_INT:
                va_arg(args, int32_t); // Consume arg
                total_data_size += sizeof(int32_t);
                break;
            case CORTEZ_TYPE_STRING:
                total_data_size += strlen(va_arg(args, char*)) + 1;
                break;
            case CORTEZ_TYPE_BLOB:
                total_data_size += (uint32_t)va_arg(args, size_t);
                va_arg(args, void*); // Consume data pointer
                break;
            default: goto end_calc_loop;
        }
    }
end_calc_loop:
    va_end(args);

    if (total_data_size > MAX_IPC_SIZE) {
        fprintf(stderr, "cortez_ipc: Total data size exceeds MAX_IPC_SIZE\n");
        return -1;
    }

    // 2. Create a uniquely named tunnel
    char tunnel_name[64];
    snprintf(tunnel_name, sizeof(tunnel_name), "cortez_ipc_%d", getpid());

    int fd = open(TUNNEL_DEVICE, O_RDWR);
    if (fd < 0) {
        perror("cortez_ipc (open " TUNNEL_DEVICE ")");
        return -1;
    }

    tunnel_create_t create_info;
    strncpy(create_info.name, tunnel_name, sizeof(create_info.name) - 1);
    create_info.name[sizeof(create_info.name) - 1] = '\0';
    create_info.size = sizeof(CortezTunnelLayout) + total_data_size;

    if (ioctl(fd, TUNNEL_CREATE, &create_info) < 0) {
        perror("cortez_ipc (ioctl TUNNEL_CREATE)");
        close(fd);
        return -1;
    }
    
    // 3. Map the tunnel into our address space
    CortezTunnelLayout *shm = mmap(NULL, create_info.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) {
        perror("cortez_ipc (mmap)");
        close(fd);
        return -1;
    }

    // Initialize shared memory header for synchronization
    shm->data_ready = 0;
    shm->data_len = total_data_size;
    
    // 4. Second pass (Wet Run): Serialize data directly into shared memory.
    char *current_pos = shm->data;
    va_start(args, target_exe);
    while (1) {
        CortezDataType type = va_arg(args, int);
        if (type == 0) break;

        uint32_t len;
        void *data_ptr;
        
        *current_pos++ = (uint8_t)type;

        switch (type) {
            case CORTEZ_TYPE_INT:
                len = sizeof(int32_t);
                int32_t val = va_arg(args, int32_t);
                data_ptr = &val;
                break;
            case CORTEZ_TYPE_STRING:
                data_ptr = va_arg(args, char*);
                len = strlen((char*)data_ptr) + 1;
                break;
            case CORTEZ_TYPE_BLOB:
                len = (uint32_t)va_arg(args, size_t);
                data_ptr = va_arg(args, void*);
                break;
            default: goto end_send_loop;
        }

        memcpy(current_pos, &len, sizeof(len));
        current_pos += sizeof(len);
        memcpy(current_pos, data_ptr, len);
        current_pos += len;
    }
end_send_loop:
    va_end(args);

    // 5. Fork and execute the receiver, passing the tunnel name
    pid_t pid = fork();
    if (pid == -1) {
        perror("cortez_ipc (fork)");
        munmap(shm, create_info.size);
        close(fd);
        return -1;
    }

    if (pid == 0) { // --- Child Process ---
        char size_str[32];
        snprintf(size_str, sizeof(size_str), "%lu", create_info.size);
        execlp(executable_path, executable_path, tunnel_name, size_str, (char *)NULL);
        perror("cortez_ipc (execlp)");
        _exit(127);
    } else { // --- Parent Process ---
        // 6. Signal to the receiver that the data is ready
        shm->data_ready = 1;
        
        // 7. Wait for child to exit and clean up
        waitpid(pid, NULL, 0);
    }

    munmap(shm, create_info.size);
    close(fd);
    return 0;
}

typedef struct {
    void* shm_addr;
    size_t shm_size;
    int fd;
    CortezIPCData node;
} CortezIPCInternalHead;


CortezIPCData* cortez_ipc_receive(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "cortez_ipc: Tunnel name and size not provided to receiver.\n");
        return NULL;
    }
    const char *tunnel_name = argv[1];
    unsigned long tunnel_size = strtoul(argv[2], NULL, 10);
    if (tunnel_size == 0) {
        fprintf(stderr, "cortez_ipc: Invalid tunnel size provided.\n");
        return NULL;
    }

    int fd = open(TUNNEL_DEVICE, O_RDWR);
    if (fd < 0) {
        perror("cortez_ipc (open " TUNNEL_DEVICE ")");
        return NULL;
    }

    // 1. Connect to the tunnel
    if (ioctl(fd, TUNNEL_CONNECT, (char*)tunnel_name) < 0) {
        perror("cortez_ipc (ioctl TUNNEL_CONNECT)");
        close(fd);
        return NULL;
    }

    // 2. Map the tunnel
    CortezTunnelLayout *shm = mmap(NULL, tunnel_size, PROT_READ, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) {
        perror("cortez_ipc (mmap)");
        close(fd);
        return NULL;
    }

    // 3. Wait for the sender to signal that data is ready
    while (shm->data_ready == 0) {
        sched_yield();
    }

    // 4. De-serialize data by pointing directly into the shared memory buffer
    CortezIPCData *head = NULL, *current = NULL;
    const char *data_ptr = shm->data;
    uint32_t bytes_to_process = shm->data_len;

    while (bytes_to_process > 0) {
        if (bytes_to_process < 1 + sizeof(uint32_t)) break;
        
        uint8_t type_byte = *(uint8_t*)data_ptr;
        data_ptr += 1;
        uint32_t len = *(uint32_t*)data_ptr;
        data_ptr += sizeof(uint32_t);

        if (bytes_to_process < 1 + sizeof(uint32_t) + len) break;

        // Allocate the appropriate struct. The first node is the special internal
        // one, subsequent nodes are standard.
        CortezIPCData *new_node;
        if (!head) {
            CortezIPCInternalHead *internal_head = malloc(sizeof(CortezIPCInternalHead));
            if (!internal_head) break;
            internal_head->shm_addr = shm;
            internal_head->shm_size = tunnel_size;
            internal_head->fd = fd;
            new_node = &internal_head->node; // Use the embedded node
        } else {
            new_node = malloc(sizeof(CortezIPCData));
            if (!new_node) break;
        }

        new_node->type = (CortezDataType)type_byte;
        new_node->length = len;
        new_node->next = NULL;

        // ZERO-COPY: Assign pointers directly into the shared memory region
        switch (new_node->type) {
            case CORTEZ_TYPE_INT:    new_node->data.int_val = (const int32_t*)data_ptr; break;
            case CORTEZ_TYPE_STRING: new_node->data.string_val = (const char*)data_ptr; break;
            case CORTEZ_TYPE_BLOB:   new_node->data.blob_val = (const void*)data_ptr; break;
            default:
                // If it's the head node, we need to free the internal wrapper
                if (new_node == &((CortezIPCInternalHead*)new_node)->node) free((CortezIPCInternalHead*)new_node);
                else free(new_node);
                continue; // Skip unknown type
        }
        
        data_ptr += len;

        if (!head) head = current = new_node;
        else { current->next = new_node; current = new_node; }
        
        bytes_to_process -= (1 + sizeof(uint32_t) + len);
    }

    // 5. DO NOT unmap memory or close fd here. This is now handled by free().
    return head;
}


void cortez_ipc_free_data(CortezIPCData *head) {
    if (!head) return;

    // 1. Recalculate the address of the internal header from the head node pointer
    CortezIPCInternalHead *internal_head = (CortezIPCInternalHead*)((char*)head - offsetof(CortezIPCInternalHead, node));

    // 2. Unmap memory and close the file descriptor using the stored info
    munmap(internal_head->shm_addr, internal_head->shm_size);
    close(internal_head->fd);

    // 3. Free the linked list nodes.
    // The data pointers (e.g., string_val) are NOT freed because they point
    // into the shared memory we just unmapped.
    CortezIPCData *current = head->next; // Start with the node AFTER the head
    while (current != NULL) {
        CortezIPCData *next = current->next;
        free(current);
        current = next;
    }

    // 4. Finally, free the allocation for the internal head struct itself
    free(internal_head);
}


