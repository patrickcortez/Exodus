/*
 * exodus-tui.c (Node Explorer)
 *
 * COMPILE (with ctz-json.c):
 * gcc -Wall -Wextra -O2 -o exodus-tui exodus-tui.c ctz-json.a -lncursesw
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ncurses.h>
#include <locale.h>
#include <unistd.h>     
#include <sys/stat.h>
#include <sys/wait.h>   
#include <dirent.h>
#include <libgen.h>     
#include <ctype.h>
#include "ctz-json.h" 

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

// --- Global State ---
int running = 1;
char status_message[256] = "Welcome to Exodus TUI! Press Ctrl+X to exit.";
char config_file_path[PATH_MAX] = {0};

// --- Data Structures ---

typedef struct FileNode {
    char *name;
    char path[PATH_MAX];
    char relative_path[PATH_MAX];
    int is_dir;
    int is_open;
    int depth;
    struct FileNode *parent;
    struct FileNode *children;
    struct FileNode *next;
    struct FileNode *prev;
} FileNode;

// --- Window & Pane Management ---
WINDOW *tree_border_win, *tree_win;
int tree_scroll_top = 0;
int tree_item_count = 0;

// --- Tree State ---
FileNode *tree_root = NULL;
FileNode *selected_node = NULL;

typedef enum {
    NET_STATE_NONE,
    NET_STATE_CREATED,
    NET_STATE_MODIFIED,
    NET_STATE_DELETED,
    NET_STATE_TEMP_DELETED,
    NET_STATE_MOVED
} FileNetState;

typedef struct FileStatusNode {
    char path[PATH_MAX];
    FileNetState state;
    int modify_count;
    struct FileStatusNode* next;
    char from_path[PATH_MAX];
} FileStatusNode;

FileStatusNode* g_status_head = NULL;

// --- Function Declarations ---
void init_ncurses();
void create_windows();
void destroy_windows();
void main_loop();
void draw_layout();
void draw_tree();
void draw_status_bar();
void handle_input(int ch);
void launch_editor(const char *file_path);

// Helper functions
int get_executable_dir(char* buffer, size_t size);
int get_config_path(char* buffer, size_t size);

// Tree functions
FileNode *create_node(const char *name, const char *path, const char *relative_path, int is_dir, int depth, FileNode *parent);
void free_tree(FileNode *node);
void load_file_tree();
void load_directory_children(FileNode *parent_node);
FileNode *get_next_visible_node(FileNode *node);
FileNode *get_prev_visible_node(FileNode *node);
void free_status_map();
void load_node_status(const char* node_path);
FileNetState get_status_for_node(FileNode* node);

/*
 * ===================================================================
 * MAIN APPLICATION LOGIC
 * ===================================================================
 */

int main() {
    setlocale(LC_ALL, "");
    init_ncurses();
    create_windows();

    if (get_config_path(config_file_path, sizeof(config_file_path)) != 0) {
        snprintf(status_message, sizeof(status_message), "ERROR: Could not find nodewatch.json path.");
    } else {
        load_file_tree();
    }

    if (tree_root && tree_root->children) {
        selected_node = tree_root->children;
    }

    main_loop();

    destroy_windows();
    endwin();
    free_tree(tree_root);
    return 0;
}

void init_ncurses() {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    start_color();
    use_default_colors();
    curs_set(0); // Hide cursor in explorer
    init_pair(1, COLOR_CYAN, -1);
    init_pair(2, COLOR_WHITE, -1);
    init_pair(3, COLOR_BLACK, COLOR_WHITE);
    init_pair(4, COLOR_GREEN, -1);  
    init_pair(5, COLOR_YELLOW, -1);
}

void create_windows() {
    int height, width;
    getmaxyx(stdscr, height, width);
    tree_border_win = newwin(height - 1, width, 0, 0);
    tree_win = newwin(height - 3, width - 2, 1, 1);
}

void destroy_windows() {
    delwin(tree_win);
    delwin(tree_border_win);
}

