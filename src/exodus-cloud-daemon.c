/*
 * Compile Command:
 * gcc -Wall -Wextra -O2 exodus-cloud-daemon.c cortez-mesh.o ctz-json.a -o cloud_daemon -pthread
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <ctype.h>
#include <stddef.h>
#include <ctype.h>
#include <pthread.h>
#include <sys/inotify.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h> 
#include <ctype.h>
#include <ftw.h>
#include <fcntl.h>
#include <stdint.h>  
#include <sys/uio.h>
#include <sys/wait.h>

#include "cortez-mesh.h"
#include "exodus-common.h"
#include "ctz-json.h"



#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_SENTENCE_LEN 256
#define MAX_SENTENCES 10

#define NODE_CONFIG_FILE "nodewatch.json"

typedef struct WatchedNode WatchedNode;

// Maps a watch descriptor (wd) to a path and its parent node
typedef struct WatchDescriptorMap {
    int wd;
    char path[PATH_MAX];
    WatchedNode* parent_node;
    struct WatchDescriptorMap* next;
} WatchDescriptorMap;

// Caches file content to detect changes for detailed logging
typedef struct FileCache {
    char path[PATH_MAX];
    char* content;
    struct FileCache* next;
    time_t last_processed_time;
} FileCache;

// Represents a single file system event in a node's history
typedef enum {
    EV_CREATED,
    EV_DELETED,
    EV_MODIFIED,
    EV_MOVED
} EventType;

typedef struct NodeEvent {
    EventType type;
    char name[MAX_PATH_LEN];
    time_t timestamp;
    struct NodeEvent* next;
} NodeEvent;

typedef enum {
    TIME_UNIX,
    TIME_REAL
} TimeFormat;

typedef struct FilterEntry {
    char extension[32];
    struct FilterEntry* next;
} FilterEntry;

typedef struct DiffChange {
    char op; 
    int line_num;
    const char* content;
    struct DiffChange* next;
    int matched;
} DiffChange;

typedef struct MovedChange {
    int from_line;
    int to_line;
    const char* content;
    struct MovedChange* next;
} MovedChange;

// Represents a directory being watched by the daemon
struct WatchedNode {
    char name[MAX_NODE_NAME_LEN];
    char path[PATH_MAX];
    char author[MAX_ATTR_LEN];
    char current_version[MAX_NODE_NAME_LEN];
    char type[32];
    char desc[MAX_ATTR_LEN];
    char tag[MAX_ATTR_LEN];
    int active;
    int is_auto;
    char conf_path[PATH_MAX];
    NodeEvent* history_head;
    WatchedNode* next;
    TimeFormat time_format;
    FilterEntry* filter_list_head;
};

typedef struct PendingMove {
    uint32_t cookie;
    char from_path[PATH_MAX];
    WatchedNode* from_node;
    time_t timestamp;
    char user[64];
    struct PendingMove* next;
} PendingMove;

// --- Global Variables ---

static char* file_content = NULL;
static size_t file_size = 0;
static char last_uploaded_file_path[PATH_MAX] = {0};
static pid_t guardian_daemon_pid = 0;
static char g_exe_dir[PATH_MAX] = {0};
static pid_t g_signal_daemon_pid = 0;
 

static PendingMove* pending_move_head = NULL;
static WatchDescriptorMap* wd_map_head = NULL;
static FileCache* file_cache_head = NULL;
static WatchedNode* watched_nodes_head = NULL;

static pthread_mutex_t wd_map_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t file_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t node_list_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t pending_move_mutex = PTHREAD_MUTEX_INITIALIZER;


static char config_file_path[PATH_MAX] = {0};
static int inotify_fd = -1;


// Forward declare new functions
int get_executable_dir(char* buffer, size_t size);
void load_nodes();
void save_nodes();
void* watcher_thread_func(void* arg);
void recursive_scan_dir(const char* base_path, ctz_json_value* json_array);
void add_watches_recursively(WatchedNode* node, const char* base_path);
void remove_all_watches_for_node(WatchedNode* node);
void handle_file_modification(WatchedNode* node, const char* full_path, const char* user);
void add_event_to_node(WatchedNode* node, EventType type, const char* name, const char* user, const char* details_json_obj);
char* read_file_content(const char* path);
void update_file_cache(const char* path, const char* content);
int recursive_delete(const char* path); 
int copy_file(const char* src, const char* dest); 
int recursive_copy(const char* src, const char* dest);

static ssize_t robust_read(int fd, void *buf, size_t count) {
    size_t done = 0;
    while (done < count) {
        ssize_t r = read(fd, (char*)buf + done, count - done);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) break;
        done += r;
    }
    return (ssize_t)done;
}

static int secure_file_delete(const char *path) {
    // Open file for read/write. Do NOT create if missing.
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("[Cloud] secure_file_delete: open");
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("[Cloud] secure_file_delete: fstat");
        close(fd);
        return -1;
    }

    // If it's not a regular file, refuse.
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "[Cloud] secure_file_delete: Not a regular file: %s\n", path);
        close(fd);
        return -1;
    }

    off_t orig_size = st.st_size;
    if (orig_size == 0) {
        // If empty file, just unlink and exit.
        if (unlink(path) < 0) {
            perror("[Cloud] secure_file_delete: unlink (empty file)");
            close(fd);
            return -1;
        }
        close(fd);
        return 0;
    }

    // Remove the name so the file is gone from directory entries immediately.
    if (unlink(path) < 0) {
        perror("[Cloud] secure_file_delete: unlink");
        close(fd);
        return -1;
    }

    // Open /dev/urandom to get overwrite bytes
    int rnd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (rnd < 0) {
        perror("[Cloud] secure_file_delete: /dev/urandom");
        fprintf(stderr, "[Cloud] Falling back to zero-fill (less secure)\n");
    }

    const size_t BUFSZ = 16*1024;
    uint8_t *buf = malloc(BUFSZ);
    if (!buf) {
        fprintf(stderr, "[Cloud] secure_file_delete: malloc failed\n");
        if (rnd >= 0) close(rnd);
        close(fd);
        return -1;
    }

    off_t remaining = orig_size;
    off_t offset = 0;
    while (remaining > 0) {
        size_t to_write = (remaining > (off_t)BUFSZ) ? BUFSZ : (size_t)remaining;
        
        // Fill buffer from /dev/urandom if possible
        if (rnd >= 0) {
            ssize_t got = robust_read(rnd, buf, to_write);
            if (got < 0 || (size_t)got != to_write) {
                fprintf(stderr, "[Cloud] secure_file_delete: reading /dev/urandom failed; falling back to zeros\n");
                memset(buf, 0, to_write);
                // Don't close rnd, just use zeros from now on
                close(rnd);
                rnd = -1;
            }
        } else {
            memset(buf, 0, to_write);
        }

        // Use pwrite to write at the current offset
        ssize_t w = pwrite(fd, buf, to_write, offset);
        if (w < 0) {
            if (errno == EINTR) continue;
            perror("[Cloud] secure_file_delete: pwrite");
            free(buf);
            if (rnd >= 0) close(rnd);
            close(fd);
            return -1;
        }

        offset += w;
        remaining -= w;
    }

    // Ensure data is on stable storage.
    if (fdatasync(fd) < 0) {
        perror("[Cloud] secure_file_delete: fdatasync");
    }
    if (fsync(fd) < 0) {
        perror("[Cloud] secure_file_delete: fsync");
    }

    // Optionally truncate to 0 bytes
    if (ftruncate(fd, 0) < 0) {
        perror("[Cloud] secure_file_delete: ftruncate");
    } else {
        if (fsync(fd) < 0) perror("[Cloud] secure_file_delete: fsync after ftruncate");
    }

    // cleanup
    free(buf);
    if (rnd >= 0) close(rnd);

    // Closing the fd will allow kernel to free inode/blocks.
    if (close(fd) < 0) {
        perror("[Cloud] secure_file_delete: close");
        return -1;
    }

    return 0; // Success
}

static int split_lines(char* content_copy, char*** lines_array_out) {
    if (!content_copy) {
        *lines_array_out = NULL;
        return 0;
    }

    int line_count = 0;
    
    for (char* p = content_copy; *p; p++) {
        if (*p == '\n') {
            line_count++;
        }
    }
    // Account for the last line if the file doesn't end with \n
    if (content_copy[0] != '\0' && (line_count == 0 || content_copy[strlen(content_copy) - 1] != '\n')) {
         line_count++;
    }

    if (line_count == 0) {
        *lines_array_out = NULL;
        return 0;
    }

    // 2. Allocate the array
    *lines_array_out = malloc(line_count * sizeof(char*));
    if (!*lines_array_out) return 0;
    
    int current_line = 0;
    char* line_start = content_copy;
    
    // Always add the first line (even if the file is just "hello")
    (*lines_array_out)[current_line++] = line_start;

    // Iterate and manually split the string
    for (char* p = content_copy; *p; p++) {
        if (*p == '\n') {
            *p = '\0'; // Terminate the current line string

            if (current_line < line_count) {
                (*lines_array_out)[current_line++] = p + 1;
            }
        }
    }
    
    return current_line;
}

static void free_lcs_matrix(int** matrix, int rows) {
    if (!matrix) return;
    for (int i = 0; i < rows; i++) {
        free(matrix[i]);
    }
    free(matrix);
}

static unsigned char* base64_decode(const char* data, size_t input_length, size_t* output_length) {
    static const int b64_inv_table[] = { 
        -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
        -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
        -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,62, -1,-1,-1,63,
        52,53,54,55, 56,57,58,59, 60,61,-1,-1, -1, 0,-1,-1, // Note: 0 is for '=' padding
        -1, 0, 1, 2,  3, 4, 5, 6,  7, 8, 9,10, 11,12,13,14,
        15,16,17,18, 19,20,21,22, 23,24,25,-1, -1,-1,-1,-1,
        -1,26,27,28, 29,30,31,32, 33,34,35,36, 37,38,39,40,
        41,42,43,44, 45,46,47,48, 49,50,51,-1, -1,-1,-1,-1
    };

    if (input_length % 4 != 0) return NULL;
    *output_length = (input_length / 4) * 3;
    if (data[input_length - 1] == '=') (*output_length)--;
    if (data[input_length - 2] == '=') (*output_length)--;

    unsigned char* decoded_data = malloc(*output_length);
    if (decoded_data == NULL) return NULL;

    for (size_t i = 0, j = 0; i < input_length;) {
        uint32_t sextet_a = data[i] == '=' ? 0 & i++ : b64_inv_table[(int)data[i++]];
        uint32_t sextet_b = data[i] == '=' ? 0 & i++ : b64_inv_table[(int)data[i++]];
        uint32_t sextet_c = data[i] == '=' ? 0 & i++ : b64_inv_table[(int)data[i++]];
        uint32_t sextet_d = data[i] == '=' ? 0 & i++ : b64_inv_table[(int)data[i++]];
        uint32_t triple = (sextet_a << 3 * 6) + (sextet_b << 2 * 6) + (sextet_c << 1 * 6) + (sextet_d << 0 * 6);

        if (j < *output_length) decoded_data[j++] = (triple >> 2 * 8) & 0xFF;
        if (j < *output_length) decoded_data[j++] = (triple >> 1 * 8) & 0xFF;
        if (j < *output_length) decoded_data[j++] = (triple >> 0 * 8) & 0xFF;
    }
    return decoded_data;
}

static int get_home_and_name_from_uid(uid_t uid, char* name_buf, size_t name_size, char* home_buf, size_t home_size) {
    FILE* f = fopen("/etc/passwd", "r");
    if (!f) {
        perror("[Cloud] Error: Could not open /etc/passwd");
        return -1;
    }

    char line[1024];
    char* saveptr;
    int found = 0;

    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        char* line_copy = strdup(line);
        if (line_copy == NULL) continue;

        char* name = strtok_r(line_copy, ":", &saveptr);
        if (name == NULL) { free(line_copy); continue; }
        
        char* pass = strtok_r(NULL, ":", &saveptr);
        if (pass == NULL) { free(line_copy); continue; }
        
        char* uid_str = strtok_r(NULL, ":", &saveptr);
        if (uid_str == NULL) { free(line_copy); continue; }

        if (atoi(uid_str) == (int)uid) {
            char* gid_str = strtok_r(NULL, ":", &saveptr);
            if (gid_str == NULL) { free(line_copy); continue; }
            
            char* gecos = strtok_r(NULL, ":", &saveptr);
            if (gecos == NULL) { free(line_copy); continue; }
            
            char* home_dir = strtok_r(NULL, ":", &saveptr);
            if (home_dir) {
                strncpy(home_buf, home_dir, home_size - 1);
                home_buf[home_size - 1] = '\0';
                
                strncpy(name_buf, name, name_size - 1);
                name_buf[name_size - 1] = '\0';
                found = 1;
            }
            free(line_copy);
            break;
        }
        free(line_copy);
    }
    fclose(f);
    
    if (!found) {
         fprintf(stderr, "[Cloud] Could not find user for UID %d in /etc/passwd\n", (int)uid);
    }
    return found ? 0 : -1;
}

static int get_user_dbus_address(uid_t user_id, char* bus_addr_buf, size_t buf_size) {
    char cmd[256];
    // Find the 'systemd --user' process PID for the target user
    snprintf(cmd, sizeof(cmd), "/usr/bin/pgrep -u %d -f \"systemd --user\" | /usr/bin/head -n 1", (int)user_id);
    
    FILE* pp = popen(cmd, "r");
    if (!pp) return -1;
    
    char pid_str[32];
    if (fgets(pid_str, sizeof(pid_str), pp) == NULL) {
        pclose(pp);
        return -1; // No process found
    }
    pclose(pp);
    
    pid_t pid = atoi(pid_str);
    if (pid <= 1) return -1;

    // Now read the environment of that process
    char env_path[PATH_MAX];
    snprintf(env_path, sizeof(env_path), "/proc/%d/environ", pid);
    
    int fd = open(env_path, O_RDONLY);
    if (fd < 0) return -1;
    
    // 8KB buffer should be more than enough for environment strings
    char buffer[8192]; 
    ssize_t bytes_read = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    
    if (bytes_read <= 0) return -1;
    buffer[bytes_read] = '\0'; // Ensure termination

    // Iterate through the null-delimited strings
    char* p = buffer;
    char* end = buffer + bytes_read;
    int found = 0;
    
    while (p < end) {
        if (strncmp(p, "DBUS_SESSION_BUS_ADDRESS=", 25) == 0) {
            strncpy(bus_addr_buf, p + 25, buf_size - 1);
            bus_addr_buf[buf_size - 1] = '\0';
            found = 1;
            break;
        }
        p += strlen(p) + 1; // Move to the next string (past the null byte)
    }
    
    return found ? 0 : -1;
}

static int get_user_from_path(const char* path, char* user_buf, size_t buf_size, uid_t* user_id) {
    struct stat st;
    if (stat(path, &st) != 0) {
        perror("[Cloud] stat failed on node path");
        return -1;
    }
    
    *user_id = st.st_uid;
    
    char home_dir[PATH_MAX]; // We don't use the home dir, just need a buffer
    
    if (get_home_and_name_from_uid(st.st_uid, user_buf, buf_size, home_dir, sizeof(home_dir)) != 0) {
        fprintf(stderr, "[Cloud] Could not find user for UID %d in /etc/passwd\n", (int)st.st_uid);
        return -1;
    }
    
    return 0; // Success
}

static uid_t get_uid_for_path(const char* path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return st.st_uid;
    }
    return (uid_t)-1; // Error
}

static void get_username_from_uid(uid_t uid, char* buf, size_t buf_size) {
    // Default to a fallback name
    snprintf(buf, buf_size, "%d", (int)uid);

    FILE* f = fopen("/etc/passwd", "r");
    if (!f) {
        return; // Use the fallback name
    }

    char line[1024];
    char* saveptr;
    int found = 0;

    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0; // Remove newline
        char* line_copy = strdup(line);
        if (line_copy == NULL) continue;

        // 1. name
        char* name = strtok_r(line_copy, ":", &saveptr);
        if (name == NULL) { free(line_copy); continue; }
        
        // 2. pass
        char* pass = strtok_r(NULL, ":", &saveptr);
        if (pass == NULL) { free(line_copy); continue; }
        
        // 3. uid
        char* uid_str = strtok_r(NULL, ":", &saveptr);
        if (uid_str == NULL) { free(line_copy); continue; }

        if (atoi(uid_str) == (int)uid) {
            strncpy(buf, name, buf_size - 1);
            buf[buf_size - 1] = '\0';
            found = 1;
            free(line_copy);
            break;
        }
        free(line_copy);
    }
    fclose(f);
    
    if (!found) {
        // Fallback already set, just return
        return;
    }
}

static void write_to_handle_and_commit(cortez_mesh_t* mesh, pid_t target_pid, uint16_t msg_type, const void* data, size_t size) {
    int sent_ok = 0;
    for (int i = 0; i < 5; i++) { // Retry 5 times
        cortez_write_handle_t* h = cortez_mesh_begin_send_zc(mesh, target_pid, size);
        if (h) {
            write_to_handle(h, data, size); // Use your existing helper
            cortez_mesh_commit_send_zc(h, msg_type);
            sent_ok = 1;
            break;
        }
        usleep(100000); // Wait 100ms
    }
    if (!sent_ok) {
        fprintf(stderr, "[Cloud] Failed to send message (type %d) to PID %d\n", msg_type, target_pid);
    }
}

WatchedNode* find_node_by_name_locked(const char* name) {
    pthread_mutex_lock(&node_list_mutex);
    for (WatchedNode* n = watched_nodes_head; n; n = n->next) {
        if (strcmp(n->name, name) == 0) {
            // Found it
            pthread_mutex_unlock(&node_list_mutex);
            return n;
        }
    }
    // Not found
    pthread_mutex_unlock(&node_list_mutex);
    return NULL;
}

int get_full_node_path(WatchedNode* node, const char* relative_path, char* full_path_buf, size_t buf_size) {
    if (!node || !relative_path || !full_path_buf) return -1;

    // 1. Basic check for '..'
    if (strstr(relative_path, "..")) {
        return -1; // Disallow ".." completely
    }

    // 2. Construct the path
    snprintf(full_path_buf, buf_size, "%s/%s", node->path, relative_path);

    // 3. Get real paths
    char base_real_path[PATH_MAX];
    char final_real_path[PATH_MAX];

    if (realpath(node->path, base_real_path) == NULL) {
        return -1; // Node base path is invalid
    }

    // realpath needs the path to exist for resolution, except for the last component.
    // For our check, we can use a temporary buffer.
    char temp_path[PATH_MAX];
    strncpy(temp_path, full_path_buf, sizeof(temp_path) - 1);
    
    if (realpath(temp_path, final_real_path) == NULL) {
        // Path doesn't exist yet (e.g., for 'create').
        // Let's resolve the parent directory instead.
        char* last_slash = strrchr(temp_path, '/');
        if (last_slash) {
            *last_slash = '\0';
            if (realpath(temp_path, final_real_path) == NULL) {
                 return -1; // Parent path is invalid
            }
        } else {
            // No slash? Just use the base path.
             realpath(node->path, final_real_path);
        }
    }

    // 4. Check if the resolved path is still inside the base path
    size_t base_len = strlen(base_real_path);
    if (strncmp(base_real_path, final_real_path, base_len) != 0) {
        return -1; // Path traversal detected!
    }
    
    // Ensure it's not just a partial match (e.g., /base/foo vs /base/foobar)
    if (final_real_path[base_len] != '\0' && final_real_path[base_len] != '/') {
         return -1; // Traversal
    }

    return 0; // Success
}

int secure_recursive_delete_callback(const char* fpath, const struct stat* sb, int typeflag, struct FTW* ftwbuf) {
    (void)sb; (void)ftwbuf;
    int rv = -1;

    switch (typeflag) {
        case FTW_F:   // Regular File
            rv = secure_file_delete(fpath); // <-- Use secure delete
            break;
            
        case FTW_SL:  // Symlink
        case FTW_SLN: // Broken symlink
            rv = unlink(fpath); // <-- Symlinks are just unlinked
            break;
            
        case FTW_DP:  // Directory (post-order)
            rv = rmdir(fpath); // <-- Directories are just removed
            break;
            
        case FTW_D:   // Directory (pre-order)
            // Do nothing in pre-order
            rv = 0;
            break;
            
        default:
            rv = -1; // Error
            break;
    }
    
    if (rv != 0) {
        fprintf(stderr, "[Cloud] secure_recursive_delete_callback failed for: %s\n", fpath);
    }
    return rv;
}

int secure_recursive_delete(const char* path) {
    return nftw(path, secure_recursive_delete_callback, 64, FTW_DEPTH | FTW_PHYS);
}


int copy_file(const char* src, const char* dest) {
    int src_fd, dest_fd;
    char buf[8192];
    ssize_t n;
    struct stat st;

    if (stat(src, &st) != 0) return -1;
    if ((src_fd = open(src, O_RDONLY)) == -1) return -1;
    if ((dest_fd = open(dest, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode)) == -1) {
        close(src_fd);
        return -1;
    }

    while ((n = read(src_fd, buf, sizeof(buf))) > 0) {
        if (write(dest_fd, buf, n) != n) {
            close(src_fd);
            close(dest_fd);
            return -1;
        }
    }
    
    close(src_fd);
    close(dest_fd);
    return (n == 0) ? 0 : -1;
}

int recursive_copy(const char* src, const char* dest) {
    DIR* dir;
    struct dirent* entry;
    struct stat st;

    if (stat(src, &st) != 0) return -1;
    if (mkdir(dest, st.st_mode) != 0) return -1;
    if ((dir = opendir(src)) == NULL) return -1;

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char src_path[PATH_MAX];
        char dest_path[PATH_MAX];
        snprintf(src_path, sizeof(src_path), "%s/%s", src, entry->d_name);
        snprintf(dest_path, sizeof(dest_path), "%s/%s", dest, entry->d_name);

        if (stat(src_path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            if (recursive_copy(src_path, dest_path) != 0) {
                closedir(dir);
                return -1;
            }
        } else {
            if (copy_file(src_path, dest_path) != 0) {
                closedir(dir);
                return -1;
            }
        }
    }
    
    closedir(dir);
    return 0;
}

static void get_real_time_string(char* buf, size_t size) {
    time_t now = time(NULL);
    struct tm local_time;
    localtime_r(&now, &local_time);
    strftime(buf, size, "%Y-%m-%d %H:%M:%S", &local_time);
}

static void free_filter_list(FilterEntry* head) {
    FilterEntry* current = head;
    while (current) {
        FilterEntry* next = current->next;
        free(current);
        current = next;
    }
}

static int is_file_filtered(const char* filename, FilterEntry* filter_list_head) {
    if (!filter_list_head) {
        return 0; // No filters set
    }
    
    for (FilterEntry* f = filter_list_head; f; f = f->next) {
        size_t file_len = strlen(filename);
        size_t ext_len = strlen(f->extension);
        if (file_len > ext_len) {
            if (strcmp(filename + file_len - ext_len, f->extension) == 0) {
                return 1; // Match found
            }
        }
    }
    return 0; // No match
}

// Generates the contents.json file for a given node
void generate_node_contents_json(WatchedNode* node) {
    if (!node) return;

    char contents_file_path[PATH_MAX];
    snprintf(contents_file_path, sizeof(contents_file_path), "%s/.log/contents.json", node->path);

    ctz_json_value* root_array = ctz_json_new_array();
    if (!root_array) {
        fprintf(stderr, "[Cloud] Failed to create JSON array for node '%s'\n", node->name);
        return;
    }

    recursive_scan_dir(node->path, root_array);

    char* json_string = ctz_json_stringify(root_array, 1); // Pretty print
    if (json_string) {
        FILE* f = fopen(contents_file_path, "w");
        if (f) {
            fprintf(f, "%s", json_string);
            fclose(f);
        } else {
            fprintf(stderr, "[Cloud] Failed to write to %s\n", contents_file_path);
        }
        free(json_string);
    }
    
    ctz_json_free(root_array);
    printf("[Cloud] Re-indexed contents for node '%s'.\n", node->name);
}


char* read_file_content(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (length < 0) {
        fclose(f);
        return NULL;
    }

    char* buffer = malloc(length + 1);
    if (!buffer) {
        fclose(f);
        return NULL;
    }

    if (fread(buffer, 1, length, f) != (size_t)length) {
        free(buffer);
        fclose(f);
        return NULL;
    }

    buffer[length] = '\0';
    fclose(f);
    return buffer;
}


void update_file_cache(const char* path, const char* content) {
    pthread_mutex_lock(&file_cache_mutex);
    FileCache* current = file_cache_head;
    while (current) {
        if (strcmp(current->path, path) == 0) {
            // Found existing entry, update it
            free(current->content);
            current->content = content ? strdup(content) : NULL;
            current->last_processed_time = time(NULL);
            // If content is null, this effectively removes the cached data
            pthread_mutex_unlock(&file_cache_mutex);
            return;
        }
        current = current->next;
    }

    // Not found, add a new entry if content is not null
    if (content) {
        FileCache* new_entry = malloc(sizeof(FileCache));
        if (new_entry) {
            strncpy(new_entry->path, path, sizeof(new_entry->path) - 1);
            new_entry->path[sizeof(new_entry->path) - 1] = '\0';
            new_entry->content = strdup(content);
            new_entry->last_processed_time = time(NULL); // <-- ADD THIS LINE
            new_entry->next = file_cache_head;
            file_cache_head = new_entry;
        }
    }
    pthread_mutex_unlock(&file_cache_mutex);
}


void add_watches_recursively(WatchedNode* node, const char* base_path) {
    DIR* dir = opendir(base_path);
    if (!dir) {
        fprintf(stderr, "[Watcher] Could not open directory for watching: %s\n", base_path);
        return;
    }

    // Add a watch for the current directory itself
    int wd = inotify_add_watch(inotify_fd, base_path, IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO);
    if (wd == -1) {
        fprintf(stderr, "[Watcher] Failed to watch %s: %s\n", base_path, strerror(errno));
        closedir(dir);
        return;
    }

    // Add the new watch descriptor to our map
    WatchDescriptorMap* new_map_entry = malloc(sizeof(WatchDescriptorMap));
    if (new_map_entry) {
        new_map_entry->wd = wd;
        strncpy(new_map_entry->path, base_path, sizeof(new_map_entry->path) - 1);
        new_map_entry->parent_node = node;
        
        pthread_mutex_lock(&wd_map_mutex);
        new_map_entry->next = wd_map_head;
        wd_map_head = new_map_entry;
        pthread_mutex_unlock(&wd_map_mutex);
    } else {
        inotify_rm_watch(inotify_fd, wd); // Cleanup on malloc failure
    }
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 || strcmp(entry->d_name, ".log") == 0) continue;

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", base_path, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                // It's a directory, recurse into it
                add_watches_recursively(node, full_path);
            } else if (S_ISREG(st.st_mode)) {
                // It's a regular file, cache its initial content for diffing
                char* content = read_file_content(full_path);
                if (content) {
                    update_file_cache(full_path, content);
                    free(content);
                }
            }
        }
    }
    closedir(dir);
}



void remove_all_watches_for_node(WatchedNode* node) {
    // Remove all watch descriptors from inotify and our map
    pthread_mutex_lock(&wd_map_mutex);
    WatchDescriptorMap** pptr = &wd_map_head;
    while (*pptr) {
        WatchDescriptorMap* entry = *pptr;
        if (entry->parent_node == node) {
            inotify_rm_watch(inotify_fd, entry->wd);
            *pptr = entry->next; // Unlink from list
            free(entry);
        } else {
            pptr = &(*pptr)->next;
        }
    }
    pthread_mutex_unlock(&wd_map_mutex);

    // Clear this node's files from the content cache
    pthread_mutex_lock(&file_cache_mutex);
    FileCache** cache_pptr = &file_cache_head;
    while (*cache_pptr) {
        FileCache* entry = *cache_pptr;
        if (strncmp(entry->path, node->path, strlen(node->path)) == 0) {
            *cache_pptr = entry->next; // Unlink from list
            free(entry->content);
            free(entry);
        } else {
            cache_pptr = &(*cache_pptr)->next;
        }
    }
    pthread_mutex_unlock(&file_cache_mutex);
}


void handle_file_modification(WatchedNode* node, const char* full_path, const char* user) {

    const int DEBOUNCE_SECONDS = 2; // Cooldown window
    time_t now = time(NULL);

    pthread_mutex_lock(&file_cache_mutex);
    for (FileCache* current = file_cache_head; current; current = current->next) {
        if (strcmp(current->path, full_path) == 0) {
            if (now < current->last_processed_time + DEBOUNCE_SECONDS) {
                pthread_mutex_unlock(&file_cache_mutex);
                return; 
            }
            break; 
        }
    }
    pthread_mutex_unlock(&file_cache_mutex);

    char* new_content = read_file_content(full_path);
    if (!new_content) return; 

    char* old_content = NULL;
    
    pthread_mutex_lock(&file_cache_mutex);
    for (FileCache* current = file_cache_head; current; current = current->next) {
        if (strcmp(current->path, full_path) == 0) {
            old_content = current->content ? strdup(current->content) : NULL;
            break;
        }
    }
    pthread_mutex_unlock(&file_cache_mutex);

    const char* relative_path = full_path + strlen(node->path) + 1;
    if (!old_content) {
        add_event_to_node(node, EV_MODIFIED, relative_path, user, NULL);
        update_file_cache(full_path, new_content);
        free(new_content);
        return;
    }
    
    ctz_json_value* changes_obj = ctz_json_new_object();
    ctz_json_value* added_array = ctz_json_new_array();
    ctz_json_value* removed_array = ctz_json_new_array();
    ctz_json_value* moved_array = ctz_json_new_array();

    char* old_copy = strdup(old_content);
    char* new_copy = strdup(new_content);
    
    char** old_lines = NULL;
    char** new_lines = NULL;

    int old_count = split_lines(old_copy, &old_lines);
    int new_count = split_lines(new_copy, &new_lines);

    // 1. Allocate and fill the LCS dynamic programming matrix
    int** lcs_matrix = malloc((old_count + 1) * sizeof(int*));
    if (!lcs_matrix) { /* handle malloc failure */ }
    
    for (int i = 0; i <= old_count; i++) {
        lcs_matrix[i] = calloc((new_count + 1), sizeof(int));
        if (!lcs_matrix[i]) { /* handle malloc failure */ }
    }

    for (int i = 1; i <= old_count; i++) {
        for (int j = 1; j <= new_count; j++) {
            if (strcmp(old_lines[i - 1], new_lines[j - 1]) == 0) {
                lcs_matrix[i][j] = lcs_matrix[i - 1][j - 1] + 1;
            } else {
                int above = lcs_matrix[i - 1][j];
                int left = lcs_matrix[i][j - 1];
                lcs_matrix[i][j] = (above > left) ? above : left;
            }
        }
    }


    DiffChange* added_list_head = NULL;
    DiffChange* removed_list_head = NULL;
    int i = old_count;
    int j = new_count;
    while (i > 0 || j > 0) {
        if (i > 0 && j > 0 && strcmp(old_lines[i - 1], new_lines[j - 1]) == 0) {
            i--;
            j--;
        } else if (j > 0 && (i == 0 || lcs_matrix[i][j - 1] >= lcs_matrix[i - 1][j])) {
            // Added line (from new)
            DiffChange* change = malloc(sizeof(DiffChange));
            change->op = 'a';
            change->line_num = j;
            change->content = new_lines[j - 1];
            change->matched = 0; // --- NEW ---
            change->next = added_list_head; // --- MODIFIED ---
            added_list_head = change; // --- MODIFIED ---
            j--;
        } else if (i > 0 && (j == 0 || lcs_matrix[i][j - 1] < lcs_matrix[i - 1][j])) {
            // Removed line (from old)
            DiffChange* change = malloc(sizeof(DiffChange));
            change->op = 'd';
            change->line_num = i;
            change->content = old_lines[i - 1];
            change->matched = 0; // --- NEW ---
            change->next = removed_list_head; // --- MODIFIED ---
            removed_list_head = change; // --- MODIFIED ---
            i--;
        }
    }

    MovedChange* moved_list_head = NULL;

    // Iterate over all removed lines
    for (DiffChange* r_node = removed_list_head; r_node; r_node = r_node->next) {
        if (r_node->matched) continue; // Already matched as part of a move

        // Try to find a matching added line
        for (DiffChange* a_node = added_list_head; a_node; a_node = a_node->next) {
            if (a_node->matched) continue; // Already matched

            if (strcmp(r_node->content, a_node->content) == 0) {
                // We found a move!
                MovedChange* move = malloc(sizeof(MovedChange));
                move->from_line = r_node->line_num;
                move->to_line = a_node->line_num;
                move->content = r_node->content; // Content is the same
                move->next = moved_list_head;
                moved_list_head = move;

                // Mark both nodes as matched so they aren't processed again
                r_node->matched = 1;
                a_node->matched = 1;

                break; // Stop inner loop; this added line is now consumed
            }
        }
    }



    // 3. Convert the diff linked lists into the final JSON arrays
    
    MovedChange* current_move = moved_list_head;
    while (current_move) {
        ctz_json_value* change_obj = ctz_json_new_object();
        ctz_json_object_set_value(change_obj, "from", ctz_json_new_number(current_move->from_line));
        ctz_json_object_set_value(change_obj, "to", ctz_json_new_number(current_move->to_line));
        ctz_json_object_set_value(change_obj, "content", ctz_json_new_string(current_move->content));
        ctz_json_array_push_value(moved_array, change_obj);
        
        MovedChange* next = current_move->next;
        free(current_move);
        current_move = next;
    }


    DiffChange* current_change = added_list_head;
    while (current_change) {
        if (!current_change->matched) { // Check the flag
            ctz_json_value* change_obj = ctz_json_new_object();
            ctz_json_object_set_value(change_obj, "line", ctz_json_new_number(current_change->line_num));
            ctz_json_object_set_value(change_obj, "content", ctz_json_new_string(current_change->content));
            ctz_json_array_push_value(added_array, change_obj);
        }
        DiffChange* next = current_change->next;
        free(current_change);
        current_change = next;
    }


    current_change = removed_list_head;
    while (current_change) {
        if (!current_change->matched) { // Check the flag
            ctz_json_value* change_obj = ctz_json_new_object();
            ctz_json_object_set_value(change_obj, "line", ctz_json_new_number(current_change->line_num));
            ctz_json_object_set_value(change_obj, "content", ctz_json_new_string(current_change->content));
            ctz_json_array_push_value(removed_array, change_obj);
        }
        DiffChange* next = current_change->next;
        free(current_change);
        current_change = next;
    }


    // 4. Clean up all allocations
    free_lcs_matrix(lcs_matrix, old_count + 1);
    free(old_lines);
    free(new_lines);
    free(old_copy);
    free(new_copy);



    char* details_json_string = NULL;
    
    if (ctz_json_get_array_size(moved_array) > 0) ctz_json_object_set_value(changes_obj, "moved", moved_array);
    else ctz_json_free(moved_array);
    
    if (ctz_json_get_array_size(added_array) > 0) ctz_json_object_set_value(changes_obj, "added", added_array);
    else ctz_json_free(added_array);
    
    if (ctz_json_get_array_size(removed_array) > 0) ctz_json_object_set_value(changes_obj, "removed", removed_array);
    else ctz_json_free(removed_array);

    if (ctz_json_get_object_size(changes_obj) > 0) {
        details_json_string = ctz_json_stringify(changes_obj, 0);
    }
    
    add_event_to_node(node, EV_MODIFIED, relative_path, user, details_json_string);
    
    // Cleanup
    if (details_json_string) free(details_json_string);
    ctz_json_free(changes_obj);
    free(old_content);
    
    update_file_cache(full_path, new_content);
    free(new_content);
}

