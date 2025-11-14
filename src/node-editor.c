/*
 * node-editor.c
 *
 * A simple, standalone ncurses text editor for the Exodus TUI.
 * Refactored to have a "nano" style layout.
 *
 * It takes one argument: the path to the file to edit.
 *
 * Controls:
 * - Ctrl+S: Save
 * - Ctrl+X: Exit
 * - Arrow Keys, Backspace, Enter, Typing: Normal text editing
 *
 * COMPILE:
 * gcc -Wall -Wextra -O2 -o node-editor node-editor.c -lncursesw
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ncurses.h>
#include <locale.h>
#include <ctype.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define GUTTER_WIDTH 6
// --- Data Structures ---

typedef struct Line {
    char *text;
    int length;
    int capacity;
    struct Line *next;
    struct Line *prev;
} Line;

// --- Editor State ---
Line *editor_head = NULL;
Line *editor_top_line = NULL;
Line *editor_cursor_line = NULL;
int editor_cursor_x = 0; // X position relative to line start
int editor_cursor_y = 0; // Y position relative to window top
int editor_top_line_num = 1;
int editor_win_height = 0;
int running = 1;
char file_path[PATH_MAX] = {0};
char status_message[256] = "[ Read 0 lines ]"; // Nano-style default
int is_modified = 0; // --- NANO-STYLE REFACTOR: Track file changes ---

// --- Window ---
// --- NANO-STYLE REFACTOR: We only need one sub-window for the text area ---
WINDOW *editor_win;
WINDOW *gutter_win;
// --- Function Declarations ---
void init_ncurses();
void create_windows();
void destroy_windows();
void main_loop();
void draw_layout();
void draw_header();
void draw_editor();
void draw_gutter();
void draw_footer();
void handle_input(int ch);

// Editor functions
Line *create_line(const char *text);
void free_editor_content();
void load_file_into_editor(const char *path);
void save_editor_content(const char *path);
void editor_move_cursor(int dx, int dy);
void editor_insert_char(int ch);
void editor_insert_newline();
void editor_delete_char();
void clamp_cursor();
void clear_status_message_on_input();

/*
 * ===================================================================
 * MAIN APPLICATION LOGIC
 * ===================================================================
 */

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: node-editor <file_path>\n");
        return 1;
    }
    strncpy(file_path, argv[1], PATH_MAX - 1);

    setlocale(LC_ALL, "");
    init_ncurses();
    create_windows();

    load_file_into_editor(file_path);

    main_loop();

    destroy_windows();
    endwin();
    free_editor_content();
    return 0;
}

void init_ncurses() {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    start_color();
    use_default_colors();
    curs_set(1);
    init_pair(1, COLOR_CYAN, -1);
}

void create_windows() {
    int height, width;
    getmaxyx(stdscr, height, width);
    
    // --- NANO-STYLE REFACTOR ---
    // Header is line 0 (on stdscr)
    // Editor window is from line 1 to height - 3
    // Footer is lines height - 2 and height - 1 (on stdscr)
    
    editor_win_height = height - 3;

    gutter_win = newwin(editor_win_height, GUTTER_WIDTH, 1, 0);

    editor_win = newwin(editor_win_height, width - GUTTER_WIDTH, 1, GUTTER_WIDTH);

    scrollok(editor_win, TRUE);
}