void main_loop() {
    while (running) {
        draw_layout();
        int ch = getch();
        handle_input(ch);
    }
}

/*
 * ===================================================================
 * DRAWING FUNCTIONS
 * ===================================================================
 */

void draw_layout() {
    werase(tree_border_win);
    wattron(tree_border_win, COLOR_PAIR(1));
    box(tree_border_win, 0, 0);
    mvwprintw(tree_border_win, 0, 2, " Exodus Node Explorer ");
    wattroff(tree_border_win, COLOR_PAIR(1));

    draw_tree();
    draw_status_bar();

    wnoutrefresh(stdscr);
    wnoutrefresh(tree_border_win);
    wnoutrefresh(tree_win);
    doupdate();
}

void draw_tree() {
    wclear(tree_win);
    int y = 0;
    
    FileNode *temp_node = tree_root ? tree_root->children : NULL;
    tree_item_count = 0;
    while (temp_node) {
        tree_item_count++;
        temp_node = get_next_visible_node(temp_node);
    }

    FileNode *node = tree_root ? tree_root->children : NULL;
    for (int i = 0; i < tree_scroll_top && node; i++) {
        node = get_next_visible_node(node);
    }

    while (node != NULL && y < getmaxy(tree_win)) {

        FileNetState state = get_status_for_node(node);
        int color_pair = 2; // Default (white)
        if (state == NET_STATE_CREATED || state == NET_STATE_MOVED) {
            color_pair = 4; // Green
        } else if (state == NET_STATE_MODIFIED) {
            color_pair = 5; // Yellow
        }

        if (node == selected_node) {
            wattron(tree_win, COLOR_PAIR(3));
        } else {
            wattron(tree_win, COLOR_PAIR(color_pair));
        }

        char indent[256] = {0};
        for (int i = 0; i < node->depth; i++) strcat(indent, "  ");
        const char *prefix = node->is_dir ? (node->is_open ? "[-] " : "[+] ") : "    ";
        char line[PATH_MAX];
        snprintf(line, sizeof(line), "%s%s%s", indent, prefix, node->name);
        line[getmaxx(tree_win)] = '\0';
        mvwprintw(tree_win, y, 0, "%s", line);

        if (node == selected_node) {
            int len = strlen(line);
            for (int i = len; i < getmaxx(tree_win); i++) mvwaddch(tree_win, y, i, ' ');
            wattroff(tree_win, COLOR_PAIR(3));
        }else {
            wattroff(tree_win, COLOR_PAIR(color_pair));
        }
        y++;
        node = get_next_visible_node(node);
    }
}

void draw_status_bar() {
    int height, width;
    getmaxyx(stdscr, height, width);
    attron(A_REVERSE);
    
    char help_text[256];
    snprintf(help_text, sizeof(help_text), " (Enter: Open/Select | Ctrl+X: Exit) ");
    
    mvprintw(height - 1, 0, "%s", help_text);
    int len = strlen(help_text);
    for (int i = len; i < width; i++) addch(' ');
    attroff(A_REVERSE);
    
    mvprintw(height - 1, width - strlen(status_message) - 2, "%s", status_message);
}

/*
 * ===================================================================
 * INPUT HANDLING
 * ===================================================================
 */

char* get_input_from_footer(const char* prompt_format, const char* arg) {
    static char buffer[512];
    memset(buffer, 0, sizeof(buffer));
    int pos = 0;
    
    int height, width;
    getmaxyx(stdscr, height, width);
    
    // Show cursor, enable echo
    curs_set(1);
    echo();

    int ch;
    do {
        // Draw the prompt
        attron(A_REVERSE);
        mvprintw(height - 1, 0, " ");
        for (int i = 1; i < width; i++) addch(' ');
        mvprintw(height - 1, 1, prompt_format, arg);
        printw("%s", buffer);
        attroff(A_REVERSE);
        
        refresh();
        
        ch = getch();
        
        if (isprint(ch)) {
            if (pos < (int)sizeof(buffer) - 1) {
                buffer[pos++] = ch;
                buffer[pos] = '\0';
            }
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (pos > 0) {
                buffer[--pos] = '\0';
            }
        }
    } while (ch != '\n' && ch != KEY_ENTER);
    
    // Hide cursor, disable echo
    curs_set(0);
    noecho();
    
    if (strlen(buffer) == 0) return NULL; // User cancelled
    return strdup(buffer);
}