void recursive_scan_dir(const char* base_path, ctz_json_value* json_array) {
    struct dirent* de;
    DIR* dr = opendir(base_path);

    if (dr == NULL) {
        // fprintf(stderr, "[Cloud] Could not open directory %s\n", base_path);
        return;
    }

    while ((de = readdir(dr)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0 || strcmp(de->d_name, ".log") == 0) {
            continue;
        }

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", base_path, de->d_name);
        
        // Create a JSON object for this item
        ctz_json_value* item_obj = ctz_json_new_object();
        ctz_json_object_set_value(item_obj, "name", ctz_json_new_string(de->d_name));
        ctz_json_object_set_value(item_obj, "path", ctz_json_new_string(full_path));
        ctz_json_array_push_value(json_array, item_obj);

        struct stat st;
        if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            recursive_scan_dir(full_path, json_array);
        }
    }

    closedir(dr);
}

// Smart path finder for pin.json
int get_pin_json_path(char* buffer, size_t size) {
    char exe_dir[PATH_MAX];
    if (get_executable_dir(exe_dir, sizeof(exe_dir)) != 0) {
        return -1;
    }

    char data_dir[PATH_MAX];
    snprintf(data_dir, sizeof(data_dir), "%s/data", exe_dir);
    mkdir(data_dir, 0755); // Create data directory, doesn't matter if it exists

    snprintf(buffer, size, "%s/pin.json", data_dir);
    return 0;
}

