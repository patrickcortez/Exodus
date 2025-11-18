/*
 * exodus-signal.c
 * This process bridges the local cloud_daemon (via Cortez-Mesh) to the
 * LAN coordinator (via HTTP).
 *
 * COMPILE:
 * gcc -Wall -Wextra -O2 exodus-signal.c cortez-mesh.o ctz-json.a ctz-set.o -o exodus-signal -pthread
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <time.h> 
#include <stdarg.h>
#include <syslog.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>

// Includes cortez-mesh.h
#include "exodus-common.h" 
#include "ctz-json.h"
#include "ctz-set.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define CONF_FILE_NAME "exodus-coord.conf"
#define SIGNAL_HTTP_PORT 8081 // Port this signal server listens on
#define PID_FILE "/tmp/exodus.pid" 
#define MAX_HTTP_BODY_SIZE (50 * 1024 * 1024)

// --- Globals ---
static volatile int g_keep_running = 1;
static cortez_mesh_t* g_mesh = NULL;
static pid_t g_cloud_daemon_pid = 0;
static int g_server_fd = -1; // <-- NEW: Global server socket FD for shutdown

// Coordinator Info
static char g_coord_host[256] = {0};
static int g_coord_port = 8080;
static char g_unit_name[MAX_UNIT_NAME_LEN] = {0};

// State Caches
static char* g_local_node_list_json = NULL; // Cache of this unit's nodes
static pthread_mutex_t g_node_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static char g_storage_path[PATH_MAX] = {0};
static volatile int g_force_reconnect = 0;
static pthread_mutex_t g_config_mutex = PTHREAD_MUTEX_INITIALIZER;

// Request Queue (from mesh to coordinator)
typedef struct RequestNode {
    uint16_t type;
    void* payload; // Payload for the coordinator_client_thread
    size_t payload_size;
    struct RequestNode* next;
} RequestNode;

static RequestNode* g_request_queue_head = NULL;
static pthread_mutex_t g_request_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_request_queue_cond = PTHREAD_COND_INITIALIZER;


// --- Utility Functions ---

void log_msg(const char* format, ...) {
    va_list args;
    va_start(args, format);
    printf("[Signal] ");
    vprintf(format, args);
    printf("\n");
    fflush(stdout);
    va_end(args);
}

void int_handler(int dummy) {
    (void)dummy;
    g_keep_running = 0;
    pthread_cond_signal(&g_request_queue_cond); // Wake up queue thread
}

int discover_cloud_daemon_pid() {
    FILE* f = fopen(PID_FILE, "r");
    if (!f) {
        return 0; // PID file not found
    }
    
    pid_t pid1, pid2;
    if (fscanf(f, "%d\n%d\n", &pid1, &pid2) == 2) {
        g_cloud_daemon_pid = pid1;
        log_msg("Discovered Cloud Daemon with PID: %d", g_cloud_daemon_pid);
    } else {
        log_msg("Error: Could not parse PID file.");
        g_cloud_daemon_pid = 0;
    }
    
    fclose(f);
    return g_cloud_daemon_pid != 0;
}

int get_executable_dir(char* buffer, size_t size) {
    char path_buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", path_buf, sizeof(path_buf) - 1);
    if (len == -1) return -1;
    path_buf[len] = '\0';
    char* last_slash = strrchr(path_buf, '/');
    if (last_slash == NULL) return -1;
    *last_slash = '\0';
    strncpy(buffer, path_buf, size - 1);
    buffer[size - 1] = '\0';
    return 0;
}

void load_coordinator_config() {
    pthread_mutex_lock(&g_config_mutex);

    char exe_dir[PATH_MAX];
    char conf_path[PATH_MAX];
    
    // Defaults
    strncpy(g_coord_host, "127.0.0.1", sizeof(g_coord_host) - 1);
    g_coord_port = 8080;

    if (get_executable_dir(exe_dir, sizeof(exe_dir)) != 0) {
        log_msg("Error: Could not find executable dir. Using defaults.");
        return;
    }
    
    // Check for new .set file first
    snprintf(conf_path, sizeof(conf_path), "%s/exodus-coord.set", exe_dir);
    FILE* f = fopen(conf_path, "r");
    
    if (f) {
        fclose(f); // Exists, close and load with parser
        SetConfig* cfg = set_load(conf_path);
        if (cfg) {
            // Iterate sections to find "current = true"
            SetSection* sec = cfg->sections;
            int found_active = 0;
            while (sec) {
                if (strcmp(sec->name, "global") != 0) {
                    if (set_get_bool(cfg, sec->name, "current", 0)) {
                        const char* ip = set_get_string(cfg, sec->name, "ip", "127.0.0.1");
                        int port = (int)set_get_int(cfg, sec->name, "port", 8080);
                        
                        strncpy(g_coord_host, ip, sizeof(g_coord_host) - 1);
                        g_coord_port = port;
                        log_msg("Active Coordinator Profile: '%s' (%s:%d)", sec->name, g_coord_host, g_coord_port);
                        found_active = 1;
                        break;
                    }
                }
                sec = sec->next;
            }
            if (!found_active) {
                log_msg("Warning: No coordinator profile marked as 'current'. Using defaults.");
            }
            set_free(cfg);
            pthread_mutex_unlock(&g_config_mutex);
            return;
        }
    }
    
    // Fallback to old .conf if .set doesn't exist (Migration path)
    snprintf(conf_path, sizeof(conf_path), "%s/exodus-coord.conf", exe_dir);
    f = fopen(conf_path, "r");
    if (f) {
        log_msg("Legacy config found (%s). Please upgrade using 'unit-set --coord'.", conf_path);
        char line[512];
        if (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\n")] = 0;
            char* host_start = strstr(line, "://");
            if (host_start) {
                host_start += 3;
                char* port_start = strrchr(host_start, ':');
                if (port_start) {
                    *port_start = '\0';
                    g_coord_port = atoi(port_start + 1);
                    strncpy(g_coord_host, host_start, sizeof(g_coord_host) - 1);
                }
            }
        }
        fclose(f);
    } else {
        log_msg("No coordinator config found. Using default 127.0.0.1:8080");
    }
    pthread_mutex_unlock(&g_config_mutex);
}

void load_unit_config() {
    char exe_dir[PATH_MAX];
    char conf_path[PATH_MAX];
    const char* default_name = "My-Exodus-Unit";

    if (get_executable_dir(exe_dir, sizeof(exe_dir)) != 0) {
        log_msg("Error: Could not determine executable directory.");
        return;
    }
    snprintf(conf_path, sizeof(conf_path), "%s/exodus-unit.set", exe_dir);

    // 1. Load using the robust ctz-set parser
    SetConfig* cfg = set_load(conf_path);
    
    // 2. If file doesn't exist, create it with defaults
    if (!cfg) {
        log_msg("Config not found. Creating default at %s", conf_path);
        cfg = set_create(conf_path);
        set_set_string(cfg, "unit", "name", default_name);
        set_save(cfg);
    }

    // 3. Read values (Safely handles missing keys/sections)
    const char* name = set_get_string(cfg, "unit", "name", default_name);
    const char* storage = set_get_string(cfg, "storage", "path", "");

    strncpy(g_unit_name, name, sizeof(g_unit_name) - 1);
    strncpy(g_storage_path, storage, sizeof(g_storage_path) - 1);

    // 4. Cleanup
    set_free(cfg);

    log_msg("Config loaded: Name='%s', Storage='%s'", g_unit_name, g_storage_path[0] ? g_storage_path : "(none)");
}


void send_to_cloud(uint16_t msg_type, const void* payload, uint32_t size) {
    if (!g_mesh || g_cloud_daemon_pid == 0) return;

    for (int i = 0; i < 5; i++) { // Retry 5 times
        cortez_write_handle_t* h = cortez_mesh_begin_send_zc(g_mesh, g_cloud_daemon_pid, size);
        if (h) {
            size_t part1_size;
            void* buffer = cortez_write_handle_get_part1(h, &part1_size);
            if (size <= part1_size) {
                memcpy(buffer, payload, size);
            } else {
                memcpy(buffer, payload, part1_size);
                size_t part2_size;
                void* buffer2 = cortez_write_handle_get_part2(h, &part2_size);
                memcpy(buffer2, (char*)payload + part1_size, size - part1_size);
            }
            cortez_mesh_commit_send_zc(h, msg_type);
            return;
        }
        usleep(100000); // Wait 100ms
    }
    log_msg("Error: Failed to send message (type %d) to cloud daemon.", msg_type);
}

// Simple blocking HTTP request function
int send_http_request(const char* host, int port, const char* request, char* response_buf, size_t response_size) {
    struct hostent* server = gethostbyname(host);
    if (server == NULL) {
        log_msg("HTTP Error: Could not resolve host: %s", host);
        return -1;
    }

    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        log_msg("HTTP Error: Could not create socket");
        return -1;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);

    if (connect(sock_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        log_msg("HTTP Error: Could not connect to %s:%d", host, port);
        close(sock_fd);
        return -1;
    }

    if (write(sock_fd, request, strlen(request)) < 0) {
        log_msg("HTTP Error: Failed to write to socket");
        close(sock_fd);
        return -1;
    }

    ssize_t n = read(sock_fd, response_buf, response_size - 1);
    if (n >= 0) {
        response_buf[n] = '\0';
    } else {
        log_msg("HTTP Error: Failed to read response");
        close(sock_fd);
        return -1;
    }

    close(sock_fd);
    
    if (strstr(response_buf, "HTTP/1.1 200 OK") == NULL) {
        log_msg("HTTP Error: Server returned non-200 status.");
        log_msg("--- Server Response ---");
        printf("%s\n", response_buf);
        log_msg("-----------------------");
        return -1;
    }
    
    return 0; // Success
}

// --- Mesh Listener Thread ---
void* mesh_listener_thread(void* arg) {
    (void)arg;
    log_msg("Mesh listener thread started.");
    
    while (g_keep_running) {
        cortez_msg_t* msg = cortez_mesh_read(g_mesh, 1000);
        if (!msg) {
            if (!g_keep_running) break;
            continue;
        }

        if (cortez_msg_sender_pid(msg) != g_cloud_daemon_pid) {
            log_msg("Warning: Received message from non-cloud PID %d", cortez_msg_sender_pid(msg));
            cortez_mesh_msg_release(g_mesh, msg);
            continue;
        }
        
        uint16_t msg_type = cortez_msg_type(msg);
        const void* payload = cortez_msg_payload(msg);
        size_t payload_size = cortez_msg_payload_size(msg);
        
        log_msg("Received message %d from cloud daemon.", msg_type);

        if (msg_type == MSG_SIG_CACHE_NODE_LIST) {
            pthread_mutex_lock(&g_node_cache_mutex);
            free(g_local_node_list_json);
            g_local_node_list_json = malloc(payload_size + 1);
            if (g_local_node_list_json) {
                memcpy(g_local_node_list_json, payload, payload_size);
                g_local_node_list_json[payload_size] = '\0';
                log_msg("Updated local node list cache.");
            }
            pthread_mutex_unlock(&g_node_cache_mutex);
        }
        else if (msg_type == MSG_SIG_REQUEST_UNIT_LIST || 
                 msg_type == MSG_SIG_REQUEST_VIEW_UNIT ||
                 msg_type == MSG_SIG_REQUEST_SYNC_NODE) {
            
            RequestNode* new_req = malloc(sizeof(RequestNode));
            if (new_req) {
                new_req->type = msg_type;
                new_req->payload = malloc(payload_size);
                if (new_req->payload) {
                    memcpy(new_req->payload, payload, payload_size);
                    new_req->payload_size = payload_size;
                    new_req->next = NULL;
                    
                    pthread_mutex_lock(&g_request_queue_mutex);
                    if (g_request_queue_head == NULL) {
                        g_request_queue_head = new_req;
                    } else {
                        RequestNode* temp = g_request_queue_head;
                        while(temp->next) temp = temp->next;
                        temp->next = new_req;
                    }
                    pthread_mutex_unlock(&g_request_queue_mutex);
                    pthread_cond_signal(&g_request_queue_cond);
                    
                } else {
                    free(new_req);
                }
            }
        }else if (msg_type == MSG_SIG_REQUEST_VIEW_CACHE) {
            log_msg("Received VIEW_CACHE request from cloud daemon.");
            
            // The payload from the cloud daemon contains the original request_id
            if (payload_size < sizeof(uint64_t)) {
                log_msg("Error: VIEW_CACHE request is malformed (too small).");
                cortez_mesh_msg_release(g_mesh, msg);
                continue;
            }
            uint64_t request_id;
            memcpy(&request_id, payload, sizeof(uint64_t));

            pthread_mutex_lock(&g_node_cache_mutex);
            
            const char* cache_content = (g_local_node_list_json != NULL) ? g_local_node_list_json : "[]";
            size_t content_len = strlen(cache_content);
            size_t total_resp_size = sizeof(uint64_t) + content_len + 1; // +1 for null terminator

            char* resp_buf = malloc(total_resp_size);
            if (resp_buf) {
                // Wrap the response with the original request_id
                memcpy(resp_buf, &request_id, sizeof(uint64_t));
                memcpy(resp_buf + sizeof(uint64_t), cache_content, content_len + 1);
                
                send_to_cloud(MSG_SIG_RESPONSE_VIEW_CACHE, resp_buf, total_resp_size);
                
                free(resp_buf);
            }
            
            pthread_mutex_unlock(&g_node_cache_mutex);

        } else if (msg_type == MSG_SIG_REQUEST_RESOLVE_UNIT) {
            RequestNode* new_req = malloc(sizeof(RequestNode));
            if (new_req) {
                new_req->type = msg_type;
                new_req->payload = malloc(payload_size);
                memcpy(new_req->payload, payload, payload_size);
                new_req->payload_size = payload_size;
                new_req->next = NULL;
                
                pthread_mutex_lock(&g_request_queue_mutex);
                if (g_request_queue_head == NULL) {
                    g_request_queue_head = new_req;
                } else {
                    RequestNode* temp = g_request_queue_head;
                    while(temp->next) temp = temp->next;
                    temp->next = new_req;
                }
                pthread_mutex_unlock(&g_request_queue_mutex);
                pthread_cond_signal(&g_request_queue_cond);
            }
        } else if (msg_type == MSG_SIG_RELOAD_CONFIG) {
            log_msg("Received Hot Reload signal. Reloading configuration...");
            load_coordinator_config(); // Reloads from disk
            g_force_reconnect = 1;     // Triggers client thread to reset
        }
        else if (msg_type == MSG_TERMINATE) {
            log_msg("Received TERMINATE from cloud daemon.");
            g_keep_running = 0;
            pthread_cond_signal(&g_request_queue_cond);
        }

        cortez_mesh_msg_release(g_mesh, msg);
    }
    
    log_msg("Mesh listener thread stopping.");
    return NULL;
}

// --- Coordinator Client Thread ---
void* coordinator_client_thread(void* arg) {
    (void)arg;
    log_msg("Coordinator client thread started.");
    
    char http_req_buf[8192]; 
    char http_resp_buf[8192];
    time_t last_register_time = 0;
    int connected_to_coord = 0;

    while (g_keep_running) {
        time_t now = time(NULL);

        if (g_force_reconnect) {
            log_msg("Hot Reload: Resetting connection state.");
            connected_to_coord = 0;
            last_register_time = 0;
            g_force_reconnect = 0;
        }

        if (now > last_register_time + 30) {
            
            char current_host[256];
            int current_port;
            
            pthread_mutex_lock(&g_config_mutex);
            strncpy(current_host, g_coord_host, sizeof(current_host)-1);
            current_port = g_coord_port;
            pthread_mutex_unlock(&g_config_mutex);
            log_msg("Registering with coordinator at %s:%d", g_coord_host, g_coord_port);
            
            char json_payload[512];
            snprintf(json_payload, sizeof(json_payload), 
                "{\"unit_name\": \"%s\", \"listen_port\": %d}", 
                g_unit_name, SIGNAL_HTTP_PORT);

            snprintf(http_req_buf, sizeof(http_req_buf),
                "POST /register HTTP/1.1\r\n"
                "Host: %s:%d\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: %zu\r\n"
                "Connection: close\r\n\r\n%s",
                g_coord_host, g_coord_port, strlen(json_payload), json_payload
            );
            
            if (send_http_request(g_coord_host, g_coord_port, http_req_buf, http_resp_buf, sizeof(http_resp_buf)) == 0) {
                if (!connected_to_coord) {
                    log_msg("Successfully connected to coordinator.");
                    connected_to_coord = 1;
                    sig_status_update_t status = { .connected = 1 };
                    strncpy(status.coordinator_url, g_coord_host, sizeof(status.coordinator_url) - 1);
                    send_to_cloud(MSG_SIG_STATUS_UPDATE, &status, sizeof(status));
                }
            } else {
                if (connected_to_coord) {
                    log_msg("Error: Lost connection to coordinator.");
                    connected_to_coord = 0;
                    sig_status_update_t status = { .connected = 0 };
                    send_to_cloud(MSG_SIG_STATUS_UPDATE, &status, sizeof(status));
                }
            }
            last_register_time = now;
        }

        RequestNode* req_to_process = NULL;
        
        pthread_mutex_lock(&g_request_queue_mutex);
        if (g_request_queue_head) {
            req_to_process = g_request_queue_head;
            g_request_queue_head = req_to_process->next;
        }
        pthread_mutex_unlock(&g_request_queue_mutex);

        if (req_to_process) {

            if (req_to_process->payload_size < sizeof(uint64_t)) {
                log_msg("Error: Dropping malformed request, payload too small.");
                free(req_to_process->payload);
                free(req_to_process);
                continue;
            }
            
            uint64_t request_id;
            memcpy(&request_id, req_to_process->payload, sizeof(uint64_t));
            const void* inner_payload = (const char*)req_to_process->payload + sizeof(uint64_t);

            log_msg("Processing queued request (type %d)", req_to_process->type);
            
            if (!connected_to_coord) {
                log_msg("Error: Not connected to coordinator. Dropping request.");
                ack_t nack = { .success = 0 };
                snprintf(nack.details, sizeof(nack.details), "Not connected to coordinator.");
                size_t wrapped_nack_size = sizeof(uint64_t) + sizeof(ack_t);
                char* wrapped_nack = malloc(wrapped_nack_size);
                if (wrapped_nack) {
                    memcpy(wrapped_nack, &request_id, sizeof(uint64_t));
                    memcpy(wrapped_nack + sizeof(uint64_t), &nack, sizeof(ack_t));
                    send_to_cloud(MSG_OPERATION_ACK, wrapped_nack, wrapped_nack_size);
                    free(wrapped_nack);
                }
            } else {
                int http_ok = -1;
                
                if (req_to_process->type == MSG_SIG_REQUEST_UNIT_LIST) {
                    snprintf(http_req_buf, sizeof(http_req_buf),
                        "GET /units HTTP/1.1\r\n"
                        "Host: %s:%d\r\n"
                        "Connection: close\r\n\r\n",
                        g_coord_host, g_coord_port
                    );
                    http_ok = send_http_request(g_coord_host, g_coord_port, http_req_buf, http_resp_buf, sizeof(http_resp_buf));
                    
                } else if (req_to_process->type == MSG_SIG_REQUEST_VIEW_UNIT) {
                    sig_view_unit_req_t* req = (sig_view_unit_req_t*)inner_payload;
                    snprintf(http_req_buf, sizeof(http_req_buf),
                        "GET /nodes?target_unit=%s HTTP/1.1\r\n"
                        "Host: %s:%d\r\n"
                        "Connection: close\r\n\r\n",
                        req->unit_name, g_coord_host, g_coord_port
                    );
                    http_ok = send_http_request(g_coord_host, g_coord_port, http_req_buf, http_resp_buf, sizeof(http_resp_buf));
                
                } else if (req_to_process->type == MSG_SIG_REQUEST_SYNC_NODE) {
                    sig_sync_req_t* req = (sig_sync_req_t*)inner_payload;
                    
                    char json_payload[MAX_SYNC_DATA_SIZE + 512];
                    snprintf(json_payload, sizeof(json_payload),
                        "{\"target_unit\": \"%s\", \"target_node\": \"%s\", \"data\": %s}",
                        req->target_unit, req->remote_node, req->sync_payload_json);

                    snprintf(http_req_buf, sizeof(http_req_buf),
                        "POST /sync HTTP/1.1\r\n"
                        "Host: %s:%d\r\n"
                        "Content-Type: application/json\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: close\r\n\r\n%s",
                        g_coord_host, g_coord_port, strlen(json_payload), json_payload
                    );
                    http_ok = send_http_request(g_coord_host, g_coord_port, http_req_buf, http_resp_buf, sizeof(http_resp_buf));
                }else if (req_to_process->type == MSG_SIG_REQUEST_RESOLVE_UNIT) {
                    resolve_unit_req_t* req = (resolve_unit_req_t*)inner_payload;
                    
                    // URL encode spaces as + for simplicity
                    char encoded_name[256];
                    strncpy(encoded_name, req->target_unit_name, sizeof(encoded_name)-1);
                    for(int i=0; encoded_name[i]; i++) if(encoded_name[i] == ' ') encoded_name[i] = '+';

                    snprintf(http_req_buf, sizeof(http_req_buf),
                        "GET /resolve?unit=%s HTTP/1.1\r\n"
                        "Host: %s:%d\r\n"
                        "Connection: close\r\n\r\n",
                        encoded_name, g_coord_host, g_coord_port
                    );
                    
                    http_ok = send_http_request(g_coord_host, g_coord_port, http_req_buf, http_resp_buf, sizeof(http_resp_buf));
                    
                    resolve_unit_resp_t resp = {0};
                    if (http_ok == 0) {
                        char* body = strstr(http_resp_buf, "\r\n\r\n");
                        if (body) {
                            body += 4;
                            ctz_json_value* root = ctz_json_parse(body, NULL, 0);
                            if (root) {
                                const char* ip = ctz_json_get_string(ctz_json_find_object_value(root, "ip"));
                                int port = (int)ctz_json_get_number(ctz_json_find_object_value(root, "port"));
                                if (ip && port > 0) {
                                    resp.success = 1;
                                    strncpy(resp.ip_addr, ip, sizeof(resp.ip_addr) - 1);
                                    resp.port = port;
                                }
                                ctz_json_free(root);
                            }
                        }
                    }
                    
                    size_t wrapped_size = sizeof(uint64_t) + sizeof(resolve_unit_resp_t);
                    char* wrapped_resp = malloc(wrapped_size);
                    memcpy(wrapped_resp, &request_id, sizeof(uint64_t));
                    memcpy(wrapped_resp + sizeof(uint64_t), &resp, sizeof(resolve_unit_resp_t));
                    
                    send_to_cloud(MSG_SIG_RESPONSE_RESOLVE_UNIT, wrapped_resp, wrapped_size);
                    free(wrapped_resp);
                }

                if (http_ok == 0) {
                    char* body = strstr(http_resp_buf, "\r\n\r\n");
                    if (body) {
                        body += 4; 
                        size_t body_len = strlen(body);
                        
                        size_t wrapped_size = sizeof(uint64_t) + body_len + 1; // +1 for null
                        char* wrapped_resp = malloc(wrapped_size);
                        if (!wrapped_resp) { /* handle error */ }
                        
                        memcpy(wrapped_resp, &request_id, sizeof(uint64_t));
                        memcpy(wrapped_resp + sizeof(uint64_t), body, body_len + 1); // Copy null
                        
                        if (req_to_process->type == MSG_SIG_REQUEST_UNIT_LIST) {
                            send_to_cloud(MSG_SIG_RESPONSE_UNIT_LIST, wrapped_resp, wrapped_size);
                        } else if (req_to_process->type == MSG_SIG_REQUEST_VIEW_UNIT) {
                            send_to_cloud(MSG_SIG_RESPONSE_VIEW_UNIT, wrapped_resp, wrapped_size);
                        } else if (req_to_process->type == MSG_SIG_REQUEST_SYNC_NODE) {
                            ack_t ack = { .success = 1 };
                            snprintf(ack.details, sizeof(ack.details), "Sync request sent to coordinator.");
                            
                            // Re-wrap for the ACK
                            size_t wrapped_ack_size = sizeof(uint64_t) + sizeof(ack_t);
                            memcpy(wrapped_resp, &request_id, sizeof(uint64_t)); // Reuse buffer
                            memcpy(wrapped_resp + sizeof(uint64_t), &ack, sizeof(ack_t));
                            send_to_cloud(MSG_OPERATION_ACK, wrapped_resp, wrapped_ack_size);
                        }
                        
                        free(wrapped_resp);
                    }
                } else {
                    ack_t nack = { .success = 0 };
                    snprintf(nack.details, sizeof(nack.details), "Coordinator request failed.");
                    
                    size_t wrapped_nack_size = sizeof(uint64_t) + sizeof(ack_t);
                    char* wrapped_nack = malloc(wrapped_nack_size);
                    if (wrapped_nack) {
                        memcpy(wrapped_nack, &request_id, sizeof(uint64_t));
                        memcpy(wrapped_nack + sizeof(uint64_t), &nack, sizeof(ack_t));
                        send_to_cloud(MSG_OPERATION_ACK, wrapped_nack, wrapped_nack_size);
                        free(wrapped_nack);
                    }
                }
            }
            
            free(req_to_process->payload);
            free(req_to_process);

        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1; 
            
            pthread_mutex_lock(&g_request_queue_mutex);
            if (g_keep_running && g_request_queue_head == NULL) {
                pthread_cond_timedwait(&g_request_queue_cond, &g_request_queue_mutex, &ts);
            }
            pthread_mutex_unlock(&g_request_queue_mutex);
        }
    }
    
    log_msg("Coordinator client thread stopping.");
    return NULL;
}

