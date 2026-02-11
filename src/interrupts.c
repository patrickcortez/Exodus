#include "interrupts.h"
#include "signals.h"
#include "utils.h"
#include "excon_io.h"
#include "ctz-set.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/limits.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <signal.h>
#include "autosuggest.h"

static SetConfig* history_cfg = NULL;
static const char* history_file_path = NULL;
static char saved_history_path[PATH_MAX];
static volatile sig_atomic_t win_resized = 0;

static void ensure_data_dir(const char* exe_dir) {
    char data_dir[PATH_MAX];
    snprintf(data_dir, sizeof(data_dir), "%s/data", exe_dir);
    struct stat st;
    if (stat(data_dir, &st) == -1) {
        mkdir(data_dir, 0755);
    }
}

void history_init(const char* exe_dir) {
    ensure_data_dir(exe_dir);
    snprintf(saved_history_path, sizeof(saved_history_path), "%s/data/history.set", exe_dir);
    history_file_path = saved_history_path;

    history_cfg = set_load(history_file_path);
    if (!history_cfg) {
        history_cfg = set_create(history_file_path);
        SetNode* root = set_get_root(history_cfg);
        if (set_node_type(root) != SET_TYPE_ARRAY) {
             set_set_child(root, "history", SET_TYPE_ARRAY);
        }
    }
}

void history_add(const char* command) {
    if (!history_cfg || strlen(command) == 0) return;
    
    SetNode* root = set_get_root(history_cfg);
    SetNode* hist_arr = set_get_child(root, "history");
    if (!hist_arr) {
        hist_arr = set_set_child(root, "history", SET_TYPE_ARRAY);
    }

    size_t count = set_node_size(hist_arr);
    if (count > 0) {
        SetNode* last = set_get_at(hist_arr, count - 1);
        const char* last_cmd = set_node_string(last, "");
        if (strcmp(last_cmd, command) == 0) return;
    }

    SetNode* new_node = set_array_push(hist_arr, SET_TYPE_STRING);
    set_node_set_string(new_node, command);
    
    set_save(history_cfg);
}

void history_close(void) {
    if (history_cfg) {
        set_save(history_cfg);
        set_free(history_cfg);
        history_cfg = NULL;
    }
}

static char* get_history_at(size_t index) {
    if (!history_cfg) return NULL;
    SetNode* root = set_get_root(history_cfg);
    SetNode* hist_arr = set_get_child(root, "history");
    if (!hist_arr) return NULL;
    size_t count = set_node_size(hist_arr);
    if (index >= count) return NULL;
    
    SetNode* item = set_get_at(hist_arr, index);
    return (char*)set_node_string(item, "");
}

static size_t get_history_count(void) {
    if (!history_cfg) return 0;
    SetNode* root = set_get_root(history_cfg);
    SetNode* hist_arr = set_get_child(root, "history");
    if (!hist_arr) return 0;
    return set_node_size(hist_arr);
}

static struct termios orig_termios;
static int is_orig_termios_set = 0;

void shell_disable_raw_mode(void) {
    if (is_orig_termios_set && tcsetattr(STDIN_FILENO, TCSADRAIN, &orig_termios) == -1) {
        // die("tcsetattr");
    }
}

void shell_enable_raw_mode(void) {
    if (!is_orig_termios_set) {
        if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
            // die("tcgetattr");
        }
        atexit(shell_disable_raw_mode);
        is_orig_termios_set = 1;
    }

    struct termios raw = orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG); 
    
    raw.c_cc[VMIN] = 1; 
    raw.c_cc[VTIME] = 0; 
    
    if (tcsetattr(STDIN_FILENO, TCSADRAIN, &raw) == -1) {
        // die("tcsetattr");
    }
}

static int get_window_size(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
    const char* col_env = getenv("COLUMNS");
    const char* row_env = getenv("LINES");
    if (col_env && row_env) {
        *cols = atoi(col_env);
        *rows = atoi(row_env);
        if (*cols > 0 && *rows > 0) return 0;
    }
    *cols = 80;
    *rows = 24;
    return -1; 
}

static size_t get_visible_length(const char* str) {
    size_t len = 0;
    int in_esc = 0;
    for (size_t i = 0; str[i]; i++) {
        if (str[i] == '\033') {
            in_esc = 1;
        } else if (in_esc) {
            if (isalpha(str[i])) {
                in_esc = 0;
            }
        } else {
            len++;
        }
    }
    return len;
}

static void handle_winch(int sig) {
    (void)sig;
    win_resized = 1;
}