//sputnik

void write_to_handle(cortez_write_handle_t* h, const void* data, size_t size);

// Simple in-memory word index (linked list)
typedef struct WordOccurrence {
    const char* sentence_start;
    struct WordOccurrence* next;
} WordOccurrence;

typedef struct WordIndex {
    char word[MAX_WORD_LEN];
    int count;
    WordOccurrence* occurrences; // Head of linked list of occurrences
    struct WordIndex* next;
} WordIndex;
static volatile int keep_running = 1;

static WordIndex* index_head = NULL;

void int_handler(int dummy) {
    (void)dummy;
    keep_running = 0;
}

int get_executable_dir(char* buffer, size_t size) {
    char path_buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", path_buf, sizeof(path_buf) - 1);

    if (len == -1) {
        perror("readlink failed");
        return -1;
    }

    path_buf[len] = '\0';

    char *last_slash = strrchr(path_buf, '/');
    if (last_slash == NULL) {
        fprintf(stderr, "Could not find slash in executable path\n");
        return -1;
    }

    *last_slash = '\0';

    strncpy(buffer, path_buf, size - 1);
    buffer[size - 1] = '\0';

    return 0;
}

void free_index() {
    WordIndex* current = index_head;
    while (current != NULL) {
        WordIndex* next = current->next;
        
        WordOccurrence* occ = current->occurrences;
        while (occ != NULL) {
            WordOccurrence* next_occ = occ->next;
            free(occ);
            occ = next_occ;
        }
        
        free(current);
        current = next;
    }
    index_head = NULL;
}

void build_index() {
    free_index();
    if (!file_content) return;

    // Use a copy for tokenization that preserves original content
    char* content_for_tokens = strdup(file_content);
    if (!content_for_tokens) return;

    const char* delimiters = " \t\n\r,.;:!?\"()[]{}";
    char* token = strtok(content_for_tokens, delimiters);

    while (token != NULL) {
        char normalized_token[MAX_WORD_LEN];
        strncpy(normalized_token, token, MAX_WORD_LEN - 1);
        normalized_token[MAX_WORD_LEN - 1] = '\0';
        for (char* p = normalized_token; *p; ++p) *p = tolower((unsigned char)*p);

        // Find where this token is in the *original* file_content
        ptrdiff_t offset = token - content_for_tokens;
        const char* original_pos = file_content + offset;

        // Find the start of the sentence for this token
        const char* sentence_start = original_pos;
        while (sentence_start > file_content) {
            if (*(sentence_start - 1) == '.' || *(sentence_start - 1) == '!' || *(sentence_start - 1) == '?') {
                break;
            }
            sentence_start--;
        }
        // Trim leading whitespace from the sentence start
        while (*sentence_start && isspace((unsigned char)*sentence_start)) {
            sentence_start++;
        }

        WordIndex* entry = index_head;
        while (entry != NULL) {
            if (strcmp(entry->word, normalized_token) == 0) {
                entry->count++;
                break;
            }
            entry = entry->next;
        }

        if (entry == NULL) { // New word
            entry = malloc(sizeof(WordIndex));
            if (entry) {
                strcpy(entry->word, normalized_token);
                entry->count = 1;
                entry->occurrences = NULL; // Initialize occurrences
                entry->next = index_head;
                index_head = entry;
            }
        }

        if (entry) { // Add the occurrence
            WordOccurrence* new_occ = malloc(sizeof(WordOccurrence));
            if(new_occ) {
                new_occ->sentence_start = sentence_start;
                new_occ->next = entry->occurrences;
                entry->occurrences = new_occ;
            }
        }

        token = strtok(NULL, delimiters);
    }
    free(content_for_tokens);
    printf("[Cloud] File indexed successfully.\n");
}

