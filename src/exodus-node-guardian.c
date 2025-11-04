/*
 * exodus-node-guardian.c
 * Standalone watcher for a single node.
 *
 * Compile:
 * gcc -Wall -Wextra -O2 exodus-node-guardian.c ctz-json.a -o exodus-node-guardian -pthread
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/inotify.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <dirent.h>
#include <libgen.h>
#include <time.h>
#include <ctype.h>

#include "ctz-json.h"
#include "exodus-common.h"

// --- Struct Definitions (Copied from cloud-daemon) ---

typedef struct WatchDescriptorMap {
    int wd;
    char path[PATH_MAX];
    struct WatchDescriptorMap* next;
} WatchDescriptorMap;

typedef struct FileCache {
    char path[PATH_MAX];
    char* content;
    struct FileCache* next;
    time_t last_processed_time;
} FileCache;

typedef enum {
    EV_CREATED,
    EV_DELETED,
    EV_MODIFIED
} EventType;

typedef enum {
    TIME_UNIX,
    TIME_REAL
} TimeFormat;

typedef struct FilterEntry {
    char extension[32];
    struct FilterEntry* next;
} FilterEntry;

// --- Global Variables ---

static volatile int g_keep_running = 1;
static int g_inotify_fd = -1;
static char g_node_path[PATH_MAX] = {0};
static char g_history_file_path[PATH_MAX] = {0};

static WatchDescriptorMap* g_wd_map_head = NULL;
static FileCache* g_file_cache_head = NULL;
static pthread_mutex_t g_wd_map_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_file_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static TimeFormat g_time_format = TIME_UNIX;
static FilterEntry* g_filter_list_head = NULL;
static char g_node_name[MAX_NODE_NAME_LEN] = {0};

// --- Signal Handler ---

void int_handler(int dummy) {
    (void)dummy;
    g_keep_running = 0;
    fprintf(stderr, "[Guardian] Received termination signal.\n");
}

// --- Helper Functions (Minimal versions from cloud-daemon) ---

static int check_and_update_debounce(const char* full_path) {
    const int DEBOUNCE_SECONDS = 2;
    time_t now = time(NULL);
    int should_process = 0;

    pthread_mutex_lock(&g_file_cache_mutex);
    
    FileCache* current = g_file_cache_head;
    while (current) {
        if (strcmp(current->path, full_path) == 0) {
            
            if (now < current->last_processed_time + DEBOUNCE_SECONDS) {
                
                should_process = 0;
                current->last_processed_time = now;
            } else {

                current->last_processed_time = now;
                should_process = 1;
            }
        
            pthread_mutex_unlock(&g_file_cache_mutex);
            return should_process;
        }
        current = current->next;
    }

    FileCache* new_entry = malloc(sizeof(FileCache));
    if (new_entry) {
        strncpy(new_entry->path, full_path, sizeof(new_entry->path) - 1);
        new_entry->path[sizeof(new_entry->path) - 1] = '\0';
        new_entry->content = NULL; 
        new_entry->last_processed_time = now;
        new_entry->next = g_file_cache_head;
        g_file_cache_head = new_entry;
    }
    should_process = 1; 

    pthread_mutex_unlock(&g_file_cache_mutex);
    return should_process;
}

static void get_real_time_string(char* buf, size_t size) {
    time_t now = time(NULL);
    struct tm local_time;
    localtime_r(&now, &local_time);
    strftime(buf, size, "%Y-%m-%d %H:%M:%S", &local_time);
}

static void free_filter_list() {
    FilterEntry* current = g_filter_list_head;
    while (current) {
        FilterEntry* next = current->next;
        free(current);
        current = next;
    }
    g_filter_list_head = NULL;
}

static void load_guardian_config() {
    char conf_path[PATH_MAX];
    snprintf(conf_path, sizeof(conf_path), "%s/.log/%s.conf", g_node_path, g_node_name);

    FILE* f = fopen(conf_path, "r");
    if (!f) {
        fprintf(stderr, "[Guardian] No config file found at %s. Using defaults.\n", conf_path);
        return;
    }

    char line[PATH_MAX + 10];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0; // strip newline
        
        if (strncmp(line, "time=", 5) == 0) {
            if (strcmp(line + 5, "Real") == 0) {
                g_time_format = TIME_REAL;
            } else {
                g_time_format = TIME_UNIX;
            }
        } else if (strncmp(line, "filter=", 7) == 0) {
            free_filter_list(); // Clear any existing filters
            char* filters = line + 7;
            char* ext = strtok(filters, " ");
            while (ext) {
                FilterEntry* new_entry = malloc(sizeof(FilterEntry));
                if (new_entry) {
                    strncpy(new_entry->extension, ext, sizeof(new_entry->extension) - 1);
                    new_entry->next = g_filter_list_head;
                    g_filter_list_head = new_entry;
                }
                ext = strtok(NULL, " ");
            }
        }
    }
    fclose(f);

    fprintf(stderr, "[Guardian] Config loaded: time=%s\n", g_time_format == TIME_REAL ? "Real" : "Unix");
    fprintf(stderr, "[Guardian] Loaded filters: ");
    for (FilterEntry* f = g_filter_list_head; f; f = f->next) {
        fprintf(stderr, "%s ", f->extension);
    }
    fprintf(stderr, "\n");
}

static int is_file_filtered(const char* filename) {
    if (!g_filter_list_head) {
        return 0; // No filters set
    }
    
    for (FilterEntry* f = g_filter_list_head; f; f = f->next) {
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

char* read_file_content(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (length < 0) { fclose(f); return NULL; }
    char* buffer = malloc(length + 1);
    if (!buffer) { fclose(f); return NULL; }
    if (fread(buffer, 1, length, f) != (size_t)length) {
        free(buffer); fclose(f); return NULL;
    }
    buffer[length] = '\0';
    fclose(f);
    return buffer;
}

void update_file_cache(const char* path, const char* content) {
    pthread_mutex_lock(&g_file_cache_mutex);
    FileCache* current = g_file_cache_head;
    while (current) {
        if (strcmp(current->path, path) == 0) {
            free(current->content);
            current->content = content ? strdup(content) : NULL;
            current->last_processed_time = time(NULL);
            pthread_mutex_unlock(&g_file_cache_mutex);
            return;
        }
        current = current->next;
    }
    if (content) {
        FileCache* new_entry = malloc(sizeof(FileCache));
        if (new_entry) {
            strncpy(new_entry->path, path, sizeof(new_entry->path) - 1);
            new_entry->content = strdup(content);
            new_entry->last_processed_time = time(NULL);
            new_entry->next = g_file_cache_head;
            g_file_cache_head = new_entry;
        }
    }
    pthread_mutex_unlock(&g_file_cache_mutex);
}

void add_event_to_history(EventType type, const char* name, const char* details_json_obj) {
    
    // 1. Load the existing history file
    ctz_json_value* history_array = ctz_json_load_file(g_history_file_path, NULL, 0);
    if (!history_array || ctz_json_get_type(history_array) != CTZ_JSON_ARRAY) {
        if (history_array) ctz_json_free(history_array);
        history_array = ctz_json_new_array();
    }

    // 2. Create the new JSON object for this event.
    ctz_json_value* event_obj = ctz_json_new_object();
    const char* type_str = (type == EV_CREATED) ? "Created" : (type == EV_DELETED ? "Deleted" : "Modified");
    
    ctz_json_object_set_value(event_obj, "event", ctz_json_new_string(type_str));
    ctz_json_object_set_value(event_obj, "name", ctz_json_new_string(name));

    
    if (g_time_format == TIME_REAL) {
        char time_buf[64];
        get_real_time_string(time_buf, sizeof(time_buf));
        ctz_json_object_set_value(event_obj, "timestamp", ctz_json_new_string(time_buf));
    } else {
        double new_timestamp = (double)time(NULL);
        ctz_json_object_set_value(event_obj, "timestamp", ctz_json_new_number(new_timestamp));
    }
    

    if (details_json_obj) {
        ctz_json_value* changes_obj = ctz_json_parse(details_json_obj, NULL, 0);
        if (changes_obj) {
            ctz_json_object_set_value(event_obj, "changes", changes_obj);
        }
    }


    size_t history_size = ctz_json_get_array_size(history_array);
    if (history_size > 0) {
        ctz_json_value* last_event = ctz_json_get_array_element(history_array, history_size - 1);
        if (last_event && ctz_json_get_type(last_event) == CTZ_JSON_OBJECT) {
            ctz_json_value* last_name_val = ctz_json_find_object_value(last_event, "name");
            ctz_json_value* last_event_val = ctz_json_find_object_value(last_event, "event");

            if (last_name_val && last_event_val) {
                const char* last_name = ctz_json_get_string(last_name_val);
                const char* last_event_str = ctz_json_get_string(last_event_val);
                
                // If name and event are the same, check timestamp to see if it's *too* close
                if (strcmp(last_name, name) == 0 && strcmp(last_event_str, type_str) == 0) {
                    double last_time_num = 0;
                    ctz_json_value* last_time_val = ctz_json_find_object_value(last_event, "timestamp");
                    if (last_time_val && ctz_json_get_type(last_time_val) == CTZ_JSON_NUMBER) {
                        last_time_num = ctz_json_get_number(last_time_val);
                    }
                    
                    if (g_time_format == TIME_UNIX) {
                         // If timestamps are identical (same second), it's a duplicate
                        if (last_time_num == (double)time(NULL)) {
                             fprintf(stderr, "[Guardian] Skipping duplicate event (same second): %s %s\n", type_str, name);
                             ctz_json_free(event_obj);
                             ctz_json_free(history_array);
                             return;
                        }
                    }
                    // Cannot easily check for 'Real' time duplicates, so we allow them
                }
            }
        }
    }


    ctz_json_array_push_value(history_array, event_obj);

    // 5. Stringify and save
    char* json_output = ctz_json_stringify(history_array, 1); // Pretty print
    if (json_output) {
        FILE* f = fopen(g_history_file_path, "w");
        if (f) {
            fprintf(f, "%s", json_output);
            fclose(f);
            fprintf(stderr, "[Guardian] Logged event: %s %s\n", type_str, name);
        } else {
            fprintf(stderr, "[Guardian] CRITICAL: Failed to write to log file %s\n", g_history_file_path);
        }
        free(json_output);
    }
    ctz_json_free(history_array);
}

void handle_file_modification(const char* full_path) {

    // Use the existing guardian debounce function
    if (!check_and_update_debounce(full_path)) {
        return; 
    }

    char* new_content = read_file_content(full_path);
    if (!new_content) return; // Cannot read the file, so we can't process it.

    char* old_content = NULL;
    
    // Find the previous content in our cache
    pthread_mutex_lock(&g_file_cache_mutex);
    for (FileCache* current = g_file_cache_head; current; current = current->next) {
        if (strcmp(current->path, full_path) == 0) {
            old_content = current->content ? strdup(current->content) : NULL;
            break;
        }
    }
    pthread_mutex_unlock(&g_file_cache_mutex);

    // Calculate relative_path using the guardian's global g_node_path
    const char* relative_path = full_path + strlen(g_node_path) + 1;
    
    // If we have no previous state, we can't diff. Log a simple modification.
    if (!old_content) {
        add_event_to_history(EV_MODIFIED, relative_path, NULL);
        update_file_cache(full_path, new_content);
        free(new_content);
        return;
    }
    
    // --- Naive Line-Based Diff Implementation ---
    ctz_json_value* changes_obj = ctz_json_new_object();
    ctz_json_value* added_array = ctz_json_new_array();
    ctz_json_value* removed_array = ctz_json_new_array();

    char* old_copy = strdup(old_content);
    char* new_copy = strdup(new_content);
    
    // Find lines in old_content that are not in new_content
    char* saveptr1;
    char* old_line = strtok_r(old_copy, "\n", &saveptr1);
    while(old_line) {
        if (strstr(new_content, old_line) == NULL) {
            ctz_json_array_push_value(removed_array, ctz_json_new_string(old_line));
        }
        old_line = strtok_r(NULL, "\n", &saveptr1);
    }

    // Find lines in new_content that are not in old_content
    char* saveptr2;
    char* new_line = strtok_r(new_copy, "\n", &saveptr2);
    while(new_line) {
        if (strstr(old_content, new_line) == NULL) {
             ctz_json_array_push_value(added_array, ctz_json_new_string(new_line));
        }
        new_line = strtok_r(NULL, "\n", &saveptr2);
    }
    free(old_copy);
    free(new_copy);

    char* details_json_string = NULL;
    if (ctz_json_get_array_size(added_array) > 0) ctz_json_object_set_value(changes_obj, "added", added_array);
    else ctz_json_free(added_array);
    
    if (ctz_json_get_array_size(removed_array) > 0) ctz_json_object_set_value(changes_obj, "removed", removed_array);
    else ctz_json_free(removed_array);

    if (ctz_json_get_object_size(changes_obj) > 0) {
        details_json_string = ctz_json_stringify(changes_obj, 0);
    }
    
    // Use the guardian's add_event_to_history function
    add_event_to_history(EV_MODIFIED, relative_path, details_json_string);
    
    // Cleanup
    if (details_json_string) free(details_json_string);
    ctz_json_free(changes_obj);
    free(old_content);
    
    // Finally, update the cache with the new content for the next change
    update_file_cache(full_path, new_content);
    free(new_content);
}

void add_watches_recursively(const char* base_path) {
    DIR* dir = opendir(base_path);
    if (!dir) return;

    int wd = inotify_add_watch(g_inotify_fd, base_path, IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO);
    if (wd == -1) {
        closedir(dir);
        return;
    }

    WatchDescriptorMap* new_map_entry = malloc(sizeof(WatchDescriptorMap));
    if (new_map_entry) {
        new_map_entry->wd = wd;
        strncpy(new_map_entry->path, base_path, sizeof(new_map_entry->path) - 1);
        pthread_mutex_lock(&g_wd_map_mutex);
        new_map_entry->next = g_wd_map_head;
        g_wd_map_head = new_map_entry;
        pthread_mutex_unlock(&g_wd_map_mutex);
    } else {
        inotify_rm_watch(g_inotify_fd, wd);
    }
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 || strcmp(entry->d_name, ".log") == 0) continue;

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", base_path, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                add_watches_recursively(full_path);
            } else if (S_ISREG(st.st_mode)) {
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

void free_file_cache() {
    pthread_mutex_lock(&g_file_cache_mutex);
    FileCache* current = g_file_cache_head;
    while (current) {
        FileCache* next = current->next;
        free(current->content);
        free(current);
        current = next;
    }
    g_file_cache_head = NULL;
    pthread_mutex_unlock(&g_file_cache_mutex);
}

void free_wd_map() {
    pthread_mutex_lock(&g_wd_map_mutex);
    WatchDescriptorMap* current = g_wd_map_head;
    while (current) {
        WatchDescriptorMap* next = current->next;

        free(current);
        current = next;
    }
    g_wd_map_head = NULL;
    pthread_mutex_unlock(&g_wd_map_mutex);
}

void* watcher_thread_func(void* arg) {
    (void)arg;
    char buffer[4 * (sizeof(struct inotify_event) + NAME_MAX + 1)];

    while (g_keep_running) {
        ssize_t len = read(g_inotify_fd, buffer, sizeof(buffer));
        if (len <= 0) {
            if (g_keep_running) usleep(100000);
            continue;
        }

        ssize_t i = 0;
        while (i < len) {
            struct inotify_event* event = (struct inotify_event*)&buffer[i];
            
            pthread_mutex_lock(&g_wd_map_mutex);
            WatchDescriptorMap* map_entry = NULL;
            for (WatchDescriptorMap* current = g_wd_map_head; current; current = current->next) {
                if (current->wd == event->wd) {
                    map_entry = current;
                    break;
                }
            }
            pthread_mutex_unlock(&g_wd_map_mutex);

            if (map_entry && event->len > 0) {

                char event_full_path[PATH_MAX];
                snprintf(event_full_path, sizeof(event_full_path), "%s/%s", map_entry->path, event->name);

                const char* relative_path = event_full_path + strlen(g_node_path) + 1;

                if (event->mask & IN_ISDIR) {
                   
                    if (event->mask & (IN_CREATE | IN_MOVED_TO)) {
                        if (check_and_update_debounce(event_full_path)) { 
                            add_event_to_history(EV_CREATED, relative_path, NULL);
                            add_watches_recursively(event_full_path);
                        }
                    } else if (event->mask & (IN_DELETE | IN_MOVED_FROM)) {
                        if (check_and_update_debounce(event_full_path)) { 
                            add_event_to_history(EV_DELETED, relative_path, NULL);
                        }
                    }
                    
                } else {
                    
                    if (event->mask & (IN_CREATE | IN_MOVED_TO | IN_MODIFY)) {
                        if (is_file_filtered(event->name)) {
                            if (check_and_update_debounce(event_full_path)) {
                                // File is filtered. Delete it and log as "Deleted".
                                unlink(event_full_path);
                                // Log a special "Deleted (Filtered)" event
                                add_event_to_history(EV_DELETED, relative_path, "{\"reason\":\"Filtered\"}");
                                update_file_cache(event_full_path, NULL); // Remove from cache
                            }
                            
                            i += sizeof(struct inotify_event) + event->len;
                            continue;
                        }
                    }
                    // --- END NEW ---

                    if (event->mask & (IN_CREATE | IN_MOVED_TO)) {
                        if (check_and_update_debounce(event_full_path)) { 
                            add_event_to_history(EV_CREATED, relative_path, NULL);
                            char* content = read_file_content(event_full_path);
                            if (content) {
                                update_file_cache(event_full_path, content);
                                free(content);
                            }
                        }
                    }
                    if (event->mask & (IN_DELETE | IN_MOVED_FROM)) {
                        if (check_and_update_debounce(event_full_path)) { 
                            add_event_to_history(EV_DELETED, relative_path, NULL);
                            update_file_cache(event_full_path, NULL);
                        }
                    }
                    
                    if (event->mask & IN_MODIFY) {
                        handle_file_modification(event_full_path);
                    }
                }
            }

            if (event->mask & IN_IGNORED) {
                pthread_mutex_lock(&g_wd_map_mutex);
                WatchDescriptorMap** pptr = &g_wd_map_head;
                while (*pptr) {
                    WatchDescriptorMap* entry = *pptr;
                    if (entry->wd == event->wd) {
                        *pptr = entry->next;
                        free(entry);
                        break;
                    }
                    pptr = &(*pptr)->next;
                }
                pthread_mutex_unlock(&g_wd_map_mutex);
            }
            i += sizeof(struct inotify_event) + event->len;
        }
    }
    return NULL;
}

int main() {
    signal(SIGINT, int_handler);
    signal(SIGTERM, int_handler);

    char exe_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len == -1) {
        perror("[Guardian] readlink failed");
        return 1;
    }
    exe_path[len] = '\0';


    char log_dir[PATH_MAX];
    char* exe_name = basename(exe_path); // e.g., "my_node-guardian"
    

    char* guardian_suffix = strstr(exe_name, "-guardian");
    if (guardian_suffix != NULL) {
        // Found suffix, copy the part before it
        size_t name_len = guardian_suffix - exe_name;
        if (name_len > MAX_NODE_NAME_LEN - 1) name_len = MAX_NODE_NAME_LEN - 1;
        strncpy(g_node_name, exe_name, name_len);
        g_node_name[name_len] = '\0';
    } else {
        // Did not find suffix, use the whole name (fallback)
        strncpy(g_node_name, exe_name, sizeof(g_node_name) - 1);
    }

    strncpy(log_dir, dirname(exe_path), sizeof(log_dir));
    strncpy(g_node_path, dirname(log_dir), sizeof(g_node_path));


    snprintf(g_history_file_path, sizeof(g_history_file_path), "%s/.log/history.json", g_node_path);

    fprintf(stderr, "[Guardian] Starting surveillance for node '%s' at %s\n", g_node_name, g_node_path);
    fprintf(stderr, "[Guardian] Logging to: %s\n", g_history_file_path);

    load_guardian_config();
    

    g_inotify_fd = inotify_init1(IN_NONBLOCK);
    if (g_inotify_fd == -1) {
        perror("[Guardian] Failed to initialize inotify");
        return 1;
    }

    pthread_t watcher_thread;
    if (pthread_create(&watcher_thread, NULL, watcher_thread_func, NULL) != 0) {
        fprintf(stderr, "[Guardian] Failed to create watcher thread.\n");
        close(g_inotify_fd);
        return 1;
    }

    add_watches_recursively(g_node_path);
    fprintf(stderr, "[Guardian] Initial watch setup complete. Running...\n");

    while (g_keep_running) {
        sleep(1);
    }

    fprintf(stderr, "[Guardian] Shutting down watcher thread...\n");
    pthread_join(watcher_thread, NULL);
    close(g_inotify_fd);

    fprintf(stderr, "[Guardian] Cleaning up resources...\n");
    free_file_cache();
    free_wd_map();
    free_filter_list(); // --- NEW: Cleanup filters ---

    pthread_mutex_destroy(&g_wd_map_mutex);
    pthread_mutex_destroy(&g_file_cache_mutex);

    fprintf(stderr, "[Guardian] Shutdown complete.\n");
    return 0;
}