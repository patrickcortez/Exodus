/* src/nodefs/nedit.c
 * Adapted from tools/cedit.c for NodeFS
 */

#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <stdarg.h>
#include "nodefs.h"

/* Control keys */
#define CTRL_KEY(k) ((k) & 0x1f)
enum keys {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

/* editor row */
typedef struct {
    char *chars;
    size_t len;
    size_t cap;
} erow;

/* editor state */
typedef struct {
    int cx, cy;
    int rowoff, coloff;
    int screenrows, screencols;
    erow *row;
    int numrows;
    int dirty;
    uint32_t node_id; // Changed from filename
    struct termios orig_termios;
} editor_t;

static editor_t E;

FILE* log_fp = NULL;
void log_msg(const char* fmt, ...) {
    if (!log_fp) log_fp = fopen("nedit_debug.log", "a");
    if (!log_fp) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(log_fp, fmt, args);
    va_end(args);
    fflush(log_fp);
}

/* ------------ low-level terminal -------------- */

static void disable_raw_mode(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios);
}

static void enable_raw_mode(void) {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) return;
    
    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static int editor_read_key(void) {
    int nread;
    char c;
    log_msg("Reading key...\n");
    while ((nread = read(STDIN_FILENO, &c, 1)) == 0);
    log_msg("Read key: %d, nread: %d\n", c, nread);
    if (nread < 0) return '\x1b';

    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
        return '\x1b';
    } else {
        return (unsigned char)c;
    }
}

static int get_window_size(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) return -1;
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
}

/* --------------- row helpers ------------------ */

static void erow_init(erow *r) {
    r->chars = NULL;
    r->len = 0;
    r->cap = 0;
}

static void erow_free(erow *r) {
    free(r->chars);
    r->chars = NULL;
    r->len = r->cap = 0;
}

static void erow_append_string(erow *r, const char *s, size_t slen) {
    if (r->len + slen + 1 >= r->cap) {
        r->cap = r->len + slen + 16;
        r->chars = realloc(r->chars, r->cap);
    }
    memcpy(&r->chars[r->len], s, slen);
    r->len += slen;
}

static void erow_insert_char(erow *r, int at, int c) {
    if (at < 0) at = 0;
    if (at > (int)r->len) at = r->len;
    if (r->len + 1 >= r->cap) {
        r->cap = r->cap ? r->cap * 2 : 64;
        r->chars = realloc(r->chars, r->cap);
    }
    memmove(&r->chars[at + 1], &r->chars[at], r->len - at);
    r->chars[at] = (char)c;
    r->len++;
}

static void erow_del_char(erow *r, int at) {
    if (at < 0 || at >= (int)r->len) return;
    memmove(&r->chars[at], &r->chars[at+1], r->len - at - 1);
    r->len--;
}

static void erow_split(erow *r, int at, erow *newrow) {
    if (at < 0) at = 0;
    if (at > (int)r->len) at = r->len;
    erow_init(newrow);
    int right_len = r->len - at;
    if (right_len > 0) {
        newrow->chars = malloc(right_len + 1);
        memcpy(newrow->chars, &r->chars[at], right_len);
        newrow->len = right_len;
        newrow->cap = right_len + 1;
    }
    r->len = at;
}

static void editor_insert_row(int at, const char *s, size_t len) {
    if (at < 0 || at > E.numrows) return;
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    if (at < E.numrows) {
        memmove(&E.row[at+1], &E.row[at], sizeof(erow) * (E.numrows - at));
    }
    erow_init(&E.row[at]);
    if (len) erow_append_string(&E.row[at], s, len);
    E.numrows++;
}

static void editor_del_row(int at) {
    if (at < 0 || at >= E.numrows) return;
    erow_free(&E.row[at]);
    if (at < E.numrows - 1) {
        memmove(&E.row[at], &E.row[at+1], sizeof(erow) * (E.numrows - at - 1));
    }
    E.numrows--;
    if (E.numrows == 0) {
        free(E.row);
        E.row = NULL;
    } else {
        E.row = realloc(E.row, sizeof(erow) * E.numrows);
    }
}

/* --------------- NodeFS I/O ---------------- */