// Sends a response back to the query daemon, wrapping it with the original request_id
void send_wrapped_response_zc(cortez_mesh_t* mesh, pid_t query_daemon_pid, uint16_t msg_type, uint64_t request_id, const void* response_payload, uint32_t response_payload_size) {
    uint32_t total_payload_size = sizeof(request_id) + response_payload_size;
    int sent_ok = 0;

    for (int i = 0; i < 50; i++) {
        cortez_write_handle_t* h = cortez_mesh_begin_send_zc(mesh, query_daemon_pid, total_payload_size);
        if (!h) {
            // Peer is not yet visible on the mesh, wait and retry.
            usleep(200000); // 100ms
            continue;
        }

        // Allocate the temporary buffer on the heap to prevent stack overflow.
        char* temp_buffer = malloc(total_payload_size);
        if (!temp_buffer) {
            fprintf(stderr, "[Cloud] Out of memory when creating response buffer.\n");
            cortez_mesh_abort_send_zc(h); // Abort the transaction
            break; // Exit retry loop on critical error
        }

        // Assemble the full payload in the heap buffer
        memcpy(temp_buffer, &request_id, sizeof(request_id));
        memcpy(temp_buffer + sizeof(request_id), response_payload, response_payload_size);

        // This helper function safely copies from the temp buffer into the shared memory handle.
        write_to_handle(h, temp_buffer, total_payload_size);

        // Free the heap buffer now that the data has been copied.
        free(temp_buffer);

        cortez_mesh_commit_send_zc(h, msg_type);
        sent_ok = 1;
        break; // Success! Exit the loop.
    }

    if (!sent_ok) {
        fprintf(stderr, "[Cloud] Failed to send response for request #%lu to query daemon %d after retries.\n", request_id, query_daemon_pid);
    }
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

static void process_stale_moves_locked(time_t now) {
    const int MOVE_TIMEOUT_SECONDS = 2; // How long to wait for a matching event
    PendingMove** pptr = &pending_move_head;
    while (*pptr) {
        PendingMove* entry = *pptr;
        // Check if the move event has expired
        if (now > entry->timestamp + MOVE_TIMEOUT_SECONDS) { 
            // Move expired, log it as a simple deletion from its source
            add_event_to_node(entry->from_node, EV_DELETED, entry->from_path, entry->user, NULL);
            
            // Unlink from the list and free
            *pptr = entry->next; 
            free(entry);
        } else {
            // Not expired, move to the next item
            pptr = &(*pptr)->next;
        }
    }
}

void* watcher_thread_func(void* arg) {
    (void)arg;
    // Increased buffer size to handle multiple events at once
    char buffer[4 * (sizeof(struct inotify_event) + NAME_MAX + 1)];

    while (keep_running) {
        ssize_t len = read(inotify_fd, buffer, sizeof(buffer));
        if (len <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(250000); // 250ms sleep
                continue; 
            }
            

            if (errno == EINTR) {
                continue;
            }


            if (keep_running) {
                 perror("[Watcher] read error");
            }

            usleep(100000); 
            continue;
        }

        time_t now = time(NULL);
        pthread_mutex_lock(&pending_move_mutex);
        process_stale_moves_locked(now);
        pthread_mutex_unlock(&pending_move_mutex);

        ssize_t i = 0;
        while (i < len) {
            struct inotify_event* event = (struct inotify_event*)&buffer[i];
            
            // Find the path and node associated with this event's watch descriptor
            pthread_mutex_lock(&wd_map_mutex);
            WatchDescriptorMap* map_entry = NULL;
            for (WatchDescriptorMap* current = wd_map_head; current; current = current->next) {
                if (current->wd == event->wd) {
                    map_entry = current;
                    break;
                }
            }
            pthread_mutex_unlock(&wd_map_mutex);

            if (map_entry) {
                // We only process events with a name.
                if (event->len > 0) {
                    char event_full_path[PATH_MAX];
                    snprintf(event_full_path, sizeof(event_full_path), "%s/%s", map_entry->path, event->name);
                    
                    // Ignore events from our own log directory
                    if (strstr(event_full_path, "/.log")) {
                        i += sizeof(struct inotify_event) + event->len;
                        continue;
                    }
                    
                    const char* relative_path = event_full_path + strlen(map_entry->parent_node->path) + 1;

                    uid_t event_uid = (uid_t)-1;
                    if (event->mask & (IN_CREATE | IN_MOVED_TO | IN_MODIFY)) {
                        // File exists, stat it
                        event_uid = get_uid_for_path(event_full_path);
                    } else if (event->mask & (IN_DELETE | IN_MOVED_FROM)) {
                        // File is gone, stat the parent directory
                        event_uid = get_uid_for_path(map_entry->path);
                    }

                    char event_user[64] = "unknown";
                    if (event_uid != (uid_t)-1) {
                        get_username_from_uid(event_uid, event_user, sizeof(event_user));
                    }

                    if (event->mask & IN_MOVED_FROM) {
                        PendingMove* new_move = malloc(sizeof(PendingMove));
                        if (new_move) {
                            new_move->cookie = event->cookie;
                            strncpy(new_move->from_path, relative_path, sizeof(new_move->from_path) - 1);
                            new_move->from_node = map_entry->parent_node;
                            new_move->timestamp = now;
                            strncpy(new_move->user, event_user, sizeof(new_move->user) - 1);
    
                            pthread_mutex_lock(&pending_move_mutex);
                            new_move->next = pending_move_head;
                            pending_move_head = new_move;
                            pthread_mutex_unlock(&pending_move_mutex);
                        }
                        
                        // We're done, don't process it as DELETE yet
                        i += sizeof(struct inotify_event) + event->len;
                        continue; 
                    }

                    if (event->mask & IN_MOVED_TO) {
                        int matched_move = 0;
                        pthread_mutex_lock(&pending_move_mutex);
                        
                        PendingMove** pptr = &pending_move_head;
                        while (*pptr) {
                            PendingMove* entry = *pptr;
                            if (entry->cookie == event->cookie) {
                                // We found the matching IN_MOVED_FROM! This is a rename/move.
                                
                                // Create JSON details: {"from": "old_path", "to": "new_path"}
                                ctz_json_value* details_obj = ctz_json_new_object();
                                ctz_json_object_set_value(details_obj, "from", ctz_json_new_string(entry->from_path));
                                ctz_json_object_set_value(details_obj, "to", ctz_json_new_string(relative_path));
                                char* details_str = ctz_json_stringify(details_obj, 0);
                                
                                // Log it as ONE event
                                add_event_to_node(map_entry->parent_node, EV_MOVED, relative_path, event_user, details_str);
                                
                                if (details_str) free(details_str);
                                ctz_json_free(details_obj);
    
                                // If it's a new directory, add watches for it
                                if (event->mask & IN_ISDIR) {
                                    add_watches_recursively(map_entry->parent_node, event_full_path);
                                }
    
                                // Unlink and free the pending move
                                *pptr = entry->next;
                                free(entry);
    
                                matched_move = 1;
                                break; // Found and processed
                            }
                            pptr = &(*pptr)->next;
                        }
                        pthread_mutex_unlock(&pending_move_mutex);
    
                        if (matched_move) {
                            // We've processed this move, skip all other logic
                            i += sizeof(struct inotify_event) + event->len;
                            continue;
                        }
                        
                    }

                    if (event->mask & (IN_CREATE | IN_MOVED_TO | IN_MODIFY)) {
                        if (is_file_filtered(event->name, map_entry->parent_node->filter_list_head)) {
                                // File is filtered. Delete it and log as "Deleted".
                                unlink(event_full_path);
                                add_event_to_node(map_entry->parent_node, EV_DELETED, relative_path, event_user, "{\"reason\":\"Filtered\"}");
                                update_file_cache(event_full_path, NULL); // Remove from cache
                                
                                // Skip all other processing for this event
                                i += sizeof(struct inotify_event) + event->len;
                                continue;
                            }
                        }


                    if (event->mask & IN_ISDIR) {
                        // Event on a directory
                        if (event->mask & IN_CREATE) { // MOVED_TO is handled above
                            add_event_to_node(map_entry->parent_node, EV_CREATED, relative_path, event_user, NULL);
                            add_watches_recursively(map_entry->parent_node, event_full_path);
                        } else if (event->mask & IN_DELETE) { // MOVED_FROM is handled above
                            add_event_to_node(map_entry->parent_node, EV_DELETED, relative_path, event_user, NULL);
                        }
                    } else {
                        // Event on a file
                        if (event->mask & IN_CREATE) { // MOVED_TO is separate now
                            add_event_to_node(map_entry->parent_node, EV_CREATED, relative_path, event_user, NULL);
                            char* content = read_file_content(event_full_path);
                            if (content) {
                                update_file_cache(event_full_path, content);
                                free(content);
                            }
                        }
                        if (event->mask & IN_DELETE) { // MOVED_FROM is handled above
                            add_event_to_node(map_entry->parent_node, EV_DELETED, relative_path, event_user, NULL);
                            update_file_cache(event_full_path, NULL); // Remove from cache
                        }
                        if (event->mask & IN_MOVED_TO) { // Fall-through case (move *into* area)
                            add_event_to_node(map_entry->parent_node, EV_CREATED, relative_path, event_user, NULL);
                            char* content = read_file_content(event_full_path);
                            if (content) {
                                update_file_cache(event_full_path, content);
                                free(content);
                            }
                        }
                        if (event->mask & IN_MODIFY) {
                            handle_file_modification(map_entry->parent_node, event_full_path, event_user);
                        }
                    }
                }
            }

            // Handle watch being removed (e.g., directory deleted)
            if (event->mask & IN_IGNORED) {
                pthread_mutex_lock(&wd_map_mutex);
                WatchDescriptorMap** pptr = &wd_map_head;
                while (*pptr) {
                    WatchDescriptorMap* entry = *pptr;
                    if (entry->wd == event->wd) {
                        *pptr = entry->next; // Unlink
                        free(entry);
                        break; // Found and removed
                    }
                    pptr = &(*pptr)->next;
                }
                pthread_mutex_unlock(&wd_map_mutex);
            }
            i += sizeof(struct inotify_event) + event->len;
        }
    }
    return NULL;
}

void free_node_history(WatchedNode* node) {
    NodeEvent* current = node->history_head;
    while (current) {
        NodeEvent* next = current->next;
        free(current);
        current = next;
    }
    node->history_head = NULL;
    

    free_filter_list(node->filter_list_head);
    node->filter_list_head = NULL;
}

void add_event_to_node(WatchedNode* node, EventType type, const char* name, const char* user, const char* details_json_obj) {

    NodeEvent* new_event = malloc(sizeof(NodeEvent));
    if (!new_event) return;
    new_event->type = type;
    new_event->timestamp = time(NULL);
    strncpy(new_event->name, name, sizeof(new_event->name) - 1);
    new_event->name[sizeof(new_event->name) - 1] = '\0';
    
    pthread_mutex_lock(&node_list_mutex);
    new_event->next = node->history_head;
    node->history_head = new_event;
    pthread_mutex_unlock(&node_list_mutex);


    char log_file_path[PATH_MAX];
    snprintf(log_file_path, sizeof(log_file_path), "%s/.log/history.json", node->path);

    // 1. Load the existing history file, or create a new array if it doesn't exist.
    ctz_json_value* history_array = ctz_json_load_file(log_file_path, NULL, 0);
    if (!history_array || ctz_json_get_type(history_array) != CTZ_JSON_ARRAY) {
        if (history_array) ctz_json_free(history_array); // Free if loading resulted in a non-array
        history_array = ctz_json_new_array();
        if (!history_array) {
            fprintf(stderr, "[Cloud] Failed to create new JSON history array.\n");
            return;
        }
    }

    // 2. Create the new JSON object for this event.
    ctz_json_value* event_obj = ctz_json_new_object();
    if (!event_obj) {
        ctz_json_free(history_array);
        return;
    }
    
    const char* type_str = (type == EV_CREATED) ? "Created" :
                           (type == EV_DELETED ? "Deleted" :
                           (type == EV_MODIFIED ? "Modified" : "Moved"));
    ctz_json_object_set_value(event_obj, "event", ctz_json_new_string(type_str));
    ctz_json_object_set_value(event_obj, "name", ctz_json_new_string(name));
    ctz_json_object_set_value(event_obj, "user", ctz_json_new_string(user ? user : "unknown"));

    if (node->time_format == TIME_REAL) {
        char time_buf[64];
        get_real_time_string(time_buf, sizeof(time_buf));
        ctz_json_object_set_value(event_obj, "timestamp", ctz_json_new_string(time_buf));
    } else {
        ctz_json_object_set_value(event_obj, "timestamp", ctz_json_new_number((double)new_event->timestamp));
    }

    // 3. If details are provided, parse them as a JSON object and add to the event.
    if (details_json_obj && strlen(details_json_obj) > 2) {
        char error_buf[128];
        ctz_json_value* changes_obj = ctz_json_parse(details_json_obj, error_buf, sizeof(error_buf));
        if (changes_obj) {
            ctz_json_object_set_value(event_obj, "changes", changes_obj);
        } else {
             fprintf(stderr, "[Cloud] Warning: Failed to parse event details JSON: %s\n", error_buf);
        }
    }

    // 4. Add the new event to the main history array.
    ctz_json_array_push_value(history_array, event_obj);

    // 5. Stringify the entire updated array back to a formatted string.
    char* json_output = ctz_json_stringify(history_array, 1); // Pretty print enabled

    // 6. Overwrite the log file with the new, correct content.
    if (json_output) {
        FILE* f = fopen(log_file_path, "w");
        if (f) {
            fprintf(f, "%s", json_output);
            fclose(f);
        } else {
            fprintf(stderr, "[Cloud] CRITICAL: Failed to write to log file %s\n", log_file_path);
        }
        free(json_output); // Free the string created by stringify
    }

    // 7. Clean up the ctz_json value structure.
    ctz_json_free(history_array);
    
    // Re-index the node's file list since something changed
    generate_node_contents_json(node);
}


void initialize_node_log_file(const char* node_path) {
    char log_dir_path[PATH_MAX];
    char log_file_path[PATH_MAX];
    snprintf(log_dir_path, sizeof(log_dir_path), "%s/.log", node_path);
    snprintf(log_file_path, sizeof(log_file_path), "%s/history.json", log_dir_path);

    mkdir(log_dir_path, 0755);

    // Create and initialize the file only if it doesn't exist
    FILE* f = fopen(log_file_path, "r");
    if (f) {
        fclose(f);
        // return; // We now continue to create contents.json
    } else {
        f = fopen(log_file_path, "w");
        if (f) {
            fprintf(f, "[\n\n]\n");
            fclose(f);
        }
    }
    
    char contents_file_path[PATH_MAX];
    snprintf(contents_file_path, sizeof(contents_file_path), "%s/contents.json", log_dir_path);
    f = fopen(contents_file_path, "r");
    if(f) {
        fclose(f);
    } else {
        f = fopen(contents_file_path, "w");
        if (f) {
            fprintf(f, "[]\n");
            fclose(f);
        }
    }
}

static int compare_events_by_timestamp(const void* a, const void* b) {
    const ctz_json_value* event_a = *(const ctz_json_value**)a;
    const ctz_json_value* event_b = *(const ctz_json_value**)b;

    ctz_json_value* ts_a_val = ctz_json_find_object_value(event_a, "timestamp");
    ctz_json_value* ts_b_val = ctz_json_find_object_value(event_b, "timestamp");

    // Handle missing timestamps (sort them to the end)
    if (!ts_a_val && !ts_b_val) return 0;
    if (!ts_a_val) return 1;  // a is "greater" (put at end)
    if (!ts_b_val) return -1; // b is "greater" (put at end)

    // Get types
    ctz_json_type type_a = ctz_json_get_type(ts_a_val);
    ctz_json_type type_b = ctz_json_get_type(ts_b_val);

    // Both are numbers (Unix time)
    if (type_a == CTZ_JSON_NUMBER && type_b == CTZ_JSON_NUMBER) {
        double ts_a = ctz_json_get_number(ts_a_val);
        double ts_b = ctz_json_get_number(ts_b_val);
        if (ts_a < ts_b) return -1;
        if (ts_a > ts_b) return 1;
        return 0;
    }
    
    // Both are strings (Real time)
    if (type_a == CTZ_JSON_STRING && type_b == CTZ_JSON_STRING) {
        const char* ts_a_str = ctz_json_get_string(ts_a_val);
        const char* ts_b_str = ctz_json_get_string(ts_b_val);
        return strcmp(ts_a_str, ts_b_str); // Chronological string sort
    }


    if (type_a == CTZ_JSON_NUMBER) return -1;
    
    return 1;
}


void send_local_node_list_to_signal(cortez_mesh_t* mesh) {
    if (g_signal_daemon_pid == 0 || !mesh) return;

    ctz_json_value* root_array = ctz_json_new_array();
    if (!root_array) return;

    pthread_mutex_lock(&node_list_mutex);
    for (WatchedNode* n = watched_nodes_head; n; n = n->next) {
        ctz_json_value* node_obj = ctz_json_new_object();
        ctz_json_object_set_value(node_obj, "name", ctz_json_new_string(n->name));
        ctz_json_object_set_value(node_obj, "desc", ctz_json_new_string(n->desc));
        ctz_json_object_set_value(node_obj, "tag", ctz_json_new_string(n->tag));
        ctz_json_array_push_value(root_array, node_obj);
    }
    pthread_mutex_unlock(&node_list_mutex);

    char* json_body = ctz_json_stringify(root_array, 0);
    ctz_json_free(root_array);
    
    if (json_body) {
        printf("[Cloud] Sending updated node list to signal daemon.\n");
        // We use request_id 0, as this isn't a reply
        write_to_handle_and_commit(mesh, g_signal_daemon_pid, MSG_SIG_CACHE_NODE_LIST, json_body, strlen(json_body) + 1);
        free(json_body);
    }
}

