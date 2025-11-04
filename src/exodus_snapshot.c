/*
 * exodus_snapshot.c - Git-like object storage model
 *
 *
 * COMPILE:
 * gcc -Wall -Wextra -O2 exodus_snapshot.c cortez_ipc.o ctz-json.a -o exodus_snapshot -lz
 * (Requires zlib development library: sudo apt-get install zlib1g-dev or similar)
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <libgen.h>
#include <limits.h>
#include <stdarg.h>
#include <ftw.h>
#include <stddef.h>
#include <fcntl.h>
#include <time.h>
#include <utime.h>
#include <sys/time.h>
#include <zlib.h>

// NEW: Include IPC and JSON headers
#include "cortez_ipc.h"
#include "ctz-json.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define HASH_LEN 40
#define HASH_STR_LEN (HASH_LEN + 1)

// For building tree objects in memory
typedef struct TreeEntry {
    char name[NAME_MAX + 1];
    mode_t mode;
    char type; // 'B' (blob), 'T' (tree), 'L' (link)
    char hash[HASH_STR_LEN];
    struct TreeEntry* next;
} TreeEntry;

typedef struct IgnoreEntry {
    char pattern[PATH_MAX];
    size_t pattern_len;
    struct IgnoreEntry* next;
} IgnoreEntry;


// --- Globals ---
// These are now just used by the FTW() callbacks and recursive functions
static char g_node_root_path[PATH_MAX] = {0};
static char g_objects_dir[PATH_MAX] = {0};
static const char* g_version_tag = NULL;
static char g_unlink_root_path[PATH_MAX] = {0};
static IgnoreEntry* g_ignore_list_head = NULL;


// --- Utility Functions ---

// Simple logging wrapper (now prints to stderr)
void log_msg(const char* format, ...) {
    va_list args;
    va_start(args, format);
    fprintf(stderr, "[Snapshot] ");
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    va_end(args);
}

static int run_command_get_output(const char* cmd, char* output_buf, size_t buf_size) {
    FILE* p = popen(cmd, "r");
    if (!p) {
        perror("[Snapshot] popen failed");
        return -1;
    }

    if (fgets(output_buf, buf_size, p) == NULL) {
        if (!feof(p)) { // Avoid error if command produced no output
           log_msg("Command output read error for: %s", cmd);
        }
        pclose(p);
        return -1; // Indicate potential error or no output
    }

    // Remove trailing newline if present
    output_buf[strcspn(output_buf, "\n")] = 0;

    int status = pclose(p);
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1; // Indicate non-zero exit or other error
}


static int get_content_hash(const char* fpath, char* hash_buf) {
    char cmd[PATH_MAX + 10];
    snprintf(cmd, sizeof(cmd), "sha1sum \"%s\"", fpath);
    char output[HASH_STR_LEN + 100]; // Buffer for sha1sum output line

    if (run_command_get_output(cmd, output, sizeof(output)) != 0) {
        log_msg("sha1sum command failed for %s", fpath);
        return -1;
    }

    // sha1sum output is "hash  filename"
    if (sscanf(output, "%40s", hash_buf) != 1) {
         log_msg("Failed to parse sha1sum output: %s", output);
        return -1;
    }
    hash_buf[HASH_LEN] = '\0';
    return 0;
}

// Hash a buffer directly
static int get_buffer_hash(const char* buffer, size_t size, char* hash_buf) {
    // mkstemp template must be a writable string
    char tmp_file_path[] = "/tmp/exodus_hash_XXXXXX";
    int fd = mkstemp(tmp_file_path);
    if (fd == -1) {
        perror("[Snapshot] mkstemp failed");
        return -1;
    }

    if (write(fd, buffer, size) != (ssize_t)size) {
        perror("[Snapshot] Failed to write buffer to temp file");
        close(fd);
        unlink(tmp_file_path);
        return -1;
    }
    
    // We can close the file descriptor now, the file exists
    close(fd);

    int result = get_content_hash(tmp_file_path, hash_buf);
    
    // Clean up the temporary file
    unlink(tmp_file_path);
    return result;
}

// Helper to write a simple string to a file
static int write_string_to_file(const char* fpath, const char* str) {
    char* dir_copy = strdup(fpath);
    if (!dir_copy) return -1;
    char mkdir_cmd[PATH_MAX + 10];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p \"%s\"", dirname(dir_copy));
    system(mkdir_cmd);
    free(dir_copy);

    FILE* f = fopen(fpath, "w");
    if (!f) {
        log_msg("Failed to open file '%s' for writing: %s", fpath, strerror(errno));
        return -1;
    }
    int result = fprintf(f, "%s", str);
    fclose(f);
    return (result < 0) ? -1 : 0;
}

// Helper to read a simple string (hash) from a file
static int read_string_from_file(const char* fpath, char* buf, size_t buf_size) {
    FILE* f = fopen(fpath, "r");
    if (!f) {
        // Don't log error here, might be expected (e.g., no HEAD)
        return -1;
    }
    if (fgets(buf, buf_size, f) == NULL) {
        fclose(f);
        return -1;
    }
    fclose(f);
    buf[strcspn(buf, "\n")] = 0; // Remove newline
    return 0;
}

// Construct the path to an object file from its hash
static void get_object_path(const char* hash, char* path_buf) {
    snprintf(path_buf, PATH_MAX, "%s/%.2s/%s", g_objects_dir, hash, hash + 2);
}

static void free_ignore_list() {
    while (g_ignore_list_head) {
        IgnoreEntry* next = g_ignore_list_head->next;
        free(g_ignore_list_head);
        g_ignore_list_head = next;
    }
}

// NEW: Loads the .retain file from the node's root
static void load_ignore_list(const char* node_path) {
    char retain_file_path[PATH_MAX];
    snprintf(retain_file_path, sizeof(retain_file_path), "%s/.retain", node_path);
    
    FILE* f = fopen(retain_file_path, "r");
    if (!f) {
        return; // .retain is optional, just return if not found
    }

    char line[PATH_MAX];
    while (fgets(line, sizeof(line), f)) {
        // Remove newline
        line[strcspn(line, "\n")] = 0;

        // Trim trailing slashes (e.g., "build/" -> "build")
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '/') {
            line[len - 1] = 0;
            len--;
        }

        // Skip empty lines or comments
        if (len == 0 || line[0] == '#') {
            continue;
        }

        IgnoreEntry* new_entry = malloc(sizeof(IgnoreEntry));
        if (!new_entry) continue; // Out of memory, skip

        strncpy(new_entry->pattern, line, sizeof(new_entry->pattern) - 1);
        new_entry->pattern_len = len;
        new_entry->next = g_ignore_list_head;
        g_ignore_list_head = new_entry;
    }
    
    fclose(f);
}

// NEW: Checks if a relative path should be ignored
static int is_path_ignored(const char* relative_path) {
    // Always ignore .log and .retain itself
    if (strcmp(relative_path, ".log") == 0) return 1;
    if (strcmp(relative_path, ".retain") == 0) return 1;

    for (IgnoreEntry* current = g_ignore_list_head; current; current = current->next) {
        // Check for exact match ("file.txt") or prefix match ("build/foo.c" matches "build")
        if (strncmp(relative_path, current->pattern, current->pattern_len) == 0) {
            // Ensure we're matching the whole name or a directory prefix
            // "foo" should match "foo" and "foo/bar" but not "foobar"
            char next_char = relative_path[current->pattern_len];
            if (next_char == '\0' || next_char == '/') {
                return 1;
            }
        }
    }
    return 0; // Not ignored
}

// Read and decompress an object file
static char* read_object(const char* hash, size_t* uncompressed_size) {
    char obj_path[PATH_MAX];
    get_object_path(hash, obj_path);

    FILE* f = fopen(obj_path, "rb");
    if (!f) {
        // Don't log error here, might be expected during check
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long compressed_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (compressed_size <= 0) {
        fclose(f);
        return NULL;
    }

    Bytef* compressed_buf = malloc(compressed_size);
    if (!compressed_buf) {
        fclose(f);
        return NULL;
    }

    if (fread(compressed_buf, 1, compressed_size, f) != (size_t)compressed_size) {
        fclose(f);
        free(compressed_buf);
        return NULL;
    }
    fclose(f);

    // Assume uncompressed size is at most 10x compressed size (adjust if needed)
    uLongf dest_len = compressed_size * 10;
    if (dest_len < 1000) dest_len = 1000; // Handle very small files
    Bytef* uncompressed_buf = malloc(dest_len);
    if (!uncompressed_buf) {
        free(compressed_buf);
        return NULL;
    }

    int z_result = uncompress(uncompressed_buf, &dest_len, compressed_buf, compressed_size);
    free(compressed_buf);

    if (z_result != Z_OK) {
        log_msg("Zlib uncompress error %d for object %s", z_result, hash);
        free(uncompressed_buf);
        return NULL;
    }

    *uncompressed_size = dest_len;
    return (char*)uncompressed_buf;
}

// Compress and write an object file
static int write_object(const char* hash, const char* content, size_t content_size) {
    char obj_path[PATH_MAX];
    get_object_path(hash, obj_path);

    // Check if object already exists
    struct stat st;
    if (stat(obj_path, &st) == 0) {
        return 0; // Already exists, success
    }

    // Create subdirectory if needed
    char obj_dir[PATH_MAX];
    strncpy(obj_dir, obj_path, sizeof(obj_dir));
    *strrchr(obj_dir, '/') = '\0'; // Find last '/' and terminate string there
    if (mkdir(obj_dir, 0755) != 0 && errno != EEXIST) {
        log_msg("Failed to create object subdir '%s': %s", obj_dir, strerror(errno));
        return -1;
    }

    // Compress
    uLongf compressed_size = compressBound(content_size);
    Bytef* compressed_buf = malloc(compressed_size);
    if (!compressed_buf) {
        return -1;
    }

    int z_result = compress(compressed_buf, &compressed_size, (const Bytef*)content, content_size);
    if (z_result != Z_OK) {
        log_msg("Zlib compress error %d", z_result);
        free(compressed_buf);
        return -1;
    }

    // Write compressed data
    FILE* f = fopen(obj_path, "wb");
    if (!f) {
        log_msg("Failed to open object file '%s' for writing: %s", obj_path, strerror(errno));
        free(compressed_buf);
        return -1;
    }
    if (fwrite(compressed_buf, 1, compressed_size, f) != compressed_size) {
         log_msg("Failed to write object file '%s': %s", obj_path, strerror(errno));
        fclose(f);
        free(compressed_buf);
        unlink(obj_path); // Attempt cleanup
        return -1;
    }
    fclose(f);
    free(compressed_buf);
    return 0;
}

// --- NEW FUNCTION: Generate versions.json ---

static void generate_versions_json(const char* node_path) {
    char tags_dir_path[PATH_MAX];
    char versions_file_path[PATH_MAX];

    snprintf(tags_dir_path, sizeof(tags_dir_path), "%s/.log/refs/tags", node_path);
    snprintf(versions_file_path, sizeof(versions_file_path), "%s/.log/versions.json", node_path);

    DIR* dir = opendir(tags_dir_path);
    if (!dir) {
        log_msg("Could not open tags directory: %s", tags_dir_path);
        return;
    }

    ctz_json_value* root_array = ctz_json_new_array();
    if (!root_array) {
        closedir(dir);
        return;
    }
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char tag_hash[HASH_STR_LEN];
        char tag_file_path[PATH_MAX];
        snprintf(tag_file_path, sizeof(tag_file_path), "%s/%s", tags_dir_path, entry->d_name);
        
        if (read_string_from_file(tag_file_path, tag_hash, sizeof(tag_hash)) != 0) continue;

        // Read the tag object to get the timestamp
        size_t tag_size;
        char* tag_content = read_object(tag_hash, &tag_size);
        long long timestamp = 0;
        if (tag_content) {
            char* tagger_line = strstr(tag_content, "\ntagger ");
            if (!tagger_line) tagger_line = strstr(tag_content, "tagger "); // For start of file
            
            if (tagger_line) {
                char* time_str = strstr(tagger_line, "> ");
                if (time_str) {
                    timestamp = atoll(time_str + 2); // Get time
                }
            }
            free(tag_content);
        }

        ctz_json_value* version_obj = ctz_json_new_object();
        ctz_json_object_set_value(version_obj, "version", ctz_json_new_string(entry->d_name));
        ctz_json_object_set_value(version_obj, "hash", ctz_json_new_string(tag_hash));
        ctz_json_object_set_value(version_obj, "timestamp", ctz_json_new_number((double)timestamp));
        ctz_json_array_push_value(root_array, version_obj);
    }
    closedir(dir);

    char* json_string = ctz_json_stringify(root_array, 1); // Pretty print
    if (json_string) {
        write_string_to_file(versions_file_path, json_string);
        free(json_string);
    }
    ctz_json_free(root_array);
    log_msg("Generated versions.json at %s", versions_file_path);
}


// --- Snapshot Creation (Commit) Logic ---

static void free_tree_list(TreeEntry* head) {
    while (head) {
        TreeEntry* next = head->next;
        free(head);
        head = next;
    }
}

// Recursively builds tree objects bottom-up
// Returns the hash of the tree object created for 'current_path'
// Returns NULL on error
static char* build_tree_recursive(const char* current_path, char* tree_hash_out) {
    DIR* dir = opendir(current_path);
    if (!dir) {
        log_msg("Failed to open dir for tree build: %s", current_path);
        return NULL;
    }

    TreeEntry* head = NULL;
    TreeEntry* tail = NULL;
    struct dirent* entry;

    const char* relative_path_dir = current_path + strlen(g_node_root_path);
    if (*relative_path_dir == '/') relative_path_dir++;
    if (strlen(relative_path_dir) > 0) {
        log_msg("Processing dir: %s", relative_path_dir);
    }

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", current_path, entry->d_name);

        const char* relative_path_entry = full_path + strlen(g_node_root_path);
        if (*relative_path_entry == '/') relative_path_entry++;

        // Skip .log directory at the root
        //if (strcmp(relative_path_entry, ".log") == 0) {
        //    continue;
        //}

        if (is_path_ignored(relative_path_entry)) {
            continue;
        }
        
        struct stat st;
        if (lstat(full_path, &st) == -1) {
            log_msg("lstat failed for %s", full_path);
            continue;
        }

        TreeEntry* new_entry = calloc(1, sizeof(TreeEntry));
        if (!new_entry) goto error_cleanup;
        strncpy(new_entry->name, entry->d_name, NAME_MAX);

        if (S_ISDIR(st.st_mode)) {
            new_entry->type = 'T';
            new_entry->mode = st.st_mode;
            if (build_tree_recursive(full_path, new_entry->hash) == NULL) {
                free(new_entry);
                goto error_cleanup; // Propagate error
            }
        } else if (S_ISREG(st.st_mode)) {
            new_entry->type = 'B';
            new_entry->mode = st.st_mode;
            if (get_content_hash(full_path, new_entry->hash) != 0) {
                log_msg("Failed to hash blob: %s", full_path);
                free(new_entry);
                continue; // Skip this file, try others
            }
            // Read, compress, and write the blob object
            FILE* f = fopen(full_path, "rb");
            if (!f) continue;
            fseek(f, 0, SEEK_END);
            long fsize = ftell(f);
            fseek(f, 0, SEEK_SET);
            char *content = malloc(fsize);
            if (!content) { fclose(f); continue; }

            if (fread(content, 1, fsize, f) != (size_t)fsize) {
                log_msg("Failed to read all content from %s", full_path);
                fclose(f);
                free(content);
                continue;
            }
            
            fclose(f);
            write_object(new_entry->hash, content, fsize);
            free(content);

        } else if (S_ISLNK(st.st_mode)) {
            new_entry->type = 'L';
            new_entry->mode = st.st_mode;
            char target[PATH_MAX];
            ssize_t len = readlink(full_path, target, sizeof(target)-1);
            if (len == -1) { free(new_entry); continue; }
            target[len] = '\0';
            if (get_buffer_hash(target, len, new_entry->hash) != 0) {
                log_msg("Failed to hash symlink target: %s", full_path);
                free(new_entry);
                continue;
            }
            // Write link target as a blob
            write_object(new_entry->hash, target, len);

        } else {
            free(new_entry); // Skip unsupported types
            continue;
        }

        // Add to linked list
        if (tail) {
            tail->next = new_entry;
        } else {
            head = new_entry;
        }
        tail = new_entry;
    }
    closedir(dir);

    // Build the text representation of the tree
    size_t tree_content_size = 0;
    TreeEntry* current = head;
    while (current) {
        // Format: "mode type hash\tname\n"
        tree_content_size += snprintf(NULL, 0, "%o %c %s\t%s\n",
                                      current->mode & 07777, // Permissions + type bits
                                      current->type, current->hash, current->name);
        current = current->next;
    }

    if (tree_content_size == 0) {
        // Handle empty directory
        tree_content_size = 1; // For null terminator
    }
    
    char* tree_content = malloc(tree_content_size + 1);
    if (!tree_content) goto error_cleanup;
    char* ptr = tree_content;
    current = head;
    while (current) {
        ptr += sprintf(ptr, "%o %c %s\t%s\n",
                       current->mode & 07777, current->type, current->hash, current->name);
        current = current->next;
    }
    *ptr = '\0'; // Null-terminate

    // Hash the tree content itself
    if (get_buffer_hash(tree_content, tree_content_size, tree_hash_out) != 0) {
        log_msg("Failed to hash tree content for: %s", current_path);
        free(tree_content);
        goto error_cleanup;
    }

    // Write the tree object
    if (write_object(tree_hash_out, tree_content, tree_content_size) != 0) {
         log_msg("Failed to write tree object: %s", tree_hash_out);
        free(tree_content);
        goto error_cleanup;
    }

    free(tree_content);
    free_tree_list(head);
    return tree_hash_out; // Success

error_cleanup:
    closedir(dir); // Ensure closed even if loop broke early
    free_tree_list(head);
    return NULL; // Indicate error
}

// MODIFIED: Takes parameters instead of a struct
static void execute_commit_job(const char* node_name, const char* node_path, const char* version_tag) {
    (void)node_name; // node_name is not strictly needed here, but good for context
    char head_file[PATH_MAX];
    char tag_file[PATH_MAX];

    snprintf(g_objects_dir, sizeof(g_objects_dir), "%s/.log/objects", node_path);
    snprintf(head_file, sizeof(head_file), "%s/.log/HEAD", node_path);
    snprintf(tag_file, sizeof(tag_file), "%s/.log/refs/tags/%s", node_path, version_tag);

    log_msg("Initializing object database...");

    load_ignore_list(node_path);

    char mkdir_cmd_obj[PATH_MAX + 10];
    snprintf(mkdir_cmd_obj, sizeof(mkdir_cmd_obj), "mkdir -p \"%s\"", g_objects_dir);
    if (system(mkdir_cmd_obj) != 0) {// Ignore output
        log_msg("Error: Could not create object directory.");
        return;
    }
    char* tag_dir_copy = strdup(tag_file);
    if (tag_dir_copy) {
        char mkdir_cmd_tags[PATH_MAX + 10];
        snprintf(mkdir_cmd_tags, sizeof(mkdir_cmd_tags), "mkdir -p \"%s\"", dirname(tag_dir_copy));
        system(mkdir_cmd_tags);
        free(tag_dir_copy);
    }

    log_msg("Hashing node to version '%s'...", version_tag);

    strncpy(g_node_root_path, node_path, sizeof(g_node_root_path)-1);
    g_version_tag = version_tag; // For logging

    char root_tree_hash[HASH_STR_LEN];
    if (build_tree_recursive(node_path, root_tree_hash) == NULL) {
        log_msg("Error: Failed to build root tree.");
        g_node_root_path[0] = '\0';
        g_version_tag = NULL;
        free_ignore_list();
        return; // Exit on failure
    }

    log_msg("Creating commit object...");

    // 1. Get parent commit hash from HEAD
    char parent_commit_hash[HASH_STR_LEN] = {0};
    read_string_from_file(head_file, parent_commit_hash, sizeof(parent_commit_hash)); // Fails silently on first commit, which is fine

    // 2. Create commit object content
    char commit_content[PATH_MAX * 2]; // Buffer for commit data
    long long now = (long long)time(NULL);
    int content_len;

    if (parent_commit_hash[0] == '\0') {
        // First commit (no parent)
        content_len = snprintf(commit_content, sizeof(commit_content),
                               "tree %s\n"
                               "author Exodus <exodus@localhost> %lld +0000\n"
                               "committer Exodus <exodus@localhost> %lld +0000\n"
                               "\n"
                               "%s\n",
                               root_tree_hash, now, now, version_tag);
    } else {
        // Subsequent commits
        content_len = snprintf(commit_content, sizeof(commit_content),
                               "tree %s\n"
                               "parent %s\n"
                               "author Exodus <exodus@localhost> %lld +0000\n"
                               "committer Exodus <exodus@localhost> %lld +0000\n"
                               "\n"
                               "%s\n",
                               root_tree_hash, parent_commit_hash, now, now, version_tag);
    }

    // 3. Hash and write commit object
    char new_commit_hash[HASH_STR_LEN];
    if (get_buffer_hash(commit_content, content_len, new_commit_hash) != 0) {
        log_msg("Error: Failed to hash commit object.");
        g_node_root_path[0] = '\0'; g_version_tag = NULL; return;
    }
    if (write_object(new_commit_hash, commit_content, content_len) != 0) {
        log_msg("Error: Failed to write commit object.");
        g_node_root_path[0] = '\0'; g_version_tag = NULL; return;
    }

    // 4. Create annotated tag object content
    char tag_content[PATH_MAX * 2];
    content_len = snprintf(tag_content, sizeof(tag_content),
                           "object %s\n"
                           "type commit\n"
                           "tag %s\n"
                           "tagger Exodus <exodus@localhost> %lld +0000\n"
                           "\n"
                           "Tag for version %s\n",
                           new_commit_hash, version_tag, now, version_tag);

    // 5. Hash and write tag object
    char new_tag_hash[HASH_STR_LEN];
    if (get_buffer_hash(tag_content, content_len, new_tag_hash) != 0) {
        log_msg("Error: Failed to hash tag object.");
        g_node_root_path[0] = '\0'; g_version_tag = NULL; return;
    }
    if (write_object(new_tag_hash, tag_content, content_len) != 0) {
        log_msg("Error: Failed to write tag object.");
        g_node_root_path[0] = '\0'; g_version_tag = NULL; return;
    }

    // 6. Write the reference files
    log_msg("Updating references...");
    write_string_to_file(head_file, new_commit_hash); // HEAD points to the commit
    write_string_to_file(tag_file, new_tag_hash);      // Tag file points to the tag object
    
    // 7. NEW: Generate the versions.json file
    log_msg("Generating versions.json...");
    generate_versions_json(node_path);

    // 8. Clear the persistent history log, as these changes are now committed.
    // This allows 'node-status' to use history.json to show uncommitted changes.
    log_msg("Clearing node activity log (history.json)...");
    char history_file_path[PATH_MAX];
    snprintf(history_file_path, sizeof(history_file_path), "%s/.log/history.json", node_path);
    FILE* f = fopen(history_file_path, "w");
    if (f) {
        fprintf(f, "[]\n"); // Write an empty JSON array
        fclose(f);
    } else {
        log_msg("Warning: Could not clear history.json at %s", history_file_path);
    }

    log_msg("Snapshot commit complete.");

    
    g_node_root_path[0] = '\0';
    g_version_tag = NULL;
    free_ignore_list();
}

// --- Snapshot Restoration (Build) Logic ---

static int unlink_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)sb; (void)ftwbuf;
    if (strstr(fpath, "/.log") != NULL || strcmp(fpath, g_unlink_root_path) == 0) {
        return 0;
    }
    if (typeflag == FTW_F || typeflag == FTW_SL || typeflag == FTW_DP) {
        int rv = remove(fpath);
        if (rv) log_msg("Failed to remove '%s': %s", fpath, strerror(errno));
        return rv;
    }
    return 0;
}

// Recursively unpacks tree objects and creates files/dirs
static int unpack_tree_recursive(const char* tree_hash, const char* current_dest_path) {
    size_t tree_size;
    char* tree_content = read_object(tree_hash, &tree_size);
    if (!tree_content) {
        log_msg("Failed to read tree object: %s", tree_hash);
        return -1;
    }

    char* line = tree_content;
    char* next_line;

    while (line < tree_content + tree_size) {
        next_line = strchr(line, '\n');
        if (next_line) *next_line = '\0'; // Temporarily null-terminate

        char* saveptr;
        char* mode_str = strtok_r(line, " \t", &saveptr);
        char* type_str = strtok_r(NULL, " \t", &saveptr);
        char* hash_str = strtok_r(NULL, " \t", &saveptr);
        char* name_str = strtok_r(NULL, "", &saveptr); // Rest is name

        if (!mode_str || !type_str || !hash_str || !name_str) {
             log_msg("Malformed tree line in object %s: %s", tree_hash, line);
            if (next_line) line = next_line + 1; else break;
            continue;
        }

        mode_t mode = (mode_t)strtol(mode_str, NULL, 8);
        char entry_dest_path[PATH_MAX];
        snprintf(entry_dest_path, sizeof(entry_dest_path), "%s/%s", current_dest_path, name_str);

        const char* relative_path_entry = entry_dest_path + strlen(g_node_root_path);
         if (*relative_path_entry == '/') relative_path_entry++;

        switch (type_str[0]) {
            case 'T': // Tree
                log_msg("Creating dir: %s", relative_path_entry);
                if (mkdir(entry_dest_path, mode | 0700) != 0 && errno != EEXIST) { // Ensure write perm for recursion
                    log_msg("Failed to create dir '%s': %s", entry_dest_path, strerror(errno));
                } else {
                    if (unpack_tree_recursive(hash_str, entry_dest_path) != 0) {
                        free(tree_content); return -1; // Propagate error
                    }
                    // Apply final mode after contents are written
                    if (chmod(entry_dest_path, mode) != 0) {
                         log_msg("Failed to chmod dir '%s': %s", entry_dest_path, strerror(errno));
                    }
                }
                break;

            case 'B': // Blob
                log_msg("Restoring file: %s", relative_path_entry);
                size_t blob_size;
                char* blob_content = read_object(hash_str, &blob_size);
                if (!blob_content) {
                     log_msg("Failed to read blob object %s for file %s", hash_str, name_str);
                    break;
                }
                FILE* f = fopen(entry_dest_path, "wb");
                if (!f) {
                     log_msg("Failed to open file '%s' for writing: %s", entry_dest_path, strerror(errno));
                    free(blob_content); break;
                }
                if (fwrite(blob_content, 1, blob_size, f) != blob_size) {
                    log_msg("Failed to write file '%s': %s", entry_dest_path, strerror(errno));
                }
                fclose(f);
                free(blob_content);
                if (chmod(entry_dest_path, mode) != 0) {
                     log_msg("Failed to chmod file '%s': %s", entry_dest_path, strerror(errno));
                }
                break;

             case 'L': // Link
                log_msg("Restoring link: %s", relative_path_entry);
                size_t target_size;
                char* target_content = read_object(hash_str, &target_size);
                 if (!target_content) {
                     log_msg("Failed to read link object %s for link %s", hash_str, name_str);
                    break;
                }
                // Ensure null termination for safety
                char* target_path = malloc(target_size + 1);
                if (!target_path) { free(target_content); break; }
                memcpy(target_path, target_content, target_size);
                target_path[target_size] = '\0';
                free(target_content);

                if (symlink(target_path, entry_dest_path) != 0) {
                     log_msg("Failed to create symlink '%s' -> '%s': %s", entry_dest_path, target_path, strerror(errno));
                }
                free(target_path);
                break;
        }

        if (next_line) {
            line = next_line + 1;
        } else {
            break; // End of buffer
        }
    }

    free(tree_content);
    return 0; // Success
}

// MODIFIED: Takes parameters instead of a struct
static void execute_rebuild_job(const char* node_name, const char* node_path, const char* version_tag) {
    (void)node_name;
    char head_file[PATH_MAX];
    char tag_file[PATH_MAX];
    snprintf(g_objects_dir, sizeof(g_objects_dir), "%s/.log/objects", node_path);
    snprintf(head_file, sizeof(head_file), "%s/.log/HEAD", node_path);
    snprintf(tag_file, sizeof(tag_file), "%s/.log/refs/tags/%s", node_path, version_tag);

    char root_tree_hash[HASH_STR_LEN] = {0};
    char commit_hash[HASH_STR_LEN] = {0};
    char tag_object_hash[HASH_STR_LEN] = {0};
    char* ptr;

    // Step 1: Read tag file to get tag object hash
    if (read_string_from_file(tag_file, tag_object_hash, sizeof(tag_object_hash)) != 0) {
        log_msg("Error: Snapshot version (tag) '%s' not found.", version_tag);
        return;
    }

    // Step 2: Read tag object to get commit hash
    size_t tag_size;
    char* tag_content = read_object(tag_object_hash, &tag_size);
    if (!tag_content) {
        log_msg("Error: Failed to read tag object: %s", tag_object_hash);
        return;
    }
    ptr = strstr(tag_content, "object ");
    if (!ptr || sscanf(ptr, "object %40s", commit_hash) != 1) {
        log_msg("Error: Corrupt tag object. Could not find commit hash.");
        free(tag_content); return;
    }
    free(tag_content);

    // Step 3: Read commit object to get tree hash
    size_t commit_size;
    char* commit_content = read_object(commit_hash, &commit_size);
    if (!commit_content) {
        log_msg("Error: Failed to read commit object: %s", commit_hash);
        return;
    }
    ptr = strstr(commit_content, "tree ");
    if (!ptr || sscanf(ptr, "tree %40s", root_tree_hash) != 1) {
        log_msg("Error: Corrupt commit object. Could not find tree hash.");
        free(commit_content); return;
    }
    free(commit_content);

    log_msg("Clearing current node contents...");
    strncpy(g_unlink_root_path, node_path, sizeof(g_unlink_root_path) - 1);
    nftw(node_path, unlink_cb, 64, FTW_DEPTH | FTW_PHYS);

    log_msg("Restoring node from version '%s'...", version_tag);

    strncpy(g_node_root_path, node_path, sizeof(g_node_root_path)-1);
    g_version_tag = version_tag; // For logging

    if (unpack_tree_recursive(root_tree_hash, node_path) != 0) {
         log_msg("Error: Failed during tree unpack.");
    } else {
        // Update HEAD to the commit we just built
        write_string_to_file(head_file, commit_hash);
        log_msg("Rebuild complete.");
    }

    g_node_root_path[0] = '\0';
    g_version_tag = NULL;
}


// --- NEW Main Function ---

int main(int argc, char *argv[]) {
    log_msg("exodus_snapshot starting...");

    CortezIPCData* data_head = cortez_ipc_receive(argc, argv);
    if (!data_head) {
        log_msg("Failed to receive IPC data. Tool must be run by 'exodus' client.");
        return 1;
    }

    // Expecting: [0]=command, [1]=node_name, [2]=node_path, [3]=version_tag
    const char* command = NULL;
    const char* node_name = NULL;
    const char* node_path = NULL;
    const char* version_tag = NULL;

    CortezIPCData* current = data_head;
    if (current && current->type == CORTEZ_TYPE_STRING) {
        command = current->data.string_val;
        current = current->next;
    }
    if (current && current->type == CORTEZ_TYPE_STRING) {
        node_name = current->data.string_val;
        current = current->next;
    }
    if (current && current->type == CORTEZ_TYPE_STRING) {
        node_path = current->data.string_val;
        current = current->next;
    }
    if (current && current->type == CORTEZ_TYPE_STRING) {
        version_tag = current->data.string_val;
        current = current->next;
    }

    if (!command || !node_name || !node_path || !version_tag) {
        log_msg("Received malformed IPC data. Missing arguments.");
        cortez_ipc_free_data(data_head);
        return 1;
    }

    log_msg("Command: %s", command);
    log_msg("Node: %s", node_name);
    log_msg("Path: %s", node_path);
    log_msg("Tag: %s", version_tag);

    if (strcmp(command, "commit") == 0) {
        execute_commit_job(node_name, node_path, version_tag);
    } else if (strcmp(command, "rebuild") == 0) {
        execute_rebuild_job(node_name, node_path, version_tag);
    } else {
        log_msg("Unknown command: %s", command);
    }

    cortez_ipc_free_data(data_head);
    log_msg("exodus_snapshot finished.");
    return 0;
}