static void editor_open(uint32_t id) {
    E.node_id = id;
    E.numrows = 0;
    E.row = NULL;

    // Read entire node content
    // We need to know size first. nodefs_read_data returns size if buffer is NULL? No.
    // We can just read a big chunk for now or improve API.
    // Let's assume max 1MB for now.
    size_t max_size = 1024 * 1024;
    char* buf = malloc(max_size);
    int len = nodefs_read_data(id, 0, buf, max_size);
    
    if (len > 0) {
        char* ptr = buf;
        char* end = buf + len;
        char* line_start = ptr;
        
        while (ptr < end) {
            if (*ptr == '\n') {
                editor_insert_row(E.numrows, line_start, ptr - line_start);
                line_start = ptr + 1;
            }
            ptr++;
        }
        if (line_start < end) {
            editor_insert_row(E.numrows, line_start, end - line_start);
        }
    }
    
    free(buf);
    E.dirty = 0;
}

static int editor_save(void) {
    // Flatten rows to buffer
    size_t total_len = 0;
    for (int i = 0; i < E.numrows; ++i) {
        total_len += E.row[i].len + 1; // +1 for newline
    }
    
    char* buf = malloc(total_len + 1);
    char* ptr = buf;
    
    for (int i = 0; i < E.numrows; ++i) {
        memcpy(ptr, E.row[i].chars, E.row[i].len);
        ptr += E.row[i].len;
        *ptr++ = '\n';
    }
    
    // Write to NodeFS
    int res = nodefs_write_data(E.node_id, buf, total_len);
    free(buf);
    
    if (res == 0) {
        E.dirty = 0;
        return 0;
    }
    return -1;
}

/* -------------- editing operations ---------------- */

static void editor_insert_char_at(int row, int col, int c) {
    if (row < 0 || row > E.numrows) return;
    if (row == E.numrows) {
        editor_insert_row(E.numrows, "", 0);
    }
    erow_insert_char(&E.row[row], col, c);
    E.dirty = 1;
}

static void editor_insert_newline(void) {
    log_msg("Insert newline at cy=%d cx=%d\n", E.cy, E.cx);
    if (E.cx == 0) {
        editor_insert_row(E.cy, "", 0);
    } else {
        erow newrow;
        erow_split(&E.row[E.cy], E.cx, &newrow);
        editor_insert_row(E.cy + 1, newrow.chars, newrow.len);
        erow_free(&newrow);
    }
    E.cy++;
    E.cx = 0;
    E.dirty = 1;
    log_msg("Newline inserted. New cy=%d\n", E.cy);
}

static void editor_del_char_at(int row, int col) {
    if (row < 0 || row >= E.numrows) return;
    if (col == 0 && row == 0) return;
    if (col > 0) {
        erow_del_char(&E.row[row], col - 1);
        E.cx--;
    } else {
        int prev_len = E.row[row - 1].len;
        erow_append_string(&E.row[row - 1], E.row[row].chars, E.row[row].len);
        editor_del_row(row);
        E.cy--;
        E.cx = prev_len;
    }
    E.dirty = 1;
}

/* -------------- rendering ------------------- */

static void editor_scroll(void) {
    if (E.cy < E.rowoff) E.rowoff = E.cy;
    if (E.cy >= E.rowoff + E.screenrows) E.rowoff = E.cy - E.screenrows + 1;
    if (E.cx < E.coloff) E.coloff = E.cx;
    if (E.cx >= E.coloff + E.screencols) E.coloff = E.cx - E.screencols + 1;
}

static void editor_draw_rows(void) {
    for (int y = 0; y < E.screenrows; ++y) {
        int filerow = y + E.rowoff;
        if (filerow >= E.numrows) {
            if (E.numrows == 0 && y == E.screenrows / 3) {
                const char *welcome = "Nedit - NodeFS Editor";
                int wlen = (int)strlen(welcome);
                int pad = (E.screencols - wlen) / 2;
                if (pad < 0) pad = 0;
                write(STDOUT_FILENO, "~", 1);
                pad--;
                for (int i = 0; i < pad; ++i) write(STDOUT_FILENO, " ", 1);
                write(STDOUT_FILENO, welcome, wlen);
            } else {
                write(STDOUT_FILENO, "~", 1);
            }
        } else {
            erow *r = &E.row[filerow];
            int len = r->len - E.coloff;
            if (len < 0) len = 0;
            if (len > E.screencols) len = E.screencols;
            if (len > 0) {
                write(STDOUT_FILENO, &r->chars[E.coloff], len);
            }
        }
        write(STDOUT_FILENO, "\x1b[K", 3);
        if (y < E.screenrows - 1) write(STDOUT_FILENO, "\r\n", 2);
    }
}