// --- COMPLETE: Function to merge and sort two history.json arrays ---
ctz_json_value* merge_history_arrays(ctz_json_value* local_arr, ctz_json_value* remote_arr) {
    if (!remote_arr || ctz_json_get_type(remote_arr) != CTZ_JSON_ARRAY) {
        return local_arr ? ctz_json_duplicate(local_arr, 1) : ctz_json_new_array();
    }
    if (!local_arr || ctz_json_get_type(local_arr) != CTZ_JSON_ARRAY) {
        return ctz_json_duplicate(remote_arr, 1);
    }

    size_t local_size = ctz_json_get_array_size(local_arr);
    ctz_json_value* merged_arr = ctz_json_duplicate(local_arr, 1); // Start with local
    
    // Add remote events only if they don't already exist
    for (size_t i = 0; i < ctz_json_get_array_size(remote_arr); i++) {
        ctz_json_value* remote_event = ctz_json_get_array_element(remote_arr, i);
        ctz_json_value* remote_ts_val = ctz_json_find_object_value(remote_event, "timestamp");
        ctz_json_value* remote_name_val = ctz_json_find_object_value(remote_event, "name");
        
        if (!remote_ts_val || !remote_name_val) continue;

        int duplicate = 0;
        for (size_t j = 0; j < local_size; j++) { // Only check against original local array
            ctz_json_value* local_event = ctz_json_get_array_element(local_arr, j);
            ctz_json_value* local_ts_val = ctz_json_find_object_value(local_event, "timestamp");
            ctz_json_value* local_name_val = ctz_json_find_object_value(local_event, "name");

            if (!local_ts_val || !local_name_val) continue;

            if (ctz_json_compare(remote_ts_val, local_ts_val) == 0 &&
                ctz_json_compare(remote_name_val, local_name_val) == 0) {
                duplicate = 1;
                break;
            }
        }

        if (!duplicate) {
            ctz_json_array_push_value(merged_arr, ctz_json_duplicate(remote_event, 1));
        }
    }
    
    // --- Sort the merged array by timestamp ---
    size_t merged_size = ctz_json_get_array_size(merged_arr);
    if (merged_size > 1) {
        // 1. Create a temporary array of pointers
        ctz_json_value** sort_array = malloc(merged_size * sizeof(ctz_json_value*));
        if (!sort_array) return merged_arr; // Failed to alloc, return unsorted

        for (size_t i = 0; i < merged_size; i++) {
            sort_array[i] = ctz_json_get_array_element(merged_arr, i);
        }

        // 2. Sort the temporary array using our helper
        qsort(sort_array, merged_size, sizeof(ctz_json_value*), compare_events_by_timestamp);

        // 3. Create a new, sorted JSON array
        ctz_json_value* sorted_arr = ctz_json_new_array();
        if (!sorted_arr) {
            free(sort_array);
            return merged_arr; // Failed to alloc, return unsorted
        }

        for (size_t i = 0; i < merged_size; i++) {

            ctz_json_array_push_value(sorted_arr, ctz_json_duplicate(sort_array[i], 1));
        }
        
        // 4. Clean up
        free(sort_array);
        ctz_json_free(merged_arr);
        return sorted_arr;
    }
    
    return merged_arr; 
}

void handle_incoming_sync_data(const sig_sync_data_t* req) {
    printf("[Cloud] Received incoming sync payload for node '%s' from unit '%s'\n", 
           req->target_node, req->source_unit);
           
    WatchedNode* node = find_node_by_name_locked(req->target_node);
    if (!node) {
        fprintf(stderr, "[Cloud] Error: Cannot apply sync, node '%s' not found.\n", req->target_node);
        return;
    }
    
    char error_buf[256];
    ctz_json_value* payload_obj = ctz_json_parse(req->sync_payload_json, error_buf, sizeof(error_buf));
    if (!payload_obj) {
        fprintf(stderr, "[Cloud] Error: Failed to parse incoming payload: %s\n", error_buf);
        return;
    }
    
    ctz_json_value* remote_history = ctz_json_find_object_value(payload_obj, "history");
    ctz_json_value* files_obj = ctz_json_find_object_value(payload_obj, "files");

    if (!remote_history || !files_obj || ctz_json_get_type(remote_history) != CTZ_JSON_ARRAY || ctz_json_get_type(files_obj) != CTZ_JSON_OBJECT) {
        fprintf(stderr, "[Cloud] Error: Incoming payload is malformed (missing 'history' or 'files').\n");
        ctz_json_free(payload_obj);
        return;
    }

    char local_history_path[PATH_MAX];
    snprintf(local_history_path, sizeof(local_history_path), "%s/.log/history.json", node->path);
    ctz_json_value* local_history = ctz_json_load_file(local_history_path, NULL, 0);

    // --- 1. Find ONLY the new events to apply ---
    ctz_json_value* events_to_apply = ctz_json_new_array();
    if (!events_to_apply) {
        ctz_json_free(payload_obj);
        if (local_history) ctz_json_free(local_history);
        return;
    }

    size_t local_size = (local_history && ctz_json_get_type(local_history) == CTZ_JSON_ARRAY) ? ctz_json_get_array_size(local_history) : 0;
    
    for (size_t i = 0; i < ctz_json_get_array_size(remote_history); i++) {
        ctz_json_value* remote_event = ctz_json_get_array_element(remote_history, i);
        
        int duplicate = 0;
        for (size_t j = 0; j < local_size; j++) {
            ctz_json_value* local_event = ctz_json_get_array_element(local_history, j);
            // Use the new full-object comparison function
            if (ctz_json_compare(remote_event, local_event) == 0) {
                duplicate = 1;
                break;
            }
        }

        if (!duplicate) {
            ctz_json_array_push_value(events_to_apply, ctz_json_duplicate(remote_event, 1));
        }
    }

    // --- 2. Apply the new events to the filesystem ---
    printf("[Cloud] Found %zu new remote events to apply.\n", ctz_json_get_array_size(events_to_apply));
    for (size_t i = 0; i < ctz_json_get_array_size(events_to_apply); i++) {
        ctz_json_value* event = ctz_json_get_array_element(events_to_apply, i);
        const char* event_str = ctz_json_get_string(ctz_json_find_object_value(event, "event"));
        const char* name_str = ctz_json_get_string(ctz_json_find_object_value(event, "name"));

        if (!event_str || !name_str) continue;
        
        char full_path[PATH_MAX];
        if (get_full_node_path(node, name_str, full_path, sizeof(full_path)) != 0) {
            fprintf(stderr, "  Warning: Skipping event for insecure path: %s\n", name_str);
            continue;
        }
        
        // --- APPLY CREATED / MODIFIED ---
        if (strcmp(event_str, "Created") == 0 || strcmp(event_str, "Modified") == 0) {
            ctz_json_value* file_b64_val = ctz_json_find_object_value(files_obj, name_str);
            if (!file_b64_val) {
                fprintf(stderr, "  Error: Skipping [%s] for %s. File content was not in payload.\n", event_str, name_str);
                continue;
            }
            
            const char* b64_str = ctz_json_get_string(file_b64_val);
            size_t b64_len = strlen(b64_str);
            size_t file_size;
            unsigned char* file_content = base64_decode(b64_str, b64_len, &file_size);
            
            if (!file_content) {
                fprintf(stderr, "  Error: Failed to decode Base64 for %s.\n", name_str);
                continue;
            }
            
            printf("  Applying [%s]: %s (%zu bytes)\n", event_str, name_str, file_size);
            
            // Ensure directory exists
            char* dir_copy = strdup(full_path);
            char* parent_dir = dirname(dir_copy);
            if (parent_dir) {
                char cmd[PATH_MAX + 10];
                snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", parent_dir);
                system(cmd);
            }
            free(dir_copy);

            FILE* f = fopen(full_path, "wb");
            if (f) {
                fwrite(file_content, 1, file_size, f);
                fclose(f);
            } else {
                fprintf(stderr, "  Error: Failed to write file to %s\n", full_path);
            }
            free(file_content);

        // --- APPLY DELETED ---
        } else if (strcmp(event_str, "Deleted") == 0) {
            printf("  Applying [Delete]: %s\n", name_str);
            secure_recursive_delete(full_path); // Use your secure delete
        
        // --- APPLY MOVED ---
        } else if (strcmp(event_str, "Moved") == 0) {
            ctz_json_value* changes = ctz_json_find_object_value(event, "changes");
            if (changes) {
                const char* from_str = ctz_json_get_string(ctz_json_find_object_value(changes, "from"));
                const char* to_str = ctz_json_get_string(ctz_json_find_object_value(changes, "to"));
                if (from_str && to_str) {
                    char from_full_path[PATH_MAX];
                    char to_full_path[PATH_MAX];
                    if (get_full_node_path(node, from_str, from_full_path, sizeof(from_full_path)) == 0 &&
                        get_full_node_path(node, to_str, to_full_path, sizeof(to_full_path)) == 0) {
                        
                        printf("  Applying [Move]: %s -> %s\n", from_str, to_str);
                        rename(from_full_path, to_full_path);
                    }
                }
            }
        }
    }
    ctz_json_free(events_to_apply);

    // --- 3. Save the newly merged and sorted history file ---
    ctz_json_value* merged_history = merge_history_arrays(local_history, remote_history);
    
    char* json_output = ctz_json_stringify(merged_history, 1); // Pretty-print
    if (json_output) {
        FILE* f = fopen(local_history_path, "w");
        if (f) {
            fprintf(f, "%s", json_output);
            fclose(f);
            printf("[Cloud] Successfully merged and sorted remote history into '%s'.\n", node->name);
        } else {
            fprintf(stderr, "[Cloud] CRITICAL: Failed to write merged history file: %s\n", local_history_path);
        }
        free(json_output);
    }
    
    // --- 4. Clean up ---
    if (local_history) ctz_json_free(local_history);
    // remote_history is part of payload_obj
    if (merged_history) ctz_json_free(merged_history);
    ctz_json_free(payload_obj); // This frees remote_history and files_obj
    
    // --- 5. Regenerate contents.json ---
    generate_node_contents_json(node);
}