static void refresh_line(const char* prompt, const char* buf, size_t len, size_t cursor_pos, int selection_active, const char* suggestion, int* last_cursor_row) {
    char sbuf[64];
    int cols, rows;
    get_window_size(&rows, &cols);

    // 1. Move to Prompt Start
    if (*last_cursor_row > 0) {
        snprintf(sbuf, sizeof(sbuf), "\033[%dA", *last_cursor_row);
        (void)shell_write(sbuf, (int)strlen(sbuf));
    }
    (void)shell_write("\r", 1);
    (void)shell_write("\033[J", 3);
    
    // 2. Print Content
    (void)shell_write(prompt, (int)strlen(prompt));
    if (selection_active) (void)shell_write("\033[7m", 4);
    (void)shell_write(buf, (int)len);
    if (selection_active) (void)shell_write("\033[0m", 4);
    
    if (suggestion && strlen(suggestion) > 0) {
        (void)shell_write("\033[90m", 5);
        (void)shell_write(suggestion, (int)strlen(suggestion));
        (void)shell_write("\033[0m", 4);
    }
    
    // 3. Calculate Geometry via Simulation
    int cur_col = 0;
    int cur_row = 0;
    int cursor_target_row = 0;
    int cursor_target_col = 0;
    
    size_t prompt_len = get_visible_length(prompt);
    
    // Simulate Prompt
    for (size_t i = 0; i < prompt_len; i++) {
        if (cur_col >= cols) {
            cur_col = 0;
            cur_row++;
        }
        cur_col++;
    }
    
    // Simulate Buffer + Suggestion
    size_t content_len = len;
    if (suggestion) content_len += strlen(suggestion);
    
    // We need to iterate 'buf' then 'suggestion' separately to handle newlines
    
    // Buffer
    for (size_t i = 0; i < len; i++) {
        if (i == cursor_pos) {
            cursor_target_row = cur_row;
            cursor_target_col = cur_col;
        }
        
        if (buf[i] == '\n') {
            cur_col = 0;
            cur_row++;
        } else {
             if (cur_col >= cols) {
                cur_col = 0;
                cur_row++;
            }
            cur_col++;
        }
    }
    if (len == cursor_pos) {
        cursor_target_row = cur_row;
        cursor_target_col = cur_col;
    }

    // Suggestion (Simple append check)
    if (suggestion) {
        size_t slen = strlen(suggestion);
        for (size_t i = 0; i < slen; i++) {
            if (suggestion[i] == '\n') {
                 cur_col = 0;
                 cur_row++;
            } else {
                if (cur_col >= cols) {
                    cur_col = 0;
                    cur_row++;
                }
                cur_col++;
            }
        }
    }

    int total_rows = cur_row;

    // 4. Move Cursor to End (Relative)
    // We just printed everything, so we are at 'total_rows', 'cur_col' (visibly)
    // Move back to Start of Prompt (Row 0)
    
     if (total_rows > 0) {
         snprintf(sbuf, sizeof(sbuf), "\033[%dA", total_rows);
         (void)shell_write(sbuf, (int)strlen(sbuf));
    }
    (void)shell_write("\r", 1);
    
    // 5. Move to Target
    if (cursor_target_row > 0) {
        snprintf(sbuf, sizeof(sbuf), "\033[%dB", cursor_target_row);
        (void)shell_write(sbuf, (int)strlen(sbuf));
    }
    if (cursor_target_col > 0) {
        snprintf(sbuf, sizeof(sbuf), "\033[%dC", cursor_target_col);
        (void)shell_write(sbuf, (int)strlen(sbuf));
    }
    
    *last_cursor_row = cursor_target_row;
}