void handle_input(int ch) {
    snprintf(status_message, sizeof(status_message), " ");
    int tree_height = getmaxy(tree_win);
    
    switch (ch) {
        case 'x' & 0x1F: // Ctrl+X
            running = 0;
            break;

        case 'c':
            if (selected_node && selected_node->depth == 0) {
                // We can only commit a top-level node
                char* commit_msg = get_input_from_footer("Commit message for '%s': ", selected_node->name);
                
                if (commit_msg && commit_msg[0] != '\0') {
                    // Suspend ncurses
                    def_prog_mode();
                    endwin();
                    
                    char exe_dir[PATH_MAX];
                    char exodus_path[PATH_MAX];
                    get_executable_dir(exe_dir, sizeof(exe_dir));
                    snprintf(exodus_path, sizeof(exodus_path), "%s/exodus", exe_dir);

                    pid_t pid = fork();
                    if (pid == 0) { // Child
                        printf("Running 'exodus commit %s \"%s\"'...\n", selected_node->name, commit_msg);
                        execlp(exodus_path, "exodus", "commit", selected_node->name, commit_msg, (char*)NULL);
                        perror("execlp failed");
                        exit(1);
                    } else if (pid > 0) { // Parent
                        int status;
                        waitpid(pid, &status, 0);
                        printf("\n...Commit process finished. Press Enter to return to TUI.");
                        getchar(); // Wait for user
                    }
                    
                    // Resume ncurses
                    reset_prog_mode();
                    refresh();
                    
                    // After commit, reload status (it should be empty)
                    load_node_status(selected_node->path);
                    snprintf(status_message, sizeof(status_message), "Commit complete for %s!", selected_node->name);
                    
                    free(commit_msg);
                } else {
                    snprintf(status_message, sizeof(status_message), "Commit cancelled.");
                }
            } else {
                snprintf(status_message, sizeof(status_message), "ERROR: Can only commit top-level nodes.");
            }
            break;

        case KEY_UP:
            if (selected_node) {
                FileNode *prev = get_prev_visible_node(selected_node);
                if (prev) {
                    selected_node = prev;
                    int selected_idx = 0;
                    FileNode *n = tree_root ? tree_root->children : NULL;
                    while(n) {
                        if (n == selected_node) break;
                        n = get_next_visible_node(n);
                        selected_idx++;
                    }
                    if (selected_idx < tree_scroll_top) {
                        tree_scroll_top = selected_idx;
                    }
                }
            }
            break;

        case KEY_DOWN:
            if (selected_node) {
                FileNode *next = get_next_visible_node(selected_node);
                if (next) {
                    selected_node = next;
                    int selected_idx = 0;
                    FileNode *n = tree_root ? tree_root->children : NULL;
                    while(n) {
                        if (n == selected_node) break;
                        n = get_next_visible_node(n);
                        selected_idx++;
                    }
                    if (selected_idx >= tree_scroll_top + tree_height) {
                        tree_scroll_top = selected_idx - tree_height + 1;
                    }
                }
            }
            break;

        case '\n': // Enter key
        case KEY_ENTER:
            if (selected_node) {
                if (selected_node->is_dir) {
                    if (!selected_node->is_open) {

                        if (selected_node->depth == 0) {
                            load_node_status(selected_node->path);
                        }

                        if (selected_node->children && strcmp(selected_node->children->name, "dummy") == 0) {
                            load_directory_children(selected_node);
                        }
                    }else{
                        if (selected_node->depth == 0) {
                            free_status_map();
                        }
                    }
                    selected_node->is_open = !selected_node->is_open;
                } else {
                    launch_editor(selected_node->path);
                }
            }
            break;
    }
}