// --- HTTP Server Thread (for Coordinator) ---

void* handle_coordinator_request(void* arg) {
    int sock_fd = (int)(intptr_t)arg;
    // Buffer for headers + small bodies.
    char* buffer = malloc(MAX_HTTP_BODY_SIZE + 1024);
    if (!buffer) { 
        close(sock_fd); 
        return NULL; 
    }
    // Use a separate, smaller buffer for just the headers.
    char header_buf[1024]; 
    
    ssize_t n = read(sock_fd, buffer, MAX_HTTP_BODY_SIZE + 1023);
    if (n <= 0) {
        close(sock_fd);
        free(buffer);
        return NULL;
    }
    buffer[n] = '\0';
    
    char* saveptr_line;
    char* method_path_line;
    char* body = strstr(buffer, "\r\n\r\n");

    if (body) {
        *body = '\0'; 
        body += 4;    
    }
    
    method_path_line = strtok_r(buffer, "\r\n", &saveptr_line);
    if (!method_path_line) {
        close(sock_fd);
        free(buffer);
        return NULL;
    }

    log_msg("HTTP Server: Received request: %s", method_path_line);
    
    char* saveptr_method;
    char* method = strtok_r(method_path_line, " ", &saveptr_method);
    char* path = strtok_r(NULL, " ", &saveptr_method);
    if (!method || !path) {
        close(sock_fd);
        free(buffer); 
        return NULL;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/nodes_list") == 0) {
        pthread_mutex_lock(&g_node_cache_mutex);
        
        // --- START: MODIFIED LOGIC ---
        if (g_local_node_list_json) {
            size_t body_len = strlen(g_local_node_list_json);
            // 1. Send the headers
            snprintf(header_buf, sizeof(header_buf), 
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: %zu\r\n"
                "Connection: close\r\n\r\n", // Add Connection: close
                body_len
            );
            write(sock_fd, header_buf, strlen(header_buf));
            
            // 2. Send the (potentially large) JSON body
            write(sock_fd, g_local_node_list_json, body_len);

        } else {
            const char* err = "[]";
            size_t body_len = strlen(err);
            snprintf(header_buf, sizeof(header_buf), 
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: %zu\r\n"
                "Connection: close\r\n\r\n",
                body_len
            );
            write(sock_fd, header_buf, strlen(header_buf));
            write(sock_fd, err, body_len);
        }
        // --- END: MODIFIED LOGIC ---
        
        pthread_mutex_unlock(&g_node_cache_mutex);
        
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/sync_incoming") == 0) {
        if (body) {
            char error_buf[128];
            ctz_json_value* root = ctz_json_parse(body, error_buf, sizeof(error_buf));
            if (root) {
                const char* source_unit = ctz_json_get_string(ctz_json_find_object_value(root, "source_unit"));
                const char* target_node = ctz_json_get_string(ctz_json_find_object_value(root, "target_node"));
                
                char* payload_json = ctz_json_stringify(root, 0);

                if (source_unit && target_node && payload_json) {
                    size_t json_len = strlen(payload_json);
                    size_t total_size = sizeof(sig_sync_data_t) + json_len + 1;
                    
                    sig_sync_data_t* sync_data = malloc(total_size);
                    if (sync_data) {
                        strncpy(sync_data->source_unit, source_unit, sizeof(sync_data->source_unit) - 1);
                        strncpy(sync_data->target_node, target_node, sizeof(sync_data->target_node) - 1);
                        memcpy(sync_data->sync_payload_json, payload_json, json_len + 1);
                        
                        send_to_cloud(MSG_SIG_SYNC_DATA, sync_data, total_size);
                        free(sync_data);
                    }
                }
                if (payload_json) free(payload_json);
                ctz_json_free(root);
            }
        }
        
        const char* ok_resp = "HTTP/1.1 200 OK\r\n"
                              "Content-Type: application/json\r\n"
                              "Content-Length: 15\r\n"
                              "Connection: close\r\n\r\n{\"status\":\"ok\"}";
        write(sock_fd, ok_resp, strlen(ok_resp));
        
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/push_incoming") == 0) {
        
        if (g_storage_path[0] == '\0') {
             const char* err = "HTTP/1.1 500 Server Error\r\n\r\nNo Designated Storage Path Set.";
             write(sock_fd, err, strlen(err));
             close(sock_fd); free(buffer); return NULL;
        }

        // 1. Extract Node Name from Headers
        char* node_name_line = strstr(buffer, "X-Node-Name:");
        if (!node_name_line) {
             const char* err = "HTTP/1.1 400 Bad Request\r\n\r\nMissing X-Node-Name header.";
             write(sock_fd, err, strlen(err));
             close(sock_fd); free(buffer); return NULL;
        }
        char* node_name_start = node_name_line + 12;
        while(*node_name_start == ' ') node_name_start++;
        char* node_name_end = strstr(node_name_start, "\r\n");
        char node_name[MAX_NODE_NAME_LEN] = {0};
        if (node_name_end) strncpy(node_name, node_name_start, node_name_end - node_name_start);
        
        log_msg("Receiving Push: Node '%s' into designated path '%s'", node_name, g_storage_path);

        // 2. Prepare Filesystem
        char target_dir[PATH_MAX];
        snprintf(target_dir, sizeof(target_dir), "%s/%s", g_storage_path, node_name);
        mkdir(g_storage_path, 0755);
        mkdir(target_dir, 0755);

        char temp_archive[PATH_MAX];
        snprintf(temp_archive, sizeof(temp_archive), "%s/%s.tar", g_storage_path, node_name);

        // 3. Stream Data to Disk
        FILE* f_tmp = fopen(temp_archive, "wb");
        if (!f_tmp) {
             const char* err = "HTTP/1.1 500 Server Error\r\n\r\nDisk Write Failed.";
             write(sock_fd, err, strlen(err));
             close(sock_fd); free(buffer); return NULL;
        }

        // Write what we already read into the buffer
        char* body_ptr = body; // 'body' pointer calculated earlier in function
        ssize_t initial_bytes = n - (body_ptr - buffer);
        if (initial_bytes > 0) fwrite(body_ptr, 1, initial_bytes, f_tmp);

        // Pump the rest from socket
        char stream_buf[65536];
        ssize_t r;
        size_t total_received = initial_bytes;
        
        // Set a receive timeout just in case
        struct timeval tv; tv.tv_sec = 30; tv.tv_usec = 0;
        setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

        while ((r = read(sock_fd, stream_buf, sizeof(stream_buf))) > 0) {
            fwrite(stream_buf, 1, r, f_tmp);
            total_received += r;
        }
        fclose(f_tmp);
        
        log_msg("Push received (%zu bytes). Unpacking...", total_received);

        // 4. Unpack
        char cmd[PATH_MAX * 3];
        // Fast tar extraction
        snprintf(cmd, sizeof(cmd), "tar -xf \"%s\" -C \"%s\"", temp_archive, target_dir);
        int unpack_ret = system(cmd);
        
        remove(temp_archive); // Cleanup temp file

        if (unpack_ret == 0) {
            const char* done = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nPush Successful.";
            write(sock_fd, done, strlen(done));
            log_msg("Node '%s' successfully pushed and unpacked.", node_name);
        } else {
            const char* err = "HTTP/1.1 500 Server Error\r\n\r\nUnpack Failed.";
            write(sock_fd, err, strlen(err));
            log_msg("Error unpacking node '%s'.", node_name);
        }
    } else {
        const char* not_found_resp = "HTTP/1.1 404 Not Found\r\n"
                                     "Content-Type: text/plain\r\n"
                                     "Content-Length: 9\r\n"
                                     "Connection: close\r\n\r\nNot Found";
        write(sock_fd, not_found_resp, strlen(not_found_resp));
    }
    
    close(sock_fd);
    free(buffer); 
    return NULL;
}

