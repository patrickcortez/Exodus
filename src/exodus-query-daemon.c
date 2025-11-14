/*
 * Compile Command:
 * gcc -Wall -Wextra -O2 exodus-query-daemon.c cortez-mesh.o -o query_daemon -pthread
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>

#include "cortez-mesh.h"
#include "exodus-common.h"

#define PID_FILE "/tmp/exodus.pid"

// --- Data Structures for Request Tracking ---

typedef struct PendingRequest {
    uint64_t request_id;
    pid_t client_pid;
    time_t timestamp;
    struct PendingRequest* next;
} PendingRequest;

static PendingRequest* pending_requests_head = NULL;
static uint64_t next_request_id = 1;
static pthread_mutex_t request_list_mutex = PTHREAD_MUTEX_INITIALIZER;

static volatile int keep_running = 1;
static pid_t cloud_daemon_pid = 0;

void int_handler(int dummy) {
    (void)dummy;
    keep_running = 0;
}

int discover_cloud_daemon() {
    FILE* f = fopen(PID_FILE, "r");
    if (!f) {
        return 0;
    }
    pid_t pid1, pid2;
    if (fscanf(f, "%d\n%d\n", &pid1, &pid2) == 2) {
        // The first PID in the file is always the cloud daemon
        cloud_daemon_pid = pid1;
        printf("[Query] Discovered Cloud Daemon with PID: %d\n", cloud_daemon_pid);
    }
    fclose(f);
    return cloud_daemon_pid != 0;
}

void cleanup_request_list() {
    pthread_mutex_lock(&request_list_mutex);
    PendingRequest* current = pending_requests_head;
    while(current) {
        PendingRequest* next = current->next;
        free(current);
        current = next;
    }
    pending_requests_head = NULL;
    pthread_mutex_unlock(&request_list_mutex);
}

void write_to_handle(cortez_write_handle_t* h, const void* data, size_t size) {
    size_t part1_size;
    char* part1 = cortez_write_handle_get_part1(h, &part1_size);
    
    if (size <= part1_size) {
        memcpy(part1, data, size);
    } else {
        size_t part2_size;
        char* part2 = cortez_write_handle_get_part2(h, &part2_size);
        memcpy(part1, data, part1_size);
        memcpy(part2, (const char*)data + part1_size, size - part1_size);
    }
}

int main() {
    signal(SIGINT, int_handler);
    signal(SIGTERM, int_handler);

    printf("[Query] Initializing Ingestion & Query Daemon...\n");
    cortez_mesh_t* mesh = cortez_mesh_init(QUERY_DAEMON_NAME, NULL);
    if (!mesh) {
        fprintf(stderr, "[Query] Failed to initialize mesh.\n");
        return 1;
    }

    printf("[Query] Daemon running with PID: %d\n", cortez_mesh_get_pid(mesh));
    
    // *** FIX: Wait for cloud daemon discovery BEFORE entering main loop ***
    printf("[Query] Waiting for cloud daemon to be discoverable...\n");
    while (!discover_cloud_daemon() && keep_running) {
        sleep(1);
    }
    
    if (!keep_running) {
        printf("[Query] Interrupted during startup.\n");
        cortez_mesh_shutdown(mesh);
        return 1;
    }
    
    printf("[Query] Cloud daemon discovered. Ready to process requests.\n");

    while (keep_running) {
        cortez_msg_t* msg = cortez_mesh_read(mesh, 1000);
        if (!msg) continue;

        pid_t sender_pid = cortez_msg_sender_pid(msg);
        uint16_t msg_type = cortez_msg_type(msg);

        // --- Handle requests from clients (e.g., 'exodus' CLI) ---
        if ((msg_type >= MSG_UPLOAD_FILE && msg_type <= MSG_COMMIT_NODE) || 
            (msg_type >= MSG_NODE_MAN_CREATE && msg_type <= MSG_NODE_MAN_COPY)) {
            printf("[Query] Received request (type %d) from client %d. Forwarding to cloud daemon.\n", msg_type, sender_pid);

            PendingRequest* new_req = malloc(sizeof(PendingRequest));
            if (!new_req) {
                fprintf(stderr, "[Query] Out of memory, dropping request.\n");
                cortez_mesh_msg_release(mesh, msg);
                continue;
            }
            new_req->client_pid = sender_pid;
            new_req->timestamp = time(NULL);

            pthread_mutex_lock(&request_list_mutex);
            new_req->request_id = next_request_id++;
            new_req->next = pending_requests_head;
            pending_requests_head = new_req;
            pthread_mutex_unlock(&request_list_mutex);

            const void* original_payload = cortez_msg_payload(msg);
            uint32_t original_payload_size = cortez_msg_payload_size(msg);
            uint32_t forwarded_payload_size = sizeof(uint64_t) + original_payload_size;

            int forwarded_ok = 0;
            for (int i = 0; i < 50; i++) {
                cortez_write_handle_t* h = cortez_mesh_begin_send_zc(mesh, cloud_daemon_pid, forwarded_payload_size);
                if (h) {
                    char* temp_buffer = malloc(forwarded_payload_size);
                    if (!temp_buffer) {
                        fprintf(stderr, "[Query] Out of memory creating forward buffer.\n");
                        cortez_mesh_abort_send_zc(h);
                        continue;
                    }

                    memcpy(temp_buffer, &new_req->request_id, sizeof(uint64_t));
                    memcpy(temp_buffer + sizeof(uint64_t), original_payload, original_payload_size);

                    write_to_handle(h, temp_buffer, forwarded_payload_size);
                    free(temp_buffer);

                    cortez_mesh_commit_send_zc(h, msg_type);
                    forwarded_ok = 1;
                    break;
                }
                usleep(200000);
            }

            if (!forwarded_ok) {
                fprintf(stderr, "[Query] Failed to forward message to cloud daemon\n");
                ack_t nack = {0, "Cloud daemon is not reachable."};
                
                int nack_sent = 0;
                for (int i = 0; i < 50; i++) {
                    cortez_write_handle_t* h = cortez_mesh_begin_send_zc(mesh, sender_pid, sizeof(nack));
                    if (h) {
                        size_t part1_size;
                        void* buffer = cortez_write_handle_get_part1(h, &part1_size);
                        memcpy(buffer, &nack, sizeof(nack));
                        cortez_mesh_commit_send_zc(h, MSG_OPERATION_ACK);
                        nack_sent = 1;
                        break;
                    }
                    usleep(200000);
                }
                if (!nack_sent) {
                    fprintf(stderr, "[Query] Failed to send NACK to client %d. It may have disconnected.\n", sender_pid);
                }
            }
        
        // --- Handle responses from the cloud daemon ---
        }else if ((msg_type >= MSG_QUERY_RESPONSE && msg_type <= MSG_INFO_NODE_RESPONSE) || msg_type == MSG_LOOKUP_RESPONSE){
            if (sender_pid != cloud_daemon_pid) {
                 printf("[Query] WARNING: Received a response from an unknown source (%d), ignoring.\n", sender_pid);
                 cortez_mesh_msg_release(mesh, msg);
                 continue;
            }
            
            const void* wrapped_payload = cortez_msg_payload(msg);
            uint32_t wrapped_payload_size = cortez_msg_payload_size(msg);
            if(wrapped_payload_size < sizeof(uint64_t)) {
                fprintf(stderr, "[Query] Received malformed response from cloud daemon, ignoring.\n");
                cortez_mesh_msg_release(mesh, msg);
                continue;
            }

            uint64_t response_req_id;
            memcpy(&response_req_id, wrapped_payload, sizeof(uint64_t));

            pid_t original_client_pid = 0;
            pthread_mutex_lock(&request_list_mutex);
            PendingRequest** pptr = &pending_requests_head;
            while(*pptr) {
                PendingRequest* entry = *pptr;
                if (entry->request_id == response_req_id) {
                    original_client_pid = entry->client_pid;
                    *pptr = entry->next;
                    free(entry);
                    break;
                }
                pptr = &(*pptr)->next;
            }
            pthread_mutex_unlock(&request_list_mutex);

            if (original_client_pid > 0) {
                const void* original_response_payload = (const char*)wrapped_payload + sizeof(uint64_t);
                uint32_t original_response_size = wrapped_payload_size - sizeof(uint64_t);
                
                int response_sent = 0;
                for (int i = 0; i < 5; i++) {
                    cortez_write_handle_t* h = cortez_mesh_begin_send_zc(mesh, original_client_pid, original_response_size);
                    if (h) {
                        printf("[Query] Forwarding response for request #%lu to client %d.\n", response_req_id, original_client_pid);

                        write_to_handle(h, original_response_payload, original_response_size);

                        cortez_mesh_commit_send_zc(h, msg_type);
                        response_sent = 1;
                        break;
                    }
                    usleep(100000);
                }

                if (!response_sent) {
                    fprintf(stderr, "[Query] Failed to send response to client %d after retries. Client may have disconnected.\n", original_client_pid);
                }
            } else {
                fprintf(stderr, "[Query] Received response for an unknown or timed-out request #%lu. Discarding.\n", response_req_id);
            }
        
        } else if (msg_type == MSG_TERMINATE) {
            printf("[Query] Termination signal received via mesh.\n");
            keep_running = 0;
        }

        cortez_mesh_msg_release(mesh, msg);
    }

    printf("[Query] Shutting down.\n");
    cleanup_request_list();
    cortez_mesh_shutdown(mesh);
    pthread_mutex_destroy(&request_list_mutex);
    return 0;
}