/**
 * @brief Suspends ncurses, forks, and execs the node-editor.
 * Waits for the editor to exit, then resumes ncurses.
 */
void launch_editor(const char *file_path) {
    // 1. Suspend ncurses
    def_prog_mode(); // Save terminal settings
    endwin();        // Temporarily stop ncurses
    
    // 2. Fork and Exec
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        
    } else if (pid == 0) {
        // --- Child Process ---
        char exe_dir[PATH_MAX];
        char editor_path[PATH_MAX];
        get_executable_dir(exe_dir, sizeof(exe_dir));
        snprintf(editor_path, sizeof(editor_path), "%s/node-editor", exe_dir);

        char *args[] = {"node-editor", (char*)file_path, NULL};
        
        execvp(editor_path, args);
        
        perror("execvp");
        printf("Error: Could not launch '%s'\n", editor_path);
        printf("Make sure 'node-editor' is compiled and in the same directory.\n");
        printf("Press Enter to continue...");
        getchar();
        exit(1);
        
    } else {
        // --- Parent Process ---
        int status;
        waitpid(pid, &status, 0);
    }
    
    // 3. Resume ncurses
    reset_prog_mode();
    refresh();
}


/*
 * ===================================================================
 * HELPER & FILE TREE LOGIC
 * ===================================================================
 */

 void free_status_map() {
    FileStatusNode* current = g_status_head;
    while (current) {
        FileStatusNode* next = current->next;
        free(current);
        current = next;
    }
    g_status_head = NULL;
}

FileStatusNode* find_or_create_status(FileStatusNode** head, const char* path) {
    FileStatusNode* current = *head;
    while (current) {
        if (strcmp(current->path, path) == 0) {
            return current;
        }
        current = current->next;
    }
    
    FileStatusNode* new_node = (FileStatusNode*)calloc(1, sizeof(FileStatusNode));
    if (!new_node) {
        snprintf(status_message, sizeof(status_message), "ERROR: Out of memory!");
        return NULL; 
    }
    strncpy(new_node->path, path, PATH_MAX - 1);
    new_node->state = NET_STATE_NONE;
    new_node->next = *head;
    *head = new_node;
    return new_node;
}

void load_node_status(const char* node_path) {
    free_status_map(); // Clear previous status
    
    char history_path[PATH_MAX];
    snprintf(history_path, sizeof(history_path), "%s/.log/history.json", node_path);

    char error_buf[256];
    ctz_json_value* root = ctz_json_load_file(history_path, error_buf, sizeof(error_buf));
    if (!root || ctz_json_get_type(root) != CTZ_JSON_ARRAY) {
        if (root) ctz_json_free(root);
        return; // No history or corrupt, just return
    }
    
    size_t change_count = ctz_json_get_array_size(root);
    for (size_t i = 0; i < change_count; i++) {
        ctz_json_value* item = ctz_json_get_array_element(root, i);
        ctz_json_value* event_val = ctz_json_find_object_value(item, "event");
        ctz_json_value* name_val = ctz_json_find_object_value(item, "name");
        if (!event_val || !name_val) continue;

        const char* event_str = ctz_json_get_string(event_val);
        const char* name_str = ctz_json_get_string(name_val);

        FileStatusNode* node = find_or_create_status(&g_status_head, name_str);
        if (!node) continue; 

        if (strcmp(event_str, "Created") == 0) {
            if (node->state != NET_STATE_CREATED) {
                node->state = NET_STATE_CREATED;
                node->modify_count = 0;
            }
        } else if (strcmp(event_str, "Modified") == 0) {
            if (node->state == NET_STATE_NONE) {
                node->state = NET_STATE_MODIFIED;
            }
            if (node->state != NET_STATE_DELETED) {
                node->modify_count++;
            }
        } else if (strcmp(event_str, "Deleted") == 0) {
            if (node->state == NET_STATE_CREATED) {
                node->state = NET_STATE_TEMP_DELETED;
            } else {
                node->state = NET_STATE_DELETED;
            }
            node->modify_count = 0;
        } else if (strcmp(event_str, "Moved") == 0) {
            ctz_json_value* changes_val = ctz_json_find_object_value(item, "changes");
            if (!changes_val) continue;
            ctz_json_value* from_val = ctz_json_find_object_value(changes_val, "from");
            if (!from_val) continue;
            const char* from_path_str = ctz_json_get_string(from_val);
            if (!from_path_str) continue;

            FileStatusNode* from_node = find_or_create_status(&g_status_head, from_path_str);
            if (from_node) {
                if (from_node->state == NET_STATE_CREATED || from_node->state == NET_STATE_MOVED) {
                    from_node->state = NET_STATE_TEMP_DELETED;
                } else {
                    from_node->state = NET_STATE_DELETED;
                }
                from_node->modify_count = 0;
            }
            node->state = NET_STATE_MOVED;
            strncpy(node->from_path, from_path_str, sizeof(node->from_path) - 1);
            node->modify_count = 0;
        }
    }
    ctz_json_free(root);
}