void* http_server_thread(void* arg) {
    (void)arg;
    log_msg("HTTP server thread started on port %d.", SIGNAL_HTTP_PORT);

    // int server_fd; <-- Now global g_server_fd
    struct sockaddr_in address;
    int opt = 1;
    socklen_t addrlen = sizeof(address);

    if ((g_server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) { // <-- Use global
        log_msg("HTTP Server Error: socket failed"); return NULL;
    }
    if (setsockopt(g_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        log_msg("HTTP Server Error: setsockopt failed"); return NULL;
    }
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(SIGNAL_HTTP_PORT);

    if (bind(g_server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        log_msg("HTTP Server Error: bind failed on port %d", SIGNAL_HTTP_PORT); return NULL;
    }
    if (listen(g_server_fd, 10) < 0) {
        log_msg("HTTP Server Error: listen failed"); return NULL;
    }

    while (g_keep_running) {
        int client_sock = accept(g_server_fd, (struct sockaddr*)&address, &addrlen); // <-- BLOCKING
        if (client_sock < 0) {
            // This will trigger when main closes g_server_fd
            if (g_keep_running) {
                log_msg("HTTP Server Error: accept failed: %s", strerror(errno));
            }
            // Whether it's an error or a shutdown, we check g_keep_running
            if (!g_keep_running) {
                break; // Exit loop on shutdown
            }
            continue;
        }
        
        pthread_t conn_thread;
        if (pthread_create(&conn_thread, NULL, handle_coordinator_request, (void*)(intptr_t)client_sock) != 0) {
            log_msg("HTTP Server Error: Failed to create connection thread");
            close(client_sock);
        }
        pthread_detach(conn_thread);
    }

    // We only close the socket here if it hasn't already been closed by main
    if (g_server_fd != -1) {
        close(g_server_fd);
    }
    log_msg("HTTP server thread stopping.");
    return NULL;
}


// --- Main ---

int main() {
    signal(SIGINT, int_handler);
    signal(SIGTERM, int_handler);
    
    log_msg("Initializing Exodus Signal Daemon...");
    
    load_coordinator_config();
    load_unit_config();

    g_mesh = cortez_mesh_init(SIGNAL_DAEMON_NAME, NULL);
    if (!g_mesh) {
        log_msg("Fatal: Could not initialize mesh.");
        return 1;
    }
    
    log_msg("Signal Daemon running with PID: %d", cortez_mesh_get_pid(g_mesh));
    
    while (g_cloud_daemon_pid == 0 && g_keep_running) {
        log_msg("Discovering cloud daemon via PID file...");
        if (discover_cloud_daemon_pid()) {
            break; 
        }
        sleep(2); 
    }
    
    if (!g_keep_running) {
        log_msg("Shutdown initiated during startup.");
        cortez_mesh_shutdown(g_mesh);
        return 0;
    }

    pthread_t mesh_tid, client_tid, server_tid;
    pthread_create(&mesh_tid, NULL, mesh_listener_thread, NULL);

    log_msg("Waiting for initial node list from cloud daemon...");
    while (g_keep_running) {
        pthread_mutex_lock(&g_node_cache_mutex);
        if (g_local_node_list_json != NULL) {
            // Success! Cache is populated.
            pthread_mutex_unlock(&g_node_cache_mutex);
            log_msg("Initial node list received. Starting network services.");
            break;
        }
        pthread_mutex_unlock(&g_node_cache_mutex);
        
        // Sanity check: Make sure cloud daemon is still alive
        if (kill(g_cloud_daemon_pid, 0) != 0) {
            log_msg("Error: Cloud daemon (PID %d) disappeared. Shutting down.", g_cloud_daemon_pid);
            g_keep_running = 0; // Signal all threads to stop
            break;
        }
        
        sleep(1); // Wait and retry
    }
    
    // Check if we broke the loop due to shutdown
    if (!g_keep_running) {
        log_msg("Shutdown initiated while waiting for node list.");
        pthread_join(mesh_tid, NULL); // Wait for mesh listener to stop
        cortez_mesh_shutdown(g_mesh);
        return 0;
    }

    pthread_create(&client_tid, NULL, coordinator_client_thread, NULL);
    pthread_create(&server_tid, NULL, http_server_thread, NULL);

    while (g_keep_running) {
        sleep(1);
    }
    
    log_msg("Shutdown signal received. Stopping threads...");
    
    // --- START: Graceful Shutdown ---
    if (g_server_fd != -1) {
        log_msg("Closing HTTP server socket to unblock accept()...");
        shutdown(g_server_fd, SHUT_RD); // Stop new connections
        close(g_server_fd);         // This will make accept() fail
        g_server_fd = -1;           // Mark as closed
    }
    // --- END: Graceful Shutdown ---

    pthread_join(mesh_tid, NULL);
    pthread_join(client_tid, NULL);
    pthread_join(server_tid, NULL); // <-- This will now unblock
    
    log_msg("Cleaning up...");
    cortez_mesh_shutdown(g_mesh);
    
    free(g_local_node_list_json);
    pthread_mutex_destroy(&g_node_cache_mutex);
    pthread_mutex_destroy(&g_request_queue_mutex);
    pthread_cond_destroy(&g_request_queue_cond);
    
    RequestNode* req = g_request_queue_head;
    while(req) {
        RequestNode* next = req->next;
        free(req->payload);
        free(req);
        req = next;
    }
    
    log_msg("Exodus Signal Daemon stopped.");
    return 0;
}