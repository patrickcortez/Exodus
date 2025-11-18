/*
 * exodus-coordinator.c
 * Standalone, self-hosted HTTP server for LAN unit discovery and routing.
 *
 * COMPILE:
 * gcc -Wall -Wextra -O2 exodus-coordinator.c ctz-json.a -o exodus-coordinator -pthread
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

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>

#include "ctz-json.h" // We only need ctz-json.h, not exodus-common.h

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define COORDINATOR_PORT 8080 // Port this server listens on
#define UNIT_TIMEOUT_SECONDS 90 // Time before a unit is considered "offline"
#define MAX_HTTP_BODY_SIZE (50 * 1024 * 1024)

// --- Data Structures ---

typedef struct Unit {
    char name[128];
    char ip_addr[64];
    int signal_port;
    time_t last_seen;
    struct Unit* next;
} Unit;

static Unit* g_unit_list_head = NULL;
static pthread_mutex_t g_unit_list_mutex = PTHREAD_MUTEX_INITIALIZER;

static volatile int g_keep_running = 1;

// --- Utility Functions ---

void log_msg(const char* format, ...) {
    va_list args;
    va_start(args, format);
    printf("[Coordinator] ");
    vprintf(format, args);
    printf("\n");
    fflush(stdout);
    va_end(args);
}

void int_handler(int dummy) {
    (void)dummy;
    g_keep_running = 0;
}

// Simple blocking HTTP request function
int send_http_request(const char* host, int port, const char* request, char* response_buf, size_t response_size) {
    struct hostent* server = gethostbyname(host);
    if (server == NULL) {
        log_msg("HTTP Client Error: Could not resolve host: %s", host);
        return -1;
    }

    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        log_msg("HTTP Client Error: Could not create socket");
        return -1;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);

    if (connect(sock_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        log_msg("HTTP Client Error: Could not connect to %s:%d", host, port);
        close(sock_fd);
        return -1;
    }

    if (write(sock_fd, request, strlen(request)) < 0) {
        log_msg("HTTP Client Error: Failed to write to socket");
        close(sock_fd);
        return -1;
    }

    size_t total_read = 0;
    ssize_t n;
    while (total_read < response_size - 1) {
        // Read into the buffer *after* the data we already have
        n = read(sock_fd, response_buf + total_read, (response_size - 1) - total_read);
        
        if (n < 0) {
            // A real read error
            log_msg("HTTP Client Error: Failed to read response: %s", strerror(errno));
            close(sock_fd);
            return -1;
        }
        
        if (n == 0) {
            // End of file (connection closed by server), this is the normal exit
            break;
        }
        
        total_read += n;
    }
    
    response_buf[total_read] = '\0'; // Null-terminate the full response
    
    if (total_read == response_size - 1) {
         log_msg("HTTP Client Warning: Response buffer may be full. Response might be truncated.");
    }

    close(sock_fd);
    
    if (strstr(response_buf, "HTTP/1.1 200 OK") == NULL) {
        log_msg("HTTP Client Error: Target Unit returned non-200 status.");
        return -1;
    }
    
    return 0; // Success
}

// Finds a unit, updates it, or creates it
void register_unit(const char* name, const char* ip, int port) {
    pthread_mutex_lock(&g_unit_list_mutex);
    
    Unit* unit = g_unit_list_head;
    while (unit) {
        if (strcmp(unit->name, name) == 0) {
            // Found it, update info
            strncpy(unit->ip_addr, ip, sizeof(unit->ip_addr) - 1);
            unit->signal_port = port;
            unit->last_seen = time(NULL);
            log_msg("Unit re-registered: %s at %s:%d", name, ip, port);
            pthread_mutex_unlock(&g_unit_list_mutex);
            return;
        }
        unit = unit->next;
    }
    
    // Not found, create new one
    Unit* new_unit = malloc(sizeof(Unit));
    if (new_unit) {
        strncpy(new_unit->name, name, sizeof(new_unit->name) - 1);
        strncpy(new_unit->ip_addr, ip, sizeof(new_unit->ip_addr) - 1);
        new_unit->signal_port = port;
        new_unit->last_seen = time(NULL);
        new_unit->next = g_unit_list_head;
        g_unit_list_head = new_unit;
        log_msg("New unit registered: %s at %s:%d", name, ip, port);
    }
    
    pthread_mutex_unlock(&g_unit_list_mutex);
}

// Finds a unit, returns 0 and fills buffers if successful
int find_unit(const char* name, char* ip_buf, size_t ip_size, int* port_out) {
    int found = 0;
    pthread_mutex_lock(&g_unit_list_mutex);
    
    Unit* unit = g_unit_list_head;
    while (unit) {
        if (strcmp(unit->name, name) == 0) {
            if (time(NULL) < unit->last_seen + UNIT_TIMEOUT_SECONDS) {
                // Found and it's online
                strncpy(ip_buf, unit->ip_addr, ip_size - 1);
                *port_out = unit->signal_port;
                found = 1;
            }
            break;
        }
        unit = unit->next;
    }
    
    pthread_mutex_unlock(&g_unit_list_mutex);
    return found ? 0 : -1;
}

// Helper to send a simple HTTP response
void send_response(int sock_fd, const char* status_line, const char* content_type, const char* body) {
    char response[8192];
    snprintf(response, sizeof(response),
        "%s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n%s",
        status_line, content_type, strlen(body), body
    );
    write(sock_fd, response, strlen(response));
}

// --- Connection Handler Thread ---
void* handle_connection(void* arg) {
    struct {
        int sock_fd;
        char ip_addr[64];
    } conn_info;
    
    memcpy(&conn_info, arg, sizeof(conn_info));
    free(arg); // Free the heap-allocated argument

    int sock_fd = conn_info.sock_fd;
    char* buffer = malloc(MAX_HTTP_BODY_SIZE + 1024);//50mb Buffer
    if (!buffer) { close(sock_fd); return NULL; }
    char http_resp_buf[8192]; // For client requests
    
    ssize_t n = read(sock_fd, buffer, MAX_HTTP_BODY_SIZE + 1023);
    if (n <= 0) {
        close(sock_fd);
        free(buffer);
        return NULL;
    }
    buffer[n] = '\0';
    
    // --- Parse Request ---
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

    char* saveptr_method;
    char* method = strtok_r(method_path_line, " ", &saveptr_method);
    char* path = strtok_r(NULL, " ", &saveptr_method);
    if (!method || !path) {
        close(sock_fd);
        free(buffer);
        return NULL;
    }
    
    // --- Route: POST /register ---
    if (strcmp(method, "POST") == 0 && strcmp(path, "/register") == 0) {
        if (body) {
            char error_buf[128];
            ctz_json_value* root = ctz_json_parse(body, error_buf, sizeof(error_buf));
            if (root) {
                const char* unit_name = ctz_json_get_string(ctz_json_find_object_value(root, "unit_name"));
                int listen_port = (int)ctz_json_get_number(ctz_json_find_object_value(root, "listen_port"));
                
                if (unit_name && listen_port > 0) {
                    register_unit(unit_name, conn_info.ip_addr, listen_port);
                    send_response(sock_fd, "HTTP/1.1 200 OK", "application/json", "{\"status\":\"registered\"}");
                } else {
                    send_response(sock_fd, "HTTP/1.1 400 Bad Request", "application/json", "{\"error\":\"missing unit_name or listen_port\"}");
                }
                ctz_json_free(root);
            } else {
                send_response(sock_fd, "HTTP/1.1 400 Bad Request", "application/json", "{\"error\":\"invalid json\"}");
            }
        } else {
            send_response(sock_fd, "HTTP/1.1 400 Bad Request", "application/json", "{\"error\":\"missing body\"}");
        }
        
    // --- Route: GET /units ---
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/units") == 0) {
        ctz_json_value* root = ctz_json_new_array();
        time_t now = time(NULL);
        
        pthread_mutex_lock(&g_unit_list_mutex);
        for (Unit* u = g_unit_list_head; u; u = u->next) {
            ctz_json_value* unit_obj = ctz_json_new_object();
            ctz_json_object_set_value(unit_obj, "name", ctz_json_new_string(u->name));
            if (now < u->last_seen + UNIT_TIMEOUT_SECONDS) {
                ctz_json_object_set_value(unit_obj, "status", ctz_json_new_string("online"));
            } else {
                ctz_json_object_set_value(unit_obj, "status", ctz_json_new_string("offline"));
            }
            ctz_json_array_push_value(root, unit_obj);
        }
        pthread_mutex_unlock(&g_unit_list_mutex);
        
        char* json_body = ctz_json_stringify(root, 0);
        send_response(sock_fd, "HTTP/1.1 200 OK", "application/json", json_body ? json_body : "[]");
        if (json_body) free(json_body);
        ctz_json_free(root);
    
    // --- Route: GET /nodes?target_unit=... ---
    } else if (strcmp(method, "GET") == 0 && strncmp(path, "/nodes?target_unit=", 19) == 0) {
        const char* target_name = path + 19;
        char target_ip[64];
        int target_port;
        
        if (find_unit(target_name, target_ip, sizeof(target_ip), &target_port) == 0) {
            // Found unit, now ask it for its node list
            char http_req[512];
            snprintf(http_req, sizeof(http_req),
                "GET /nodes_list HTTP/1.1\r\n"
                "Host: %s:%d\r\n"
                "Connection: close\r\n\r\n",
                target_ip, target_port
            );
            
            if (send_http_request(target_ip, target_port, http_req, http_resp_buf, sizeof(http_resp_buf)) == 0) {
                // Success! Forward the body of the response
                char* body_start = strstr(http_resp_buf, "\r\n\r\n");
                if (body_start) {
                    send_response(sock_fd, "HTTP/1.1 200 OK", "application/json", body_start + 4);
                } else {
                    send_response(sock_fd, "HTTP/1.1 500 Server Error", "application/json", "{\"error\":\"invalid response from target unit\"}");
                }
            } else {
                send_response(sock_fd, "HTTP/1.1 504 Gateway Timeout", "application/json", "{\"error\":\"could not reach target unit\"}");
            }
        } else {
            send_response(sock_fd, "HTTP/1.1 404 Not Found", "application/json", "{\"error\":\"target unit not found or offline\"}");
        }
    
    // --- Route: POST /sync ---
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/sync") == 0) {
        if (body) {
            char error_buf[128];
            ctz_json_value* root = ctz_json_parse(body, error_buf, sizeof(error_buf));
            if (root) {
                const char* target_unit = ctz_json_get_string(ctz_json_find_object_value(root, "target_unit"));
                
                char target_ip[64];
                int target_port;
                if (target_unit && find_unit(target_unit, target_ip, sizeof(target_ip), &target_port) == 0) {
                    char* body_to_forward = ctz_json_stringify(root, 0); 
                    size_t body_len = strlen(body_to_forward);
                    
                    char* http_req = malloc(body_len + 1024); // body + 1KB for headers
                    if (!http_req) {
                        send_response(sock_fd, "HTTP/1.1 500 Server Error", "application/json", "{\"error\":\"out of memory\"}");
                        free(body_to_forward);
                        ctz_json_free(root);
                        close(sock_fd);
                        free(buffer);
                        return NULL;
                    }

                    snprintf(http_req, body_len + 1024,
                        "POST /sync_incoming HTTP/1.1\r\n"
                        "Host: %s:%d\r\n"
                        "Content-Type: application/json\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: close\r\n\r\n%s",
                        target_ip, target_port, body_len, body_to_forward
                    );
                    
                    if (send_http_request(target_ip, target_port, http_req, http_resp_buf, sizeof(http_resp_buf)) == 0) {
                        send_response(sock_fd, "HTTP/1.1 200 OK", "application/json", "{\"status\":\"sync forwarded\"}");
                    } else {
                        send_response(sock_fd, "HTTP/1.1 504 Gateway Timeout", "application/json", "{\"error\":\"target unit did not accept sync\"}");
                    }
                    free(body_to_forward);
                } else {
                    send_response(sock_fd, "HTTP/1.1 404 Not Found", "application/json", "{\"error\":\"target unit not found or offline\"}");
                }
                ctz_json_free(root);
            } else {
                send_response(sock_fd, "HTTP/1.1 400 Bad Request", "application/json", "{\"error\":\"invalid json\"}");
            }
        } else {
            send_response(sock_fd, "HTTP/1.1 400 Bad Request", "application/json", "{\"error\":\"missing body\"}");
        }

    // --- Route: 404 Not Found (Default) ---
    } else {
        send_response(sock_fd, "HTTP/1.1 404 Not Found", "application/json", "{\"error\":\"endpoint not found\"}");
    }
    free(buffer);
    close(sock_fd);
    return NULL;
}


int main() {
    signal(SIGINT, int_handler);
    signal(SIGTERM, int_handler);
    
    // Ignore SIGPIPE so we don't crash if a client disconnects
    signal(SIGPIPE, SIG_IGN); 

    log_msg("Starting Exodus Coordinator on port %d...", COORDINATOR_PORT);

    int server_fd;
    struct sockaddr_in address;
    int opt = 1;
    socklen_t addrlen = sizeof(address);

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        log_msg("Fatal: socket failed"); return 1;
    }
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        log_msg("Fatal: setsockopt failed"); return 1;
    }
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(COORDINATOR_PORT);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        log_msg("Fatal: bind failed on port %d", COORDINATOR_PORT); return 1;
    }
    if (listen(server_fd, 20) < 0) {
        log_msg("Fatal: listen failed"); return 1;
    }
    
    // Set server socket to non-blocking
    fcntl(server_fd, F_SETFL, O_NONBLOCK);

    log_msg("Coordinator is live. Waiting for connections...");

    while (g_keep_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        
        if (client_sock < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                // No pending connections
                sleep(1); // Wait 1 second
            } else if (errno == EINTR) {
                // Interrupted by signal
                continue;
            } else {
                if (g_keep_running) log_msg("Error: accept failed: %s", strerror(errno));
            }
            continue;
        }
        
        // --- Connection Accepted ---
        
        // We must pass the connection info on the heap because the loop
        // will immediately overwrite the stack variables.
        void* conn_info_heap = malloc(sizeof(int) + 64);
        if (!conn_info_heap) {
            log_msg("Error: malloc failed for conn_info. Dropping connection.");
            close(client_sock);
            continue;
        }
        
        *(int*)conn_info_heap = client_sock;
        inet_ntop(AF_INET, &client_addr.sin_addr, (char*)conn_info_heap + sizeof(int), 64);
        
        log_msg("Accepted connection from %s", (char*)conn_info_heap + sizeof(int));
        
        pthread_t conn_thread;
        if (pthread_create(&conn_thread, NULL, handle_connection, conn_info_heap) != 0) {
            log_msg("Error: Failed to create connection thread");
            close(client_sock);
            free(conn_info_heap);
        }
        pthread_detach(conn_thread); // We don't need to join it
    }

    close(server_fd);
    log_msg("Coordinator shutting down.");
    
    // Free unit list
    Unit* unit = g_unit_list_head;
    while(unit) {
        Unit* next = unit->next;
        free(unit);
        unit = next;
    }
    
    pthread_mutex_destroy(&g_unit_list_mutex);
    return 0;
}