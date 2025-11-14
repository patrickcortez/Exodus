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
#include <sys/types.h>

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

static int split_lines(char* content_copy, char*** lines_array_out) {
    if (!content_copy) {
        *lines_array_out = NULL;
        return 0;
    }

    int line_count = 0;
    // 1. Count the lines
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
    
    // Always add the first line
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

void add_event_to_history(EventType type, const char* name, const char* user, const char* details_json_obj){
    
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
    ctz_json_object_set_value(event_obj, "user", ctz_json_new_string(user ? user : "unknown"));

    
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

void handle_file_modification(const char* full_path, const char* user) {

    // Use the existing guardian debounce function
    if (!check_and_update_debounce(full_path)) {
        return; 
    }

    char* new_content = read_file_content(full_path);
    if (!new_content) return; // Cannot read the file, so we can't process it.

    char* old_content = NULL;
    
    // Find the previous content in our cache (using guardian's global cache)
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
        add_event_to_history(EV_MODIFIED, relative_path, user, NULL); // Use guardian's log function
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

    // 2. Backtrack through the matrix to build the diff lists
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
            change->matched = 0;
            change->next = added_list_head;
            added_list_head = change;
            j--;
        } else if (i > 0 && (j == 0 || lcs_matrix[i][j - 1] < lcs_matrix[i - 1][j])) {
            // Removed line (from old)
            DiffChange* change = malloc(sizeof(DiffChange));
            change->op = 'd';
            change->line_num = i;
            change->content = old_lines[i - 1];
            change->matched = 0;
            change->next = removed_list_head;
            removed_list_head = change;
            i--;
        }
    }

    // 2.5. Detect Moved Lines (Post-processing)
    MovedChange* moved_list_head = NULL;
    for (DiffChange* r_node = removed_list_head; r_node; r_node = r_node->next) {
        if (r_node->matched) continue;
        for (DiffChange* a_node = added_list_head; a_node; a_node = a_node->next) {
            if (a_node->matched) continue;
            if (strcmp(r_node->content, a_node->content) == 0) {
                // We found a move!
                MovedChange* move = malloc(sizeof(MovedChange));
                move->from_line = r_node->line_num;
                move->to_line = a_node->line_num;
                move->content = r_node->content;
                move->next = moved_list_head;
                moved_list_head = move;
                r_node->matched = 1;
                a_node->matched = 1;
                break;
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
        if (!current_change->matched) {
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
        if (!current_change->matched) {
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
    
    // Use the guardian's add_event_to_history function
    add_event_to_history(EV_MODIFIED, relative_path, user, details_json_string);
    
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

                if (event->mask & IN_ISDIR) {
                   
                    if (event->mask & (IN_CREATE | IN_MOVED_TO)) {
                        if (check_and_update_debounce(event_full_path)) { 
                            add_event_to_history(EV_CREATED, relative_path, event_user, NULL);
                            add_watches_recursively(event_full_path);
                        }
                    } else if (event->mask & (IN_DELETE | IN_MOVED_FROM)) {
                        if (check_and_update_debounce(event_full_path)) { 
                            add_event_to_history(EV_DELETED, relative_path, event_user, NULL);
                        }
                    }
                    
                } else {
                    
                    if (event->mask & (IN_CREATE | IN_MOVED_TO | IN_MODIFY)) {
                        if (is_file_filtered(event->name)) {
                            if (check_and_update_debounce(event_full_path)) {
                                // File is filtered. Delete it and log as "Deleted".
                                unlink(event_full_path);
                                // Log a special "Deleted (Filtered)" event
                                add_event_to_history(EV_DELETED, relative_path, event_user, "{\"reason\":\"Filtered\"}");
                                update_file_cache(event_full_path, NULL); // Remove from cache
                            }
                            
                            i += sizeof(struct inotify_event) + event->len;
                            continue;
                        }
                    }
                    // --- END NEW ---

                    if (event->mask & (IN_CREATE | IN_MOVED_TO)) {
                        if (check_and_update_debounce(event_full_path)) { 
                            add_event_to_history(EV_CREATED, relative_path, event_user, NULL);
                            char* content = read_file_content(event_full_path);
                            if (content) {
                                update_file_cache(event_full_path, content);
                                free(content);
                            }
                        }
                    }
                    if (event->mask & (IN_DELETE | IN_MOVED_FROM)) {
                        if (check_and_update_debounce(event_full_path)) { 
                            add_event_to_history(EV_DELETED, relative_path, event_user, NULL);
                            update_file_cache(event_full_path, NULL);
                        }
                    }
                    
                    if (event->mask & IN_MODIFY) {
                        handle_file_modification(event_full_path, event_user);
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
    free_filter_list();

    pthread_mutex_destroy(&g_wd_map_mutex);
    pthread_mutex_destroy(&g_file_cache_mutex);

    fprintf(stderr, "[Guardian] Shutdown complete.\n");
    return 0;
}