void load_nodes() {
    FILE* f = fopen(config_file_path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buffer = malloc(length + 1);
    if (!buffer) { fclose(f); return; }
    fread(buffer, 1, length, f);
    buffer[length] = '\0';
    fclose(f);

    char error_buf[256];
    ctz_json_value* root = ctz_json_parse(buffer, error_buf, sizeof(error_buf));
    free(buffer);
    if (!root) {
        fprintf(stderr, "[Cloud] Failed to parse %s: %s\n", NODE_CONFIG_FILE, error_buf);
        return;
    }
    if (ctz_json_get_type(root) != CTZ_JSON_OBJECT) {
        fprintf(stderr, "[Cloud] Config file %s is not a JSON object.\n", NODE_CONFIG_FILE);
        ctz_json_free(root);
        return;
    }

    pthread_mutex_lock(&node_list_mutex);
    for (size_t i = 0; i < ctz_json_get_object_size(root); i++) {
        const char* name = ctz_json_get_object_key(root, i);
        ctz_json_value* node_obj = ctz_json_get_object_value(root, i);
        if (ctz_json_get_type(node_obj) != CTZ_JSON_OBJECT) continue;

        WatchedNode* new_node = malloc(sizeof(WatchedNode));
        if (!new_node) continue;
        memset(new_node, 0, sizeof(WatchedNode));

        strncpy(new_node->name, name, sizeof(new_node->name) - 1);
        new_node->active = 1;
        new_node->time_format = TIME_UNIX; // Default
        new_node->filter_list_head = NULL; // Default

        ctz_json_value* path_val = ctz_json_find_object_value(node_obj, "path");
        if (path_val && ctz_json_get_type(path_val) == CTZ_JSON_STRING) {
            strncpy(new_node->path, ctz_json_get_string(path_val), sizeof(new_node->path) - 1);
        }
        // ... (copy other fields like author, desc, tag, current_version) ...
        ctz_json_value* author_val = ctz_json_find_object_value(node_obj, "author");
        if (author_val && ctz_json_get_type(author_val) == CTZ_JSON_STRING) {
            strncpy(new_node->author, ctz_json_get_string(author_val), sizeof(new_node->author) - 1);
        }
        ctz_json_value* desc_val = ctz_json_find_object_value(node_obj, "desc");
        if (desc_val && ctz_json_get_type(desc_val) == CTZ_JSON_STRING) {
            strncpy(new_node->desc, ctz_json_get_string(desc_val), sizeof(new_node->desc) - 1);
        }
        ctz_json_value* tag_val = ctz_json_find_object_value(node_obj, "tag");
        if (tag_val && ctz_json_get_type(tag_val) == CTZ_JSON_STRING) {
            strncpy(new_node->tag, ctz_json_get_string(tag_val), sizeof(new_node->tag) - 1);
        }
        ctz_json_value* ver_val = ctz_json_find_object_value(node_obj, "current_version");
        if (ver_val && ctz_json_get_type(ver_val) == CTZ_JSON_STRING) {
            strncpy(new_node->current_version, ctz_json_get_string(ver_val), sizeof(new_node->current_version) - 1);
        }

        new_node->is_auto = 0;
        snprintf(new_node->conf_path, sizeof(new_node->conf_path), "%s/.log/%s.conf", new_node->path, new_node->name);
        
        FILE* conf_file = fopen(new_node->conf_path, "r");
        if (conf_file) {
            char line[PATH_MAX + 10];
            while (fgets(line, sizeof(line), conf_file)) {
                line[strcspn(line, "\n")] = 0; // strip newline

                if (strncmp(line, "auto=", 5) == 0) {
                    if (strcmp(line + 5, "1") == 0) {
                        new_node->is_auto = 1;
                    }
                } else if (strncmp(line, "time=", 5) == 0) {
                    if (strcmp(line + 5, "Real") == 0) {
                        new_node->time_format = TIME_REAL;
                    }
                } else if (strncmp(line, "filter=", 7) == 0) {
                    free_filter_list(new_node->filter_list_head); // Clear just in case
                    new_node->filter_list_head = NULL;
                    char* filters = line + 7;
                    char* ext = strtok(filters, " ");
                    while (ext) {
                        FilterEntry* new_entry = malloc(sizeof(FilterEntry));
                        if (new_entry) {
                            strncpy(new_entry->extension, ext, sizeof(new_entry->extension) - 1);
                            new_entry->next = new_node->filter_list_head;
                            new_node->filter_list_head = new_entry;
                        }
                        ext = strtok(NULL, " ");
                    }
                }
            }
            fclose(conf_file);
        }
        
        if (new_node->is_auto) {
            printf("[Cloud] Node '%s' is configured for auto-surveillance.\n", new_node->name);
        }

        new_node->next = watched_nodes_head;
        watched_nodes_head = new_node;
    }
    pthread_mutex_unlock(&node_list_mutex);
    ctz_json_free(root);
}


void save_nodes() {
    FILE* f = fopen(config_file_path, "w");
    if (!f) return;

    fprintf(f, "{\n");
    //pthread_mutex_lock(&node_list_mutex);
    WatchedNode* current = watched_nodes_head;
    while (current) {
        fprintf(f, "  \"%s\": {\n", current->name);
        fprintf(f, "    \"path\": \"%s\",\n", current->path);
        fprintf(f, "    \"author\": \"%s\",\n", current->author);
        fprintf(f, "    \"desc\": \"%s\",\n", current->desc);
        fprintf(f, "    \"tag\": \"%s\",\n", current->tag);
        fprintf(f, "    \"current_version\": \"%s\"\n", current->current_version);
        fprintf(f, "  }%s\n", current->next ? "," : "");
        current = current->next;
    }
    pthread_mutex_unlock(&node_list_mutex);
    fprintf(f, "}\n");
    fclose(f);
}

void* stop_guardians_thread(void* arg) {
    (void)arg;

    printf("[Cloud] Stopping any active node guardians in background...\n");
    pthread_mutex_lock(&node_list_mutex);
    for (WatchedNode* n = watched_nodes_head; n; n = n->next) {
        if (n->is_auto) {
            char username[64];
            uid_t user_id;
            char home_dir[PATH_MAX];
            char command[PATH_MAX + 256]; // Increased buffer

            // Get the node's owner UID and their home dir
            if (get_user_from_path(n->path, username, sizeof(username), &user_id) != 0 || 
                get_home_and_name_from_uid(user_id, username, sizeof(username), home_dir, sizeof(home_dir)) != 0) {
                fprintf(stderr, "[Cloud] Could not find owner for node %s, skipping guardian stop.\n", n->name);
                continue;
            }

            // Check if a systemd service file exists for this node
            char service_path[PATH_MAX];
            snprintf(service_path, sizeof(service_path), "%s/.config/systemd/user/%s.service", home_dir, n->name);
            
            if (access(service_path, F_OK) == 0) {
                // Systemd mode: Stop the service using your hardcoded D-Bus path logic
                printf("[Cloud] ...stopping systemd guardian for '%s'\n", n->name);
                snprintf(command, sizeof(command), 
                         "runuser -u %s -- sh -c 'export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/%u/bus; /usr/bin/systemctl --user stop %s.service'", 
                         username, (unsigned int)user_id, n->name);
            } else {
                // Desktop mode: Kill the process
                printf("[Cloud] ...stopping desktop guardian for '%s'\n", n->name);
                char exec_path[PATH_MAX];
                snprintf(exec_path, sizeof(exec_path), "%s/.log/%s-guardian", n->path, n->name);
                snprintf(command, sizeof(command), "pkill -f \"%s\"", exec_path);
            }
            
            // Run the determined command
            system(command);
        }
    }
    pthread_mutex_unlock(&node_list_mutex);
    printf("[Cloud] Background guardian stop complete.\n");
    return NULL;
}



int main() {
    signal(SIGINT, int_handler);
    signal(SIGTERM, int_handler);

    printf("[Cloud] Initializing Cloud & Indexer Daemon...\n");
    cortez_mesh_t* mesh = cortez_mesh_init(CLOUD_DAEMON_NAME, NULL);
    if (!mesh) {
        fprintf(stderr, "[Cloud] Failed to initialize mesh.\n");
        return 1;
    }

    char exe_dir[PATH_MAX];
    if (get_executable_dir(exe_dir, sizeof(exe_dir)) != 0) {
        fprintf(stderr, "[Cloud] CRITICAL: Could not determine executable directory. Aborting.\n");
        cortez_mesh_shutdown(mesh);
        return 1;
    }

    strncpy(g_exe_dir, exe_dir, sizeof(g_exe_dir) - 1);

    snprintf(config_file_path, sizeof(config_file_path), "%s/%s", exe_dir, NODE_CONFIG_FILE);
    printf("[Cloud] Using config file: %s\n", config_file_path);

    printf("[Cloud] Daemon running with PID: %d. Waiting for tasks.\n", cortez_mesh_get_pid(mesh));

    inotify_fd = inotify_init1(IN_NONBLOCK);
    if (inotify_fd == -1) {
        perror("[Cloud] Failed to initialize inotify");
        cortez_mesh_shutdown(mesh);
        return 1;
    }


    load_nodes();
    pthread_t watcher_thread;
    if (pthread_create(&watcher_thread, NULL, watcher_thread_func, NULL) != 0) {
        fprintf(stderr, "[Cloud] Failed to create watcher thread.\n");
        close(inotify_fd);
        cortez_mesh_shutdown(mesh);
        return 1;
    }

    printf("[Cloud] Stopping any Independent Nodes...\n");
    pthread_t stop_thread;
    if (pthread_create(&stop_thread, NULL, stop_guardians_thread, NULL) != 0) {
        fprintf(stderr, "[Cloud] Warning: Failed to create guardian-stopper thread.\n");
    } else {
        pthread_join(stop_thread, NULL);
        printf("[Cloud] Guardian stop sequence finished.\n");
    }

    char signal_daemon_path[PATH_MAX];
    snprintf(signal_daemon_path, sizeof(signal_daemon_path), "%s/exodus-signal", g_exe_dir);

    g_signal_daemon_pid = fork();
    if (g_signal_daemon_pid == 0) {
        // Child process
        printf("[Cloud] Launching child process: %s\n", signal_daemon_path);
        execl(signal_daemon_path, "exodus-signal", (char*)NULL);
        perror("[Cloud] FATAL: execl exodus-signal failed");
        exit(1);
    } else if (g_signal_daemon_pid < 0) {
        perror("[Cloud] FATAL: fork for exodus-signal failed");
        g_signal_daemon_pid = 0;
        fprintf(stderr, "[Cloud] WARNING: Network features will be disabled.\n");
    } else {
        printf("[Cloud] Started exodus-signal process with PID: %d\n", g_signal_daemon_pid);
    }

    printf("[Cloud] Activating watches for all loaded nodes...\n");
    pthread_mutex_lock(&node_list_mutex);
    for (WatchedNode* n = watched_nodes_head; n; n = n->next) {
        if (n->active) {
            printf("[Cloud] ...resuming surveillance for node '%s' at %s\n", n->name, n->path);
            // This function adds the inotify watches
            add_watches_recursively(n, n->path);
            
            // This re-generates the contents.json so the 'look' command works immediately
            generate_node_contents_json(n);
        }
    }
    pthread_mutex_unlock(&node_list_mutex);
    printf("[Cloud] Initial surveillance activation complete.\n");

    send_local_node_list_to_signal(mesh);

    while (keep_running) {
        cortez_msg_t* msg = cortez_mesh_read(mesh, 1000); // 1-second timeout
        if (!msg) continue;

        pid_t sender_pid = cortez_msg_sender_pid(msg);
        uint16_t msg_type = cortez_msg_type(msg);

        if (sender_pid == g_signal_daemon_pid) {
            switch (msg_type) {
                case MSG_SIG_RESPONSE_UNIT_LIST:
                case MSG_SIG_RESPONSE_VIEW_UNIT:
                case MSG_SIG_RESPONSE_VIEW_CACHE:
                case MSG_OPERATION_ACK:
                {
                    printf("[Cloud] Received response from signal, forwarding to query daemon.\n");
                    pid_t query_daemon_pid = cortez_mesh_find_peer_by_name(mesh, QUERY_DAEMON_NAME);
                    if (query_daemon_pid > 0) {
                        // Forward the *entire* payload, which includes the request_id
                        write_to_handle_and_commit(mesh, query_daemon_pid, msg_type, 
                                                   cortez_msg_payload(msg), 
                                                   cortez_msg_payload_size(msg));
                    } else {
                        fprintf(stderr, "[Cloud] Cannot find query_daemon to forward response!\n");
                    }
                    break;
                }
                case MSG_SIG_SYNC_DATA:
                    handle_incoming_sync_data((const sig_sync_data_t*)cortez_msg_payload(msg));
                    break;
                case MSG_SIG_STATUS_UPDATE:
                    // TODO: Implement status update logic if needed
                    break;
            }
            cortez_mesh_msg_release(mesh, msg);
            continue; // Skip rest of loop
        }

        if (msg_type == MSG_TERMINATE) {
            printf("[Cloud] Termination signal received.\n");
            keep_running = 0;
        } else if (cortez_msg_payload_size(msg) < sizeof(uint64_t)) {

            fprintf(stderr, "[Cloud] Received malformed (too small) request, ignoring.\n");

        }else{
        
        const void* wrapped_payload = cortez_msg_payload(msg);
        uint32_t wrapped_payload_size = cortez_msg_payload_size(msg);
        if (wrapped_payload_size < sizeof(uint64_t)) {
            fprintf(stderr, "[Cloud] Received malformed (too small) request, ignoring.\n");
            cortez_mesh_msg_release(mesh, msg);
            continue;
        }

        uint64_t request_id;
        memcpy(&request_id, wrapped_payload, sizeof(uint64_t));
        const void* payload = (const char*)wrapped_payload + sizeof(uint64_t);
        uint32_t payload_size = wrapped_payload_size - sizeof(uint64_t);

        ack_t ack;
        ack.success = 1;
        strncpy(ack.details, "Operation successful.", sizeof(ack.details)-1);


        switch (cortez_msg_type(msg)) {
            case MSG_UPLOAD_FILE: {
                const char* file_path = payload;

                // Store the file path
                strncpy(last_uploaded_file_path, file_path, sizeof(last_uploaded_file_path) - 1);
                printf("[Cloud] Received upload request for: %s\n", file_path);
                FILE* f = fopen(file_path, "rb");
                if (f) {
                    fseek(f, 0, SEEK_END);
                    long f_size = ftell(f);
                    fseek(f, 0, SEEK_SET);
                    free(file_content);
                    file_content = malloc(f_size + 1);
                    if (file_content) {
                        file_size = fread(file_content, 1, f_size, f);
                        file_content[file_size] = '\0';
                        build_index();
                    }
                    fclose(f);
                } else {
                    ack.success = 0;
                    snprintf(ack.details, sizeof(ack.details), "Failed to open file: %s", file_path);
                }
                send_wrapped_response_zc(mesh, sender_pid, MSG_OPERATION_ACK, request_id, &ack, sizeof(ack));
                break;
            }

case MSG_QUERY_WORD: {
        const char* word = payload;
        printf("[Cloud] Received query for word: %s\n", word);
        
        WordIndex* entry = index_head;
        while (entry != NULL) {
            if (strcmp(entry->word, word) == 0) break;
            entry = entry->next;
        }

        if (entry) {
            // Calculate total size for response
            int num_sentences_to_send = 0;
            size_t sentences_total_len = 0;
            WordOccurrence* occ = entry->occurrences;
            while(occ && num_sentences_to_send < MAX_SENTENCES) {
                const char* sentence_end = strpbrk(occ->sentence_start, ".!?");
                size_t len = sentence_end ? (sentence_end - occ->sentence_start + 1) : strlen(occ->sentence_start);
                if (len > MAX_SENTENCE_LEN - 1) len = MAX_SENTENCE_LEN - 1;
                sentences_total_len += len + 1; // +1 for null terminator
                num_sentences_to_send++;
                occ = occ->next;
            }
            
            size_t total_resp_size = sizeof(query_response_t) + sentences_total_len;
            query_response_t* resp = malloc(total_resp_size);
            if (!resp) { /* handle error */ break; }

            resp->count = entry->count;
            strncpy(resp->word, word, MAX_WORD_LEN - 1);
            resp->num_sentences = num_sentences_to_send;

            char* current_sentence_ptr = resp->sentences;
            occ = entry->occurrences;
            int i = 0;
            while(occ && i < num_sentences_to_send) {
                const char* sentence_end = strpbrk(occ->sentence_start, ".!?");
                size_t len = sentence_end ? (sentence_end - occ->sentence_start + 1) : strlen(occ->sentence_start);
                if (len > MAX_SENTENCE_LEN - 1) len = MAX_SENTENCE_LEN - 1;
                
                memcpy(current_sentence_ptr, occ->sentence_start, len);
                current_sentence_ptr[len] = '\0';
                current_sentence_ptr += len + 1;
                i++;
                occ = occ->next;
            }

            send_wrapped_response_zc(mesh, sender_pid, MSG_QUERY_RESPONSE, request_id, resp, total_resp_size);
            free(resp);

        } else { // Word not found
            query_response_t resp = {0};
            strncpy(resp.word, word, MAX_WORD_LEN - 1);
            send_wrapped_response_zc(mesh, sender_pid, MSG_QUERY_RESPONSE, request_id, &resp, sizeof(resp));
        }
        break;
    }

case MSG_CHANGE_WORD: {
    ack_t ack = {.success = 0};
    char* current_content = NULL;
    char* new_content = NULL;
    FILE* original_file = NULL;
    FILE* temp_file = NULL;
    char temp_file_path[PATH_MAX + 4] = {0};

    if (payload_size < sizeof(change_word_req_t)) {
        snprintf(ack.details, sizeof(ack.details), "Invalid payload size for change request.");
        send_wrapped_response_zc(mesh, sender_pid, MSG_OPERATION_ACK, request_id, &ack, sizeof(ack));
        break;
    }
    const change_word_req_t* req = payload;

    if (last_uploaded_file_path[0] == '\0') {
        snprintf(ack.details, sizeof(ack.details), "No file has been uploaded to modify.");
        send_wrapped_response_zc(mesh, sender_pid, MSG_OPERATION_ACK, request_id, &ack, sizeof(ack));
        break;
    }

    original_file = fopen(last_uploaded_file_path, "rb");
    if (!original_file) {
        snprintf(ack.details, sizeof(ack.details), "Error: Could not open source file for reading.");
        send_wrapped_response_zc(mesh, sender_pid, MSG_OPERATION_ACK, request_id, &ack, sizeof(ack));
        break;
    }

    fseek(original_file, 0, SEEK_END);
    long current_size = ftell(original_file);
    fseek(original_file, 0, SEEK_SET);

    current_content = malloc(current_size + 1);
    if (!current_content) {
        fclose(original_file);
        snprintf(ack.details, sizeof(ack.details), "Error: Memory allocation failed for file buffer.");
        send_wrapped_response_zc(mesh, sender_pid, MSG_OPERATION_ACK, request_id, &ack, sizeof(ack));
        break;
    }

    if (fread(current_content, 1, current_size, original_file) != (size_t)current_size) {
        fclose(original_file);
        free(current_content);
        snprintf(ack.details, sizeof(ack.details), "Error: Failed to read source file content.");
        send_wrapped_response_zc(mesh, sender_pid, MSG_OPERATION_ACK, request_id, &ack, sizeof(ack));
        break;
    }
    current_content[current_size] = '\0';
    fclose(original_file);
    original_file = NULL;

    long occurrences = 0;
    const char* temp_ptr = current_content;
    size_t target_len = strnlen(req->target_word, MAX_WORD_LEN);
    if (target_len == 0) {
        free(current_content);
        snprintf(ack.details, sizeof(ack.details), "Error: Target word cannot be empty.");
        send_wrapped_response_zc(mesh, sender_pid, MSG_OPERATION_ACK, request_id, &ack, sizeof(ack));
        break;
    }

    while ((temp_ptr = strstr(temp_ptr, req->target_word))) {
        occurrences++;
        temp_ptr += target_len;
    }

    if (occurrences == 0) {
        free(current_content);
        ack.success = 1;
        snprintf(ack.details, sizeof(ack.details), "Target word not found. No changes made.");
        send_wrapped_response_zc(mesh, sender_pid, MSG_OPERATION_ACK, request_id, &ack, sizeof(ack));
        break;
    }

    size_t new_word_len = strnlen(req->new_word, MAX_WORD_LEN);
    long new_size = current_size + occurrences * (new_word_len - target_len);
    
    new_content = malloc(new_size + 1);
    if (!new_content) {
        free(current_content);
        snprintf(ack.details, sizeof(ack.details), "Error: Memory allocation failed for new content.");
        send_wrapped_response_zc(mesh, sender_pid, MSG_OPERATION_ACK, request_id, &ack, sizeof(ack));
        break;
    }

    char* current_pos = current_content;
    char* new_pos = new_content;
    char* occurrence;
    while ((occurrence = strstr(current_pos, req->target_word))) {
        ptrdiff_t len = occurrence - current_pos;
        memcpy(new_pos, current_pos, len);
        new_pos += len;
        memcpy(new_pos, req->new_word, new_word_len);
        new_pos += new_word_len;
        current_pos = occurrence + target_len;
    }
    strcpy(new_pos, current_pos);
    free(current_content);
    current_content = NULL;

    snprintf(temp_file_path, sizeof(temp_file_path), "%s.tmp", last_uploaded_file_path);
    temp_file = fopen(temp_file_path, "wb");
    if (!temp_file) {
        free(new_content);
        snprintf(ack.details, sizeof(ack.details), "Error: Could not create temporary file for writing.");
        send_wrapped_response_zc(mesh, sender_pid, MSG_OPERATION_ACK, request_id, &ack, sizeof(ack));
        break;
    }

    if (fwrite(new_content, 1, new_size, temp_file) != (size_t)new_size) {
        fclose(temp_file);
        remove(temp_file_path);
        free(new_content);
        snprintf(ack.details, sizeof(ack.details), "Error: Failed to write to temporary file.");
        send_wrapped_response_zc(mesh, sender_pid, MSG_OPERATION_ACK, request_id, &ack, sizeof(ack));
        break;
    }
    fclose(temp_file);

    if (rename(temp_file_path, last_uploaded_file_path) != 0) {
        remove(temp_file_path);
        free(new_content);
        snprintf(ack.details, sizeof(ack.details), "Error: Failed to replace original file.");
        send_wrapped_response_zc(mesh, sender_pid, MSG_OPERATION_ACK, request_id, &ack, sizeof(ack));
        break;
    }

    free(file_content);
    file_content = new_content;
    file_size = new_size;
    build_index();

    ack.success = 1;
    snprintf(ack.details, sizeof(ack.details), "File updated successfully. %ld occurrences changed.", occurrences);
    send_wrapped_response_zc(mesh, sender_pid, MSG_OPERATION_ACK, request_id, &ack, sizeof(ack));
    break;
}

    case MSG_WORD_COUNT: {
        printf("[Cloud] Received word count request.\n");
        long count = 0;
        if(file_content) {
            char* content_copy = strdup(file_content);
            char* token = strtok(content_copy, " \t\n\r");
            while(token) {
                count++;
                token = strtok(NULL, " \t\n\r");
            }
            free(content_copy);
        }
        count_response_t resp = {count};
        send_wrapped_response_zc(mesh, sender_pid, MSG_COUNT_RESPONSE, request_id, &resp, sizeof(resp));
        break;
    }

    case MSG_LINE_COUNT: {
        printf("[Cloud] Received line count request.\n");
        long count = 0;
        if(file_content) {
            for(size_t i = 0; i < file_size; i++) {
                if(file_content[i] == '\n') {
                    count++;
                }
            }
            if(file_size > 0) count++; // Count last line if no trailing newline
        }
        count_response_t resp = {count};
        send_wrapped_response_zc(mesh, sender_pid, MSG_COUNT_RESPONSE, request_id, &resp, sizeof(resp));
        break;
    }
    
    case MSG_CHAR_COUNT: {
        printf("[Cloud] Received char count request.\n");
        long count = 0;
        if(file_content) {
            for(size_t i = 0; i < file_size; i++) {
                if(!isspace((unsigned char)file_content[i])) {
                    count++;
                }
            }
        }
        count_response_t resp = {count};
        send_wrapped_response_zc(mesh, sender_pid, MSG_COUNT_RESPONSE, request_id, &resp, sizeof(resp));
        break;
    }



    case MSG_ADD_NODE: {
                const add_node_req_t* req = payload;
                WatchedNode* new_node = malloc(sizeof(WatchedNode));
                if (new_node) {

                    memset(new_node, 0, sizeof(WatchedNode));
                    
                    strncpy(new_node->type, "standard", sizeof(new_node->type) - 1);


                    strncpy(new_node->name, req->node_name, sizeof(new_node->name) - 1);
                    strncpy(new_node->path, req->path, sizeof(new_node->path) - 1);
                    new_node->active = 1;
                    new_node->history_head = NULL;
                    
                    pthread_mutex_lock(&node_list_mutex);
                    new_node->next = watched_nodes_head;
                    watched_nodes_head = new_node;
                    pthread_mutex_unlock(&node_list_mutex);
                    
                    pthread_mutex_lock(&node_list_mutex);
                    save_nodes();
                    pthread_mutex_unlock(&node_list_mutex);

                    initialize_node_log_file(new_node->path);
                    add_watches_recursively(new_node, new_node->path);
                    generate_node_contents_json(new_node);

                   ack.success = 1;

                    snprintf(ack.details, sizeof(ack.details), "Node '%s' added.", new_node->name);
                    send_wrapped_response_zc(mesh, sender_pid, MSG_OPERATION_ACK, request_id, &ack, sizeof(ack));
                } else {
                    ack.success = 0;
                    snprintf(ack.details, sizeof(ack.details), "Failed to allocate memory for new node.");
                    send_wrapped_response_zc(mesh, sender_pid, MSG_OPERATION_ACK, request_id, &ack, sizeof(ack));
                }

                if (ack.success) {
                        send_local_node_list_to_signal(mesh);
                }

                break;
    }

            case MSG_LIST_NODES: {
                char buffer[4096] = {0};
                char* current = buffer;
                int count = 0;
                pthread_mutex_lock(&node_list_mutex);
                for (WatchedNode* n = watched_nodes_head; n; n = n->next) {
                    current += snprintf(current, sizeof(buffer) - (current - buffer), "%s (%s)\n", n->name, n->active ? "active" : "inactive") + 1;
                    count++;
                }
                pthread_mutex_unlock(&node_list_mutex);
                
                size_t payload_size = sizeof(list_resp_t) + (current - buffer);
                list_resp_t* resp = malloc(payload_size);
                if (resp) {
                    resp->item_count = count;
                    memcpy(resp->data, buffer, (current - buffer));
                    send_wrapped_response_zc(mesh, sender_pid, MSG_LIST_NODES_RESPONSE, request_id, resp, payload_size);
                    free(resp);
                }
                break;
            }

            case MSG_VIEW_NODE: {
                const node_req_t* req = payload;
                char buffer[8192] = {0}; // Larger buffer for history
                char* pos = buffer;
                int count = 0;
                pthread_mutex_lock(&node_list_mutex);
                WatchedNode* node = watched_nodes_head;
                while (node) {
                    if (strcmp(node->name, req->node_name) == 0) break;
                    node = node->next;
                }
                if (node) {
                    for (NodeEvent* ev = node->history_head; ev; ev = ev->next) {
                        const char* type_str = ev->type == EV_CREATED ? "{Created}" : (ev->type == EV_DELETED ? "{Deleted}" : "{Modified}");
                        pos += snprintf(pos, sizeof(buffer) - (pos-buffer), "%s: \"%s\"\n", type_str, ev->name) + 1;
                        count++;
                    }
                }
                pthread_mutex_unlock(&node_list_mutex);
                
                size_t payload_size = sizeof(list_resp_t) + (pos - buffer);
                list_resp_t* resp = malloc(payload_size);
                if (resp) {
                    resp->item_count = count;
                    memcpy(resp->data, buffer, (pos - buffer));
                    send_wrapped_response_zc(mesh, sender_pid, MSG_VIEW_NODE_RESPONSE, request_id, resp, payload_size);
                    free(resp);
                }
                break;
            }

        case MSG_ACTIVATE_NODE:
        case MSG_DEACTIVATE_NODE: {
    const node_req_t* req = payload;
    int found = 0;
    int is_activating = (cortez_msg_type(msg) == MSG_ACTIVATE_NODE);

    pthread_mutex_lock(&node_list_mutex);
    for (WatchedNode* n = watched_nodes_head; n; n = n->next) {
        if (strcmp(n->name, req->node_name) == 0) {
            if (n->active != is_activating) {
                n->active = is_activating;
                if (is_activating) {
                    // --- NEW ---
                    add_watches_recursively(n, n->path);
                } else {
                    // --- NEW ---
                    remove_all_watches_for_node(n);
                }
            }
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&node_list_mutex);

    ack.success = found;
    snprintf(ack.details, sizeof(ack.details), "Node '%s' %s.", req->node_name, ack.success ? (is_activating ? "activated" : "deactivated") : "not found");
    send_wrapped_response_zc(mesh, sender_pid, MSG_OPERATION_ACK, request_id, &ack, sizeof(ack));
    break;
}

case MSG_REMOVE_NODE: {
    const node_req_t* req = payload;
    int found = 0;
    WatchedNode* node_to_remove = NULL;

    // --- Step 1: Find and unlink the node from the list ---
    pthread_mutex_lock(&node_list_mutex);
    WatchedNode** pptr = &watched_nodes_head;
    while (*pptr) {
        WatchedNode* entry = *pptr;
        if (strcmp(entry->name, req->node_name) == 0) {
            *pptr = entry->next; // Unlink from the list
            node_to_remove = entry; // Keep pointer to deal with it outside the lock
            found = 1;
            break;
        }
        pptr = &(*pptr)->next;
    }
    pthread_mutex_unlock(&node_list_mutex);

    // --- Step 2: Perform all slow I/O and cleanup outside the lock ---
    if (node_to_remove) {
        // --- NEW --- Remove all watches and clear cache for the node
        remove_all_watches_for_node(node_to_remove);
    
        // Delete log files
        char log_dir_path[PATH_MAX];
        char contents_file_path[PATH_MAX];
        char history_file_path[PATH_MAX];
        snprintf(log_dir_path, sizeof(log_dir_path), "%s/.log", node_to_remove->path);
        snprintf(contents_file_path, sizeof(contents_file_path), "%s/contents.json", log_dir_path);
        snprintf(history_file_path, sizeof(history_file_path), "%s/history.json", log_dir_path);
        remove(contents_file_path);
        remove(history_file_path);
        rmdir(log_dir_path);

        // Free node's own memory
        free_node_history(node_to_remove);
        free(node_to_remove);
    }

    if (found) {
        pthread_mutex_lock(&node_list_mutex);
        save_nodes(); // Update the config file
        pthread_mutex_unlock(&node_list_mutex);

        ack.success = 1;
        snprintf(ack.details, sizeof(ack.details), "Node '%s' removed.", req->node_name);
    } else {
        ack.success = 0;
        snprintf(ack.details, sizeof(ack.details), "Node '%s' not found.", req->node_name);
    }
    send_wrapped_response_zc(mesh, sender_pid, MSG_OPERATION_ACK, request_id, &ack, sizeof(ack));
    break;
}

                    if (ack.success) {
                        send_local_node_list_to_signal(mesh);
                    }

    case MSG_ATTR_NODE: {
            const attr_node_req_t* req = payload;
            int found = 0;
            pthread_mutex_lock(&node_list_mutex);
            for (WatchedNode* n = watched_nodes_head; n; n = n->next) {
                if (strcmp(n->name, req->node_name) == 0) {
                    if (req->flags & ATTR_FLAG_AUTHOR) strncpy(n->author, req->author, MAX_ATTR_LEN - 1);
                    if (req->flags & ATTR_FLAG_DESC) strncpy(n->desc, req->desc, MAX_ATTR_LEN - 1);
                    if (req->flags & ATTR_FLAG_TAG) strncpy(n->tag, req->tag, MAX_ATTR_LEN - 1);
                    found = 1;
                    break;
                }
            }
            pthread_mutex_unlock(&node_list_mutex);

            if (found) {
                pthread_mutex_lock(&node_list_mutex);
                save_nodes();
                pthread_mutex_unlock(&node_list_mutex);
                snprintf(ack.details, sizeof(ack.details), "Attributes for '%s' updated.", req->node_name);
            } else {
                ack.success = 0;
                snprintf(ack.details, sizeof(ack.details), "Node '%s' not found.", req->node_name);
            }
            send_wrapped_response_zc(mesh, sender_pid, MSG_OPERATION_ACK, request_id, &ack, sizeof(ack));
            break;
        }

        case MSG_INFO_NODE: {
            const node_req_t* req = payload;
            info_node_resp_t resp = { .success = 0 };
            pthread_mutex_lock(&node_list_mutex);
            memset(&resp, 0, sizeof(resp));
            for (WatchedNode* n = watched_nodes_head; n; n = n->next) {
                if (strcmp(n->name, req->node_name) == 0) {
                    resp.success = 1;
                    strncpy(resp.author, n->author, MAX_ATTR_LEN - 1);
                    strncpy(resp.desc, n->desc, MAX_ATTR_LEN - 1);
                    strncpy(resp.tag, n->tag, MAX_ATTR_LEN - 1);
                    strncpy(resp.current_version, n->current_version, MAX_NODE_NAME_LEN - 1);
                    break;
                }
            }
            pthread_mutex_unlock(&node_list_mutex);
            send_wrapped_response_zc(mesh, sender_pid, MSG_INFO_NODE_RESPONSE, request_id, &resp, sizeof(resp));
            break;
        }

        case MSG_SEARCH_ATTR: {
            const search_attr_req_t* req = payload;
            char buffer[8192] = {0};
            char* current = buffer;
            int count = 0;
            pthread_mutex_lock(&node_list_mutex);
            for (WatchedNode* n = watched_nodes_head; n; n = n->next) {
                int match = 0;
                if (req->type == SEARCH_BY_AUTHOR && strcmp(n->author, req->target) == 0) match = 1;
                if (req->type == SEARCH_BY_TAG && strcmp(n->tag, req->target) == 0) match = 1;

                if (match) {
                    current += snprintf(current, sizeof(buffer) - (current - buffer), "%s\n", n->name) + 1;
                    count++;
                }
            }
            pthread_mutex_unlock(&node_list_mutex);

            size_t payload_size = sizeof(list_resp_t) + (current - buffer);
            list_resp_t* resp = malloc(payload_size);
            if (resp) {
                resp->item_count = count;
                memcpy(resp->data, buffer, (current - buffer));
                // We can reuse the list nodes response message type
                send_wrapped_response_zc(mesh, sender_pid, MSG_LIST_NODES_RESPONSE, request_id, resp, payload_size);
                free(resp);
            }
            break;
        }

    case MSG_LOOKUP_ITEM: {
                const lookup_req_t* req = payload;
                char result_buffer[8192] = {0}; // Large buffer for results
                char* current = result_buffer;
                int count = 0;

                pthread_mutex_lock(&node_list_mutex);
                for (WatchedNode* n = watched_nodes_head; n; n = n->next) {
                    char contents_path[PATH_MAX];
                    snprintf(contents_path, sizeof(contents_path), "%s/.log/contents.json", n->path);

                    char error_buf[256];
                    ctz_json_value* contents = ctz_json_load_file(contents_path, error_buf, sizeof(error_buf));
                    if (contents && ctz_json_get_type(contents) == CTZ_JSON_ARRAY) {
                        for (size_t i = 0; i < ctz_json_get_array_size(contents); i++) {
                            ctz_json_value* item = ctz_json_get_array_element(contents, i);
                            ctz_json_value* name_val = ctz_json_find_object_value(item, "name");
                            if (name_val && strcmp(ctz_json_get_string(name_val), req->item_name) == 0) {
                                ctz_json_value* path_val = ctz_json_find_object_value(item, "path");
                                current += snprintf(current, sizeof(result_buffer) - (current - result_buffer), 
                                    "'%s' Found in Node '%s' | Path: %s\n", 
                                    req->item_name, n->name, ctz_json_get_string(path_val)) + 1;
                                count++;
                            }
                        }
                    }
                    if (contents) ctz_json_free(contents);
                }
                pthread_mutex_unlock(&node_list_mutex);

                if (count == 0) {
                    snprintf(current, sizeof(result_buffer) - (current - result_buffer), "'%s' not found in any active node.\n", req->item_name);
                    current += strlen(current) + 1;
                }
                
                size_t payload_size = sizeof(list_resp_t) + (current - result_buffer);
                list_resp_t* resp = malloc(payload_size);
                if (resp) {
                    resp->item_count = count;
                    memcpy(resp->data, result_buffer, (current - result_buffer));
                    send_wrapped_response_zc(mesh, sender_pid, MSG_LOOKUP_RESPONSE, request_id, resp, payload_size);
                    free(resp);
                }
                break;
            }

            case MSG_NODE_MAN_CREATE: {
                const node_man_create_req_t* req = payload;
                WatchedNode* node = find_node_by_name_locked(req->node_name);
                char full_path[PATH_MAX];

                if (!node) {
                    ack.success = 0;
                    snprintf(ack.details, sizeof(ack.details), "Node '%s' not found.", req->node_name);
                } else if (get_full_node_path(node, req->path, full_path, sizeof(full_path)) != 0) {
                    ack.success = 0;
                    snprintf(ack.details, sizeof(ack.details), "Invalid or insecure path.");
                } else {
                    int result = -1;
                    if (req->is_directory) {
                        result = mkdir(full_path, 0755);
                    } else {
                        int fd = open(full_path, O_CREAT | O_WRONLY | O_EXCL, 0644);
                        if (fd != -1) {
                            close(fd);
                            result = 0;
                        } else {
                            result = -1; // File might already exist
                        }
                    }
                    
                    if (result != 0) {
                        ack.success = 0;
                        snprintf(ack.details, sizeof(ack.details), "Failed to create: %s", strerror(errno));
                    } else {
                        snprintf(ack.details, sizeof(ack.details), "Created '%s'.", req->path);
                    }
                }
                send_wrapped_response_zc(mesh, sender_pid, MSG_OPERATION_ACK, request_id, &ack, sizeof(ack));
                break;
            }

            case MSG_NODE_MAN_DELETE: {
                const node_man_delete_req_t* req = payload;
                WatchedNode* node = find_node_by_name_locked(req->node_name);
                char full_path[PATH_MAX];

                if (!node) {
                    ack.success = 0;
                    snprintf(ack.details, sizeof(ack.details), "Node '%s' not found.", req->node_name);
                } else if (get_full_node_path(node, req->path, full_path, sizeof(full_path)) != 0) {
                    ack.success = 0;
                    snprintf(ack.details, sizeof(ack.details), "Invalid or insecure path.");
                } else {
                    struct stat st;
                    if (stat(full_path, &st) != 0) {
                        ack.success = 0;
                        snprintf(ack.details, sizeof(ack.details), "File/dir not found.");
                    } else {
                        
                        if (secure_recursive_delete(full_path) != 0) {
                            ack.success = 0;
                            snprintf(ack.details, sizeof(ack.details), "Failed to delete: %s", strerror(errno));
                        } else {
                            snprintf(ack.details, sizeof(ack.details), "Deleted '%s'.", req->path);
                        }
                    }
                }
                send_wrapped_response_zc(mesh, sender_pid, MSG_OPERATION_ACK, request_id, &ack, sizeof(ack));
                break;
            }
            
            case MSG_NODE_MAN_MOVE:
            case MSG_NODE_MAN_COPY: {
                const node_man_move_copy_req_t* req = payload;
                WatchedNode* src_node = find_node_by_name_locked(req->src_node);
                WatchedNode* dest_node = find_node_by_name_locked(req->dest_node);
                char src_full_path[PATH_MAX];
                char dest_full_path[PATH_MAX];

                if (!src_node || !dest_node) {
                    ack.success = 0;
                    snprintf(ack.details, sizeof(ack.details), "Source or destination node not found.");
                } else if (get_full_node_path(src_node, req->src_path, src_full_path, sizeof(src_full_path)) != 0 ||
                           get_full_node_path(dest_node, req->dest_path, dest_full_path, sizeof(dest_full_path)) != 0) {
                    ack.success = 0;
                    snprintf(ack.details, sizeof(ack.details), "Invalid or insecure source/dest path.");
                } else {
                    int result = -1;
                    struct stat st;
                    if (stat(src_full_path, &st) != 0) {
                         ack.success = 0;
                         snprintf(ack.details, sizeof(ack.details), "Source path not found.");
                         send_wrapped_response_zc(mesh, sender_pid, MSG_OPERATION_ACK, request_id, &ack, sizeof(ack));
                         break; // Exit switch case
                    }

                    if (msg_type == MSG_NODE_MAN_MOVE) {
                        result = rename(src_full_path, dest_full_path);
                    } else { // MSG_NODE_MAN_COPY
                        if (S_ISDIR(st.st_mode)) {
                            result = recursive_copy(src_full_path, dest_full_path);
                        } else {
                            result = copy_file(src_full_path, dest_full_path);
                        }
                    }

                    if (result != 0) {
                        ack.success = 0;
                        snprintf(ack.details, sizeof(ack.details), "Operation failed: %s", strerror(errno));
                    } else {
                        snprintf(ack.details, sizeof(ack.details), "Operation successful.");
                    }
                }
                send_wrapped_response_zc(mesh, sender_pid, MSG_OPERATION_ACK, request_id, &ack, sizeof(ack));
                break;
            }

            case MSG_PIN_ITEM: {
                const pin_req_t* req = payload;
                int found = 0;
                char found_path[MAX_PATH_LEN] = {0};
                char found_node[MAX_NODE_NAME_LEN] = {0};

                // Find the first occurrence of the item to pin it
                pthread_mutex_lock(&node_list_mutex);
                 for (WatchedNode* n = watched_nodes_head; n && !found; n = n->next) {
                    char contents_path[PATH_MAX];
                    snprintf(contents_path, sizeof(contents_path), "%s/.log/contents.json", n->path);
                    ctz_json_value* contents = ctz_json_load_file(contents_path, NULL, 0);
                    if (contents && ctz_json_get_type(contents) == CTZ_JSON_ARRAY) {
                        for (size_t i = 0; i < ctz_json_get_array_size(contents); i++) {
                            ctz_json_value* item = ctz_json_get_array_element(contents, i);
                            ctz_json_value* name_val = ctz_json_find_object_value(item, "name");
                            if (name_val && strcmp(ctz_json_get_string(name_val), req->item_name) == 0) {
                                ctz_json_value* path_val = ctz_json_find_object_value(item, "path");
                                strncpy(found_path, ctz_json_get_string(path_val), sizeof(found_path) - 1);
                                strncpy(found_node, n->name, sizeof(found_node) - 1);
                                found = 1;
                                break;
                            }
                        }
                    }
                    if (contents) ctz_json_free(contents);
                }
                pthread_mutex_unlock(&node_list_mutex);

                if (found) {
                    char pin_path[PATH_MAX];
                    get_pin_json_path(pin_path, sizeof(pin_path));
                    ctz_json_value* pins = ctz_json_load_file(pin_path, NULL, 0);
                    if (!pins || ctz_json_get_type(pins) != CTZ_JSON_OBJECT) {
                        if (pins) ctz_json_free(pins);
                        pins = ctz_json_new_object();
                    }
                    
                    ctz_json_value* pin_details = ctz_json_new_object();
                    ctz_json_object_set_value(pin_details, "file", ctz_json_new_string(req->item_name));
                    ctz_json_object_set_value(pin_details, "node", ctz_json_new_string(found_node));
                    ctz_json_object_set_value(pin_details, "path", ctz_json_new_string(found_path));

                    ctz_json_object_set_value(pins, req->pin_name, pin_details);

                    char* json_string = ctz_json_stringify(pins, 1);
                    FILE* f = fopen(pin_path, "w");
                    if (f && json_string) {
                       fprintf(f, "%s", json_string);
                       fclose(f);
                       snprintf(ack.details, sizeof(ack.details), "Pinned '%s' as '%s'.", req->item_name, req->pin_name);
                    } else { ack.success = 0; snprintf(ack.details, sizeof(ack.details), "Failed to write to pin file."); }
                    if (json_string) free(json_string);
                    ctz_json_free(pins);
                } else {
                    ack.success = 0;
                    snprintf(ack.details, sizeof(ack.details), "Could not find item '%s' to pin.", req->item_name);
                }
                send_wrapped_response_zc(mesh, sender_pid, MSG_OPERATION_ACK, request_id, &ack, sizeof(ack));
                break;
            }

            case MSG_SIG_REQUEST_UNIT_LIST:
                case MSG_SIG_REQUEST_VIEW_UNIT:
                case MSG_SIG_REQUEST_SYNC_NODE:
                case MSG_SIG_REQUEST_VIEW_CACHE:
                {
                    if (g_signal_daemon_pid == 0) {
                        ack.success = 0;
                        snprintf(ack.details, sizeof(ack.details), "Network signal daemon is not running.");
                        send_wrapped_response_zc(mesh, sender_pid, MSG_OPERATION_ACK, request_id, &ack, sizeof(ack));
                    } else {
                        printf("[Cloud] Forwarding network request (type %d) to signal daemon.\n", msg_type);
                        // Forward the *entire* wrapped payload (request_id + original payload)
                        write_to_handle_and_commit(mesh, g_signal_daemon_pid, msg_type, 
                                                   wrapped_payload, wrapped_payload_size);
                        // The response will come back asynchronously
                    }
                    break;
                }

            case MSG_UNPIN_ITEM: {
                const unpin_req_t* req = payload;
                char pin_path[PATH_MAX];
                get_pin_json_path(pin_path, sizeof(pin_path));
                
                ctz_json_value* pins = ctz_json_load_file(pin_path, NULL, 0);
                if (pins && ctz_json_get_type(pins) == CTZ_JSON_OBJECT) {
                    if (ctz_json_object_remove_value(pins, req->pin_name) == 0) {
                        char* json_string = ctz_json_stringify(pins, 1);
                        FILE* f = fopen(pin_path, "w");
                        if (f && json_string) {
                            fprintf(f, "%s", json_string);
                            fclose(f);
                            snprintf(ack.details, sizeof(ack.details), "Unpinned '%s'.", req->pin_name);
                        }
                         if (json_string) free(json_string);
                    } else {
                         ack.success = 0;
                         snprintf(ack.details, sizeof(ack.details), "Pin '%s' not found.", req->pin_name);
                    }
                    ctz_json_free(pins);
                } else {
                     ack.success = 0;
                     snprintf(ack.details, sizeof(ack.details), "No pins found or pin file is corrupt.");
                     if(pins) ctz_json_free(pins);
                }
                send_wrapped_response_zc(mesh, sender_pid, MSG_OPERATION_ACK, request_id, &ack, sizeof(ack));
                break;
            }


        }
    }

        cortez_mesh_msg_release(mesh, msg);
    }

    printf("[Cloud] Shutting down.\n");
    keep_running = 0; // Signal watcher thread to stop

    if (g_signal_daemon_pid > 0) {
        printf("[Cloud] Sending termination signal to exodus-signal (PID %d)...\n", g_signal_daemon_pid);
        // Send via mesh first
        send_wrapped_response_zc(mesh, g_signal_daemon_pid, MSG_TERMINATE, 0, "stop", 5);
        sleep(1); // Give it a second to shut down
        kill(g_signal_daemon_pid, SIGTERM);
        waitpid(g_signal_daemon_pid, NULL, 0);
        printf("[Cloud] exodus-signal shut down.\n");
    }

    pthread_join(watcher_thread, NULL); // Wait for it
    close(inotify_fd);

    sleep(1);
    printf("[Cloud] Handing off surveillance to node guardians...\n");
    pthread_mutex_lock(&node_list_mutex);
    for (WatchedNode* n = watched_nodes_head; n; n = n->next) {
        if (n->is_auto) {
            char username[64];
            uid_t user_id;
            char home_dir[PATH_MAX];

            // Get the node's owner UID and their home dir
            if (get_user_from_path(n->path, username, sizeof(username), &user_id) != 0 || 
                get_home_and_name_from_uid(user_id, username, sizeof(username), home_dir, sizeof(home_dir)) != 0) {
                fprintf(stderr, "[Cloud] Could not find owner for node %s, skipping guardian start.\n", n->name);
                continue;
            }

            // Check if a systemd service file exists
            char service_path[PATH_MAX];
            snprintf(service_path, sizeof(service_path), "%s/.config/systemd/user/%s.service", home_dir, n->name);

            if (access(service_path, F_OK) == 0) {
                // Systemd mode: Start the service using your hardcoded D-Bus path logic
                printf("[Cloud] ...re-launching systemd guardian for '%s'\n", n->name);
                char command[PATH_MAX + 256];
                snprintf(command, sizeof(command), 
                         "runuser -u %s -- sh -c 'export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/%u/bus; /usr/bin/systemctl --user start %s.service'", 
                         username, (unsigned int)user_id, n->name);
                system(command);
            } else {
                // Desktop mode: Relaunch the process manually
                printf("[Cloud] ...re-launching desktop guardian for '%s'\n", n->name);
                char exec_path[PATH_MAX];
                snprintf(exec_path, sizeof(exec_path), "%s/.log/%s-guardian", n->path, n->name);
                
                pid_t pid = fork();
                if (pid == 0) {
                    // Child process
                    if (setuid(user_id) != 0) {
                        perror("[Cloud] setuid failed, guardian may not start");
                        // Don't exit, try to run anyway
                    }
                    
                    execl(exec_path, exec_path, (char*)NULL);
                    perror("[Cloud] execl failed for guardian");
                    exit(1); // Exit child process if execl fails
                    
                } else if (pid < 0) {
                    // Fork failed in parent
                    perror("[Cloud] fork failed for guardian");
                }
            }
        }
    }
    pthread_mutex_unlock(&node_list_mutex);

    pthread_mutex_lock(&node_list_mutex);
    save_nodes();
    pthread_mutex_unlock(&node_list_mutex);

    free(file_content);
    free_index();
    cortez_mesh_shutdown(mesh);
    return 0;
}