static void editor_draw_statusbar(void) {
    char status[256];
    int len = snprintf(status, sizeof(status), "Node [%u] - %d lines %s",
                       E.node_id, E.numrows, E.dirty ? "(modified)" : "");
    if (len > E.screencols) len = E.screencols;
    write(STDOUT_FILENO, "\x1b[7m", 4);
    write(STDOUT_FILENO, status, len);
    for (int i = len; i < E.screencols; ++i) write(STDOUT_FILENO, " ", 1);
    write(STDOUT_FILENO, "\x1b[m", 3);
}

static void editor_refresh_screen(void) {
    editor_scroll();
    char buf[64];
    write(STDOUT_FILENO, "\x1b[?25l", 6);
    snprintf(buf, sizeof(buf), "\x1b[H");
    write(STDOUT_FILENO, buf, strlen(buf));
    editor_draw_rows();
    write(STDOUT_FILENO, "\x1b[K", 3);
    editor_draw_statusbar();
    int cx = E.cx - E.coloff + 1;
    int cy = E.cy - E.rowoff + 1;
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", cy, cx);
    write(STDOUT_FILENO, buf, strlen(buf));
    write(STDOUT_FILENO, "\x1b[?25h", 6);
}

/* -------------- input handling ---------------- */

static void editor_move_cursor(int key) {
    erow *row = (E.cy >= 0 && E.cy < E.numrows) ? &E.row[E.cy] : NULL;
    switch (key) {
        case ARROW_LEFT:
            if (E.cx > 0) E.cx--;
            else if (E.cy > 0) { E.cy--; E.cx = E.row[E.cy].len; }
            break;
        case ARROW_RIGHT:
            if (row && E.cx < (int)row->len) E.cx++;
            else if (row && E.cx == (int)row->len) {
                if (E.cy + 1 < E.numrows) { E.cy++; E.cx = 0; }
            }
            break;
        case ARROW_UP:
            if (E.cy > 0) E.cy--;
            break;
        case ARROW_DOWN:
            if (E.cy + 1 < E.numrows) E.cy++;
            break;
    }
    row = (E.cy >= 0 && E.cy < E.numrows) ? &E.row[E.cy] : NULL;
    int rowlen = row ? (int)row->len : 0;
    if (E.cx > rowlen) E.cx = rowlen;
}

static int editor_process_keypress(void) {
    int c = editor_read_key();
    if (c == CTRL_KEY('q')) {
        return 1;
    } else if (c == CTRL_KEY('s')) {
        editor_save();
        return 0;
    } else if (c == '\r' || c == '\n') {
        editor_insert_newline();
    } else if (c == DEL_KEY || c == 127 || c == CTRL_KEY('h')) {
        if (c == DEL_KEY) {
            erow *r = (E.cy >= 0 && E.cy < E.numrows) ? &E.row[E.cy] : NULL;
            if (r && E.cx < (int)r->len) { erow_del_char(r, E.cx); E.dirty = 1; }
        } else {
            editor_del_char_at(E.cy, E.cx);
        }
    } else if (c == ARROW_UP || c == ARROW_DOWN || c == ARROW_LEFT || c == ARROW_RIGHT) {
        editor_move_cursor(c);
    } else if (c >= 32 && c <= 126) {
        editor_insert_char_at(E.cy, E.cx, c);
        E.cx++;
    }
    if (E.cy > E.numrows) E.cy = E.numrows;
    if (E.cx < 0) E.cx = 0;
    return 0;
}

static void editor_init(void) {
    E.cx = E.cy = 0;
    E.rowoff = E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    if (get_window_size(&E.screenrows, &E.screencols) == -1) {
        E.screenrows = 24;
        E.screencols = 80;
    }
    E.screenrows -= 1;
}



int nedit_run(uint32_t node_id) {
    log_msg("Starting nedit for node %d\n", node_id);
    editor_init();
    enable_raw_mode();
    editor_open(node_id);
    if (E.numrows == 0) editor_insert_row(0, "", 0);

    while (1) {
        write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);
        editor_refresh_screen();
        if (editor_process_keypress()) break;
    }

    log_msg("Exiting nedit\n");
    disable_raw_mode();
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    
    if (E.row) {
        for (int i = 0; i < E.numrows; ++i) erow_free(&E.row[i]);
        free(E.row);
    }
    return 0;
}