int shell_read_line_robust(char* buf, size_t size, const char* prompt) {
    shell_enable_raw_mode();
    
    struct sigaction sa;
    sa.sa_handler = handle_winch;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGWINCH, &sa, NULL);

    size_t len = 0;
    size_t cursor_pos = 0;
    size_t history_index = get_history_count(); 
    char original_input_buffer[4096] = {0}; 
    char suggestion[256] = {0};
    int history_scroll_active = 0;
    int selection_active = 0;
    int last_cursor_row = 0;

    memset(buf, 0, size);
    (void)shell_write(prompt, (int)strlen(prompt));
    
    int rows, cols;
    if (get_window_size(&rows, &cols) == -1) cols = 80;
    size_t prompt_len = get_visible_length(prompt);
    last_cursor_row = prompt_len / cols;

    while (1) {
        if (win_resized) {
            win_resized = 0;
            int r, c;
            if (get_window_size(&r, &c) == -1) c = 80;
            
            // Simulation to find cursor row
            int cur_col = 0;
            int cur_row = 0;
            size_t p_len = get_visible_length(prompt);
            
            // Prompt simulation
            for (size_t i = 0; i < p_len; i++) {
                if (cur_col >= c) {
                    cur_col = 0;
                    cur_row++;
                }
                cur_col++;
            }
            
            // Buffer simulation
            for (size_t i = 0; i < cursor_pos; i++) {
                if (buf[i] == '\n') {
                    cur_col = 0;
                    cur_row++;
                } else {
                    if (cur_col >= c) {
                        cur_col = 0;
                        cur_row++;
                    }
                    cur_col++;
                }
            }
            last_cursor_row = cur_row;
            
            refresh_line(prompt, buf, len, cursor_pos, selection_active, suggestion, &last_cursor_row);
        }

        if (sig_interrupt_flag) {
            sig_interrupt_flag = 0;
            buf[0] = '\0';
            shell_disable_raw_mode();
            return 1; 
        }

        char c;
        if (shell_read_byte(&c) <= 0) {
            if (win_resized) continue; 
            if (errno == EINTR) continue;
            break;
        }

        if (c == 3) {
            (void)shell_write("^C\r\n", 4);
            buf[0] = '\0';
            shell_disable_raw_mode();
            return 1;
        }
        
        if (c == 4) {
             if (len == 0) {
                 shell_disable_raw_mode();
                 return -1; 
             }
             continue; 
        }

        if (c == '\r' || c == '\n') {
            int in_quote = 0;
            char quote_char = 0;
            int backslash = 0;
            for(size_t i=0; i<len; i++) {
                if (buf[i] == '\\' && !backslash) {
                    backslash = 1;
                } else {
                    if (backslash) {
                        backslash = 0; 
                    } else {
                        if (in_quote) {
                            if (buf[i] == quote_char) in_quote = 0;
                        } else {
                            if (buf[i] == '"' || buf[i] == '\'') {
                                in_quote = 1;
                                quote_char = buf[i];
                            }
                        }
                    }
                }
            }
            if (len > 0 && buf[len-1] == '\\' && (len == 1 || buf[len-2] != '\\')) {
                (void)shell_write("\n> ", 3);
                len--;
                buf[len] = '\0';
                cursor_pos--;
                continue;
            }
            if (in_quote) {
                (void)shell_write("\nquote> ", 8);
                if (len < size - 1) {
                    buf[len++] = '\n';
                    buf[len] = '\0';
                    cursor_pos++;
                }
                continue;
            }
            (void)shell_write("\r\n", 2);
            break;
        }

        if (c == 127 || c == 8) {
            if (selection_active) {
                selection_active = 0;
                refresh_line(prompt, buf, len, cursor_pos, 0, suggestion, &last_cursor_row);
            }
            if (cursor_pos > 0) {
                memmove(&buf[cursor_pos - 1], &buf[cursor_pos], len - cursor_pos);
                len--;
                buf[len] = '\0';
                cursor_pos--;
                if (cursor_pos == len) {
                     const char* last = get_last_token(buf);
                     scan_token_for_suggestion(last, suggestion, sizeof(suggestion));
                } else {
                    suggestion[0] = '\0';
                }
                refresh_line(prompt, buf, len, cursor_pos, 0, suggestion, &last_cursor_row);
            }
            continue;
        }
        
        if (c == 9) {
            if (strlen(suggestion) > 0) {
                size_t slug_len = strlen(suggestion);
                if (len + slug_len < size - 1) {
                    strcat(buf, suggestion);
                    len += slug_len;
                    cursor_pos += slug_len;
                    suggestion[0] = '\0';
                    refresh_line(prompt, buf, len, cursor_pos, 0, suggestion, &last_cursor_row);
                }
            }
            continue;
        }

        if (c == '\x1b') {
            char seq[3];
            if (shell_read_byte(&seq[0]) != 1) continue;
            if (shell_read_byte(&seq[1]) != 1) continue;

            if (seq[0] == '[') {
                switch (seq[1]) {
                    case 'A':
                    {
                        if (history_index > 0) {
                            if (!history_scroll_active) {
                                strncpy(original_input_buffer, buf, sizeof(original_input_buffer)-1);
                                original_input_buffer[sizeof(original_input_buffer)-1] = 0;
                                history_scroll_active = 1;
                            }
                            history_index--;
                            char* cmd = get_history_at(history_index);
                            if (cmd) {
                                size_t cmd_len = strlen(cmd);
                                if (cmd_len >= size) cmd_len = size - 1;
                                strncpy(buf, cmd, size);
                                buf[size-1] = '\0'; 
                                len = cmd_len;
                                cursor_pos = cmd_len;
                                suggestion[0] = '\0';
                                refresh_line(prompt, buf, len, cursor_pos, 0, NULL, &last_cursor_row);
                            }
                        }
                    }
                    break;
                    case 'B':
                    {
                        size_t max_hist = get_history_count();
                        if (history_index < max_hist) {
                            history_index++;
                            if (history_index == max_hist) {
                                strncpy(buf, original_input_buffer, size);
                                len = strlen(buf);
                                cursor_pos = len;
                                history_scroll_active = 0;
                            } else {
                                char* cmd = get_history_at(history_index);
                                if (cmd) {
                                    size_t cmd_len = strlen(cmd);
                                    if (cmd_len >= size) cmd_len = size - 1;
                                    strncpy(buf, cmd, size);
                                    len = cmd_len;
                                    cursor_pos = cmd_len;
                                }
                            }
                            suggestion[0] = '\0';
                            refresh_line(prompt, buf, len, cursor_pos, 0, NULL, &last_cursor_row);
                        }
                    }
                    break;
                    case 'C':
                        if (cursor_pos < len) {
                            cursor_pos++;
                            if (selection_active) refresh_line(prompt, buf, len, cursor_pos, 1, suggestion, &last_cursor_row);
                            else (void)shell_write("\033[C", 3);
                        } else if (strlen(suggestion) > 0) {
                            size_t slug_len = strlen(suggestion);
                            if (len + slug_len < size - 1) {
                                strcat(buf, suggestion);
                                len += slug_len;
                                cursor_pos += slug_len;
                                suggestion[0] = '\0';
                                refresh_line(prompt, buf, len, cursor_pos, 0, suggestion, &last_cursor_row);
                            }
                        }
                        break;
                    case 'D':
                        if (cursor_pos > 0) {
                            cursor_pos--;
                            if (selection_active) refresh_line(prompt, buf, len, cursor_pos, 1, suggestion, &last_cursor_row);
                            else (void)shell_write("\b", 1);
                        }
                        break;
                    case 'H':
                        cursor_pos = 0;
                        refresh_line(prompt, buf, len, cursor_pos, selection_active, suggestion, &last_cursor_row);
                        break;
                    case 'F':
                        cursor_pos = len;
                        refresh_line(prompt, buf, len, cursor_pos, selection_active, suggestion, &last_cursor_row);
                        break;
                }
            }
            continue;
        }
        
        if (c == 1) { 
            selection_active = !selection_active;
            refresh_line(prompt, buf, len, cursor_pos, selection_active, suggestion, &last_cursor_row);
            continue;
        }

        if (c == 5) {
            cursor_pos = len;
            refresh_line(prompt, buf, len, cursor_pos, selection_active, suggestion, &last_cursor_row);
            continue;
        }

        if (c == 21) {
            buf[0] = '\0';
            len = 0;
            cursor_pos = 0;
            suggestion[0] = '\0';
            refresh_line(prompt, buf, len, cursor_pos, 0, NULL, &last_cursor_row);
            continue;
        }

        if (c == 11) {
            buf[cursor_pos] = '\0';
            len = cursor_pos;
            suggestion[0] = '\0';
            const char* last = get_last_token(buf);
            scan_token_for_suggestion(last, suggestion, sizeof(suggestion));
            refresh_line(prompt, buf, len, cursor_pos, 0, suggestion, &last_cursor_row);
            continue;
        }

        if (c == 12) {
            shell_write("\033[H\033[J", 6);
            refresh_line(prompt, buf, len, cursor_pos, 0, suggestion, &last_cursor_row);
            continue;
        }

        if (c == 23) {
            exodus_clear_screen();
            last_cursor_row = 0;
            refresh_line(prompt, buf, len, cursor_pos, 0, suggestion, &last_cursor_row);
            continue;
        }

        if (c == 22) {
            continue; 
        }

        if (!iscntrl(c) && len < size - 1) {
             if (selection_active) {
                 selection_active = 0;
                 refresh_line(prompt, buf, len, cursor_pos, 0, suggestion, &last_cursor_row);
             }

            if (cursor_pos == len) {
                buf[len] = c;
                len++;
                buf[len] = '\0';
                cursor_pos++;
                const char* last = get_last_token(buf);
                scan_token_for_suggestion(last, suggestion, sizeof(suggestion));
            } else {
                memmove(&buf[cursor_pos + 1], &buf[cursor_pos], len - cursor_pos);
                buf[cursor_pos] = c;
                len++;
                buf[len] = '\0';
                cursor_pos++;
                suggestion[0] = '\0';
            }
            
            refresh_line(prompt, buf, len, cursor_pos, 0, suggestion, &last_cursor_row);
        }
    }

    shell_disable_raw_mode();
    if (len > 0) {
        history_add(buf);
    }
    return 0;
}