FileNetState get_status_for_node(FileNode* node) {
    if (!g_status_head || !node || node->relative_path[0] == '\0') {
        return NET_STATE_NONE;
    }
    
    FileStatusNode* current = g_status_head;
    while (current) {
        if (strcmp(current->path, node->relative_path) == 0) {
            return current->state;
        }
        current = current->next;
    }
    return NET_STATE_NONE;
}

int get_executable_dir(char* buffer, size_t size) {
    char path_buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", path_buf, sizeof(path_buf) - 1);
    if (len == -1) {
        perror("readlink failed");
        return -1;
    }
    path_buf[len] = '\0';
    char* dir = dirname(path_buf);
    if (dir == NULL) {
        perror("dirname failed");
        return -1;
    }
    strncpy(buffer, dir, size - 1);
    buffer[size - 1] = '\0';
    return 0;
}

int get_config_path(char* buffer, size_t size) {
    char exe_dir[PATH_MAX];
    if (get_executable_dir(exe_dir, sizeof(exe_dir)) != 0) {
        fprintf(stderr, "Error: Could not determine executable directory.\n");
        return -1;
    }
    snprintf(buffer, size, "%s/nodewatch.json", exe_dir);
    return 0;
}

FileNode *create_node(const char *name, const char *path, const char *relative_path, int is_dir, int depth, FileNode *parent) {
    FileNode *node = (FileNode *)malloc(sizeof(FileNode));
    if (!node) return NULL;
    node->name = strdup(name);
    if (!node->name) { free(node); return NULL; }
    strncpy(node->path, path, PATH_MAX - 1);
    strncpy(node->relative_path, relative_path, PATH_MAX - 1);
    node->is_dir = is_dir;
    node->is_open = 0;
    node->depth = depth;
    node->parent = parent;
    node->children = NULL;
    node->next = NULL;
    node->prev = NULL;
    return node;
}

void free_tree(FileNode *node) {
    if (node == NULL) return;
    free_tree(node->children);
    free_tree(node->next);
    free(node->name);
    free(node);
}