void destroy_windows() {
    // --- NANO-STYLE REFACTOR: No border window to delete ---
    delwin(editor_win);
    delwin(gutter_win);
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
 * DRAWING FUNCTIONS (NANO-STYLE REFACTOR)
 * ===================================================================
 */

void draw_layout() {
    // Clear the main screen (for header/footer)
    werase(stdscr);
    
    draw_header();
    draw_editor(); // This draws to editor_win
    draw_footer();
    draw_gutter();

    // Refresh stdscr for header/footer
    wnoutrefresh(stdscr);

    wnoutrefresh(gutter_win);
    
    // Move cursor to correct position in the sub-window
    wmove(editor_win, editor_cursor_y, editor_cursor_x);
    
    // Refresh the editor window LAST to place the physical cursor
    wrefresh(editor_win); 
    
}

/**
 * @brief Draws the top header bar (line 0)
 */
void draw_header() {
    int width = getmaxx(stdscr);
    char title[256];
    
    snprintf(title, sizeof(title), " Exodus Text Editor | File: %s%s", 
             file_path, 
             is_modified ? " (Modified)" : "");
             
    attron(A_REVERSE);
    mvprintw(0, 0, "%s", title);
    for (int i = strlen(title); i < width; i++) {
        addch(' ');
    }
    attroff(A_REVERSE);
}

void draw_gutter() {
    wclear(gutter_win);
    int y = 0;
    Line *line = editor_top_line;
    int line_num = editor_top_line_num;

    // Use a color for the gutter, e.g., COLOR_PAIR(1)
    wattron(gutter_win, COLOR_PAIR(1));

    while (line != NULL && y < editor_win_height) {
        // Print right-aligned number with a space
        mvwprintw(gutter_win, y, 0, "%*d ", GUTTER_WIDTH - 1, line_num);
        line = line->next;
        line_num++;
        y++;
    }
    
    wattroff(gutter_win, COLOR_PAIR(1));
    // No refresh here, draw_layout handles it
}

void draw_editor() {
    wclear(editor_win);
    int y = 0;
    Line *line = editor_top_line;
    while (line != NULL && y < editor_win_height) {
        mvwprintw(editor_win, y, 0, "%s", line->text);
        line = line->next;
        y++;
    }
    // No refresh here, draw_layout handles it
}


void draw_footer() {
    int height, width;
    getmaxyx(stdscr, height, width);

    // --- Line 1: Keybindings ---
    char keys1[256] = " ^X Exit    ^O Save";
    attron(A_REVERSE);
    mvprintw(height - 2, 0, "%s", keys1);
    for (int i = strlen(keys1); i < width; i++) addch(' ');

    // --- Line 2: Status Message ---
    char status_line[256];
    snprintf(status_line, sizeof(status_line), " %s", status_message);
    mvprintw(height - 1, 0, "%s", status_line);
    for (int i = strlen(status_line); i < width; i++) addch(' ');

    attroff(A_REVERSE);
}


/*
 * ===================================================================
 * INPUT HANDLING
 * ===================================================================
 */

void clear_status_message_on_input() {
    // Clear temporary messages (like "File Saved!")
    if (strncmp(status_message, "[", 1) != 0) {
         snprintf(status_message, sizeof(status_message), " ");
    }
}

void handle_input(int ch) {
    clear_status_message_on_input();

    switch (ch) {
        case 'x' & 0x1F: // Ctrl+X
            running = 0;
            break;
        case 'o' & 0x1F: // Ctrl+O (Write "Out")
            save_editor_content(file_path);
            snprintf(status_message, sizeof(status_message), "File saved successfully!");
            break;      
        case KEY_BACKSPACE:
        case 127:
        case 8:
            editor_delete_char();
            is_modified = 1; // --- NANO-STYLE REFACTOR ---
            break;
        
        case KEY_ENTER:
        case '\n':
            editor_insert_newline();
            is_modified = 1; // --- NANO-STYLE REFACTOR ---
            break;

        case 9: // TAB
            editor_insert_char(' ');
            editor_insert_char(' ');
            editor_insert_char(' ');
            editor_insert_char(' ');
            is_modified = 1; // --- NANO-STYLE REFACTOR ---
            break;

        case KEY_UP:
            editor_move_cursor(0, -1);
            break;
        case KEY_DOWN:
            editor_move_cursor(0, 1);
            break;
        case KEY_LEFT:
            editor_move_cursor(-1, 0);
            break;
        case KEY_RIGHT:
            editor_move_cursor(1, 0);
            break;

        // Ignore other special keys
        case KEY_HOME:
        case KEY_END:
        case KEY_NPAGE:
        case KEY_PPAGE:
            break;

        default:
            if (isprint(ch)) {
                editor_insert_char(ch);
                is_modified = 1; // --- NANO-STYLE REFACTOR ---
            }
            break;
    }
    clamp_cursor();
}

/*
 * ===================================================================
 * TEXT EDITOR LOGIC
 * ===================================================================
 */

Line *create_line(const char *text) {
    Line *line = (Line *)malloc(sizeof(Line));
    if (!line) return NULL;
    line->length = strlen(text);
    line->capacity = line->length + 8;
    line->text = (char *)malloc(line->capacity);
    if (!line->text) { free(line); return NULL; }
    strcpy(line->text, text);
    line->next = NULL;
    line->prev = NULL;
    return line;
}

void free_editor_content() {
    Line *line = editor_head;
    while (line) {
        Line *next = line->next;
        free(line->text);
        free(line);
        line = next;
    }
    editor_head = NULL;
    editor_top_line = NULL;
    editor_cursor_line = NULL;
}

void load_file_into_editor(const char *path) {
    free_editor_content();
    
    int line_count = 0;
    FILE *f = fopen(path, "r");
    if (f) {
        char buffer[4096];
        Line *current_line = NULL;
        while (fgets(buffer, sizeof(buffer), f)) {
            buffer[strcspn(buffer, "\n")] = 0;
            Line *new_line = create_line(buffer);
            if (current_line == NULL) editor_head = new_line;
            else {
                current_line->next = new_line;
                new_line->prev = current_line;
            }
            current_line = new_line;
            line_count++;
        }
        fclose(f);
    }
    
    if (editor_head == NULL) {
        editor_head = create_line(""); // Handle empty or non-existent file
        line_count = 1;
    }

    editor_top_line = editor_head;
    editor_cursor_line = editor_head;
    editor_cursor_x = 0;
    editor_cursor_y = 0;
    editor_top_line_num = 1;
    is_modified = 0; // --- NANO-STYLE REFACTOR: Reset modified flag
    
    snprintf(status_message, sizeof(status_message), "[ Read %d lines ]", line_count);
}

void save_editor_content(const char *path) {
    if (path == NULL || path[0] == '\0') {
        snprintf(status_message, sizeof(status_message), "No file open to save.");
        return;
    }
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        snprintf(status_message, sizeof(status_message), "ERROR: Could not save file!");
        return;
    }
    Line *line = editor_head;
    int lines_written = 0;
    while (line) {
        fprintf(f, "%s\n", line->text);
        line = line->next;
        lines_written++;
    }
    fclose(f);
    
    is_modified = 0; // --- NANO-STYLE REFACTOR: Reset modified flag
    snprintf(status_message, sizeof(status_message), "[ Wrote %d lines ]", lines_written);
}

