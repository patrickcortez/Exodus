#include "auto-nav.h"
#include "autosuggest.h"
#include "errors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <termios.h>

void shell_enable_raw_mode_nav(struct termios* orig_termios) {
    if (tcgetattr(STDIN_FILENO, orig_termios) == -1) {
        exodus_error("Failed to get terminal attributes");
        return;
    }
    struct termios raw = *orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    if (tcsetattr(STDIN_FILENO, TCSADRAIN, &raw) == -1) {
        exodus_error("Failed to set terminal attributes");
    }
}

void shell_disable_raw_mode_nav(struct termios* orig_termios) {
    if (tcsetattr(STDIN_FILENO, TCSADRAIN, orig_termios) == -1) {
        exodus_error("Failed to restore terminal attributes");
    }
}

int check_conflict(const char* input) {
    int match_type = 0; 
    
    if (is_exodus_command(input)) {
        match_type |= 1;
    }

    struct stat st;
    if (stat(input, &st) == 0 && S_ISDIR(st.st_mode)) {
        match_type |= 2;
    }

    return match_type;
}

int shell_resolve_conflict(const char* input) {
    printf("\n\nAmbiguous input: '%s' is both a command and a directory.\n", input);
    printf("Select execution target:\n");

    int selected = 0; 
    const char* options[] = { "Command", "Directory" };
    
    struct termios orig_termios;
    shell_enable_raw_mode_nav(&orig_termios);

    printf("\033[?25l");

    while (1) {
        for (int i = 0; i < 2; i++) {
            if (i == selected) {
                printf("  %s[%s] <-\r\n", input, options[i]);
            } else {
                printf("  %s[%s]   \r\n", input, options[i]);
            }
        }
        printf("\033[2A");
        fflush(stdout);

        char c;
        if (read(STDIN_FILENO, &c, 1) != 1) break;

        if (c == '\033') { 
            char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) == 0) break;
            if (read(STDIN_FILENO, &seq[1], 1) == 0) break;

            if (seq[0] == '[') {
                if (seq[1] == 'A') { 
                    selected = (selected - 1 + 2) % 2;
                } else if (seq[1] == 'B') { 
                    selected = (selected + 1) % 2;
                }
            }
        } else if (c == '\n' || c == '\r') {
            break; 
        } else if (c == 3) { 
            selected = -1; 
            break;
        }
    }

    printf("\033[2B");
    printf("\033[?25h");
    
    shell_disable_raw_mode_nav(&orig_termios);
    printf("\n");

    return selected;
}