void load_file_tree() {
    free_tree(tree_root);
    tree_root = create_node("ROOT", "/", "", 1, -1, NULL);
    tree_root->is_open = 1;
    
    char error_buf[256];
    ctz_json_value* root = ctz_json_load_file(config_file_path, error_buf, sizeof(error_buf));
    
    if (!root) {
        snprintf(status_message, sizeof(status_message), "ERROR: %s", error_buf);
        return;
    }
    if (ctz_json_get_type(root) != CTZ_JSON_OBJECT) {
        snprintf(status_message, sizeof(status_message), "ERROR: nodewatch.json is not a JSON object.");
        ctz_json_free(root);
        return;
    }

    FileNode *current_node = NULL;
    size_t node_count = ctz_json_get_object_size(root);
    
    for (size_t i = 0; i < node_count; i++) {
        const char* node_name = ctz_json_get_object_key(root, i);
        ctz_json_value* node_obj = ctz_json_get_object_value(root, i);
        if (ctz_json_get_type(node_obj) != CTZ_JSON_OBJECT) continue;

        ctz_json_value* path_val = ctz_json_find_object_value(node_obj, "path");
        if (!path_val || ctz_json_get_type(path_val) != CTZ_JSON_STRING) continue;

        const char* node_path = ctz_json_get_string(path_val);
        FileNode *new_node = create_node(node_name, node_path, "", 1, 0, tree_root);
        new_node->children = create_node("dummy", "", "", 0, 1, new_node);

        if (current_node == NULL) tree_root->children = new_node;
        else {
            current_node->next = new_node;
            new_node->prev = current_node;
        }
        current_node = new_node;
    }
    ctz_json_free(root);
}

void load_directory_children(FileNode *parent_node) {
    if (!parent_node || !parent_node->is_dir) return;
    free_tree(parent_node->children);
    parent_node->children = NULL;

    DIR *d = opendir(parent_node->path);
    if (d == NULL) {
        parent_node->children = create_node("[Permission Denied]", "", "", 0, parent_node->depth + 1, parent_node);
        return;
    }

    struct dirent *dir;
    FileNode *current_child = NULL;
    while ((dir = readdir(d)) != NULL) {
        if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0 || strcmp(dir->d_name, ".log") == 0) {
            continue;
        }
        char child_path[PATH_MAX];
        snprintf(child_path, sizeof(child_path), "%s/%s", parent_node->path, dir->d_name);

        char child_relative_path[PATH_MAX];
        const char* parent_relative = parent_node->relative_path;
        snprintf(child_relative_path, sizeof(child_relative_path), "%s%s%s",
                 parent_relative,
                 (parent_relative[0] == '\0' ? "" : "/"),
                 dir->d_name);

        struct stat st;
        int is_dir = 0;
        if (lstat(child_path, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
        }
        FileNode *new_node = create_node(dir->d_name, child_path, child_relative_path, is_dir, parent_node->depth + 1, parent_node);        
        if (is_dir) {
            new_node->children = create_node("dummy", "", "", 0, new_node->depth + 1, new_node);
        }
        if (current_child == NULL) parent_node->children = new_node;
        else {
            current_child->next = new_node;
            new_node->prev = current_child;
        }
        current_child = new_node;
    }
    closedir(d);
}

FileNode *get_next_visible_node(FileNode *node) {
    if (node == NULL) return NULL;
    if (node->is_open && node->children) {
        if (strcmp(node->children->name, "dummy") != 0) {
             return node->children;
        }
    }
    if (node->next) return node->next;
    FileNode *p = node->parent;
    while (p) {
        if (p == tree_root) return NULL;
        if (p->next) return p->next;
        p = p->parent;
    }
    return NULL;
}

FileNode *get_prev_visible_node(FileNode *node) {
    if (node == NULL || node == tree_root) return NULL;


    if (node->parent == tree_root && node->prev == NULL) {
        return NULL;
    }


    FileNode *prev_sibling = node->prev;
    if (prev_sibling) {
        // Find the *last visible descendant* of the previous sibling
        while (prev_sibling->is_open && prev_sibling->children) {
            if (strcmp(prev_sibling->children->name, "dummy") == 0) break;
            FileNode *last_child = prev_sibling->children;
            while (last_child->next) last_child = last_child->next;
            prev_sibling = last_child;
        }
        return prev_sibling;
    }
    
    // If no prev sibling, go to parent (as long as it's not the invisible root)
    if (node->parent && node->parent != tree_root) {
        return node->parent;
    }
    
    return NULL;
}