void editor_move_cursor(int dx, int dy) {
    if (dy < 0 && editor_cursor_line && editor_cursor_line->prev) {
        editor_cursor_line = editor_cursor_line->prev;
        if (editor_cursor_y > 0) editor_cursor_y--;
    else {
            editor_top_line = editor_top_line->prev;
            editor_top_line_num--; // --- NEW ---
        }
    }
    if (dy > 0 && editor_cursor_line && editor_cursor_line->next) {
        editor_cursor_line = editor_cursor_line->next;
        if (editor_cursor_y < editor_win_height - 1) editor_cursor_y++;
        else {
            editor_top_line = editor_top_line->next;
            editor_top_line_num++; // --- NEW ---
        }
    }
    if (dx < 0) {
        if (editor_cursor_x > 0) {
            editor_cursor_x--;
        } else if (editor_cursor_line && editor_cursor_line->prev) {
            // Move to end of previous line
            editor_move_cursor(0, -1);
            editor_cursor_x = editor_cursor_line->length;
        }
    }
    if (dx > 0) {
        if (editor_cursor_line && editor_cursor_x < editor_cursor_line->length) {
            editor_cursor_x++;
        } else if (editor_cursor_line && editor_cursor_line->next) {
            // Move to start of next line
            editor_move_cursor(0, 1);
            editor_cursor_x = 0;
        }
    }
    clamp_cursor();
}

void clamp_cursor() {
    if (!editor_cursor_line) return;
    if (editor_cursor_x > editor_cursor_line->length) {
        editor_cursor_x = editor_cursor_line->length;
    }
}

void editor_insert_char(int ch) {
    if (!editor_cursor_line) return;

    Line *line = editor_cursor_line;
    if (line->length + 1 >= line->capacity) {
        line->capacity = line->capacity * 2 + 8;
        char *new_text = (char *)realloc(line->text, line->capacity);
        if (!new_text) return; // Out of memory
        line->text = new_text;
    }

    memmove(&line->text[editor_cursor_x + 1], 
            &line->text[editor_cursor_x], 
            line->length - editor_cursor_x + 1);
    
    line->text[editor_cursor_x] = ch;
    line->length++;
    editor_cursor_x++;
}

void editor_delete_char() {
    if (!editor_cursor_line) return;
    Line *line = editor_cursor_line;

    if (editor_cursor_x > 0) {
        memmove(&line->text[editor_cursor_x - 1], 
                &line->text[editor_cursor_x], 
                line->length - editor_cursor_x + 1);
        line->length--;
        editor_cursor_x--;
    } else if (editor_cursor_line->prev) {
        // Backspace at the start of a line (merge with previous)
        Line *prev_line = line->prev;
        if (!prev_line) return;

        int prev_len = prev_line->length;
        int new_cap = prev_len + line->length + 1;
        
        char *new_text = (char *)realloc(prev_line->text, new_cap);
        if (!new_text) return; // Out of memory
        prev_line->text = new_text;
        prev_line->capacity = new_cap;

        memcpy(&prev_line->text[prev_len], line->text, line->length + 1);
        prev_line->length += line->length;

        editor_cursor_line = prev_line;
        prev_line->next = line->next;
        if (line->next) {
            line->next->prev = prev_line;
        }

        if (editor_cursor_y > 0) editor_cursor_y--;
        else {
            editor_top_line = editor_top_line->prev;
            editor_top_line_num--; 
        }
        
        editor_cursor_x = prev_len;
        
        free(line->text);
        free(line);
    }
}

void editor_insert_newline() {
    if (!editor_cursor_line) return;
    Line *line = editor_cursor_line;

    char *split_text = &line->text[editor_cursor_x];
    Line *new_line = create_line(split_text);

    line->text[editor_cursor_x] = '\0';
    line->length = editor_cursor_x;
    
    new_line->next = line->next;
    new_line->prev = line;
    if (line->next) {
        line->next->prev = new_line;
    }
    line->next = new_line;

    editor_cursor_line = new_line;
    editor_cursor_x = 0;
    if (editor_cursor_y < editor_win_height - 1) {
        editor_cursor_y++;
    } else {
        editor_top_line = editor_top_line->next;
        editor_top_line_num++;
    }
}