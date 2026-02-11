#include "autosuggest.h"
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/types.h>

static const char* exodus_commands[] = {
    "start", "stop",
    "node-conf", "node-status", "node-edit", "node-man",
    "commit", "rebuild", "checkout", "diff", "history", "log", "clean",
    "list-subs", "add-subs", "remove-subs", "switch", "promote",
    "pack", "unpack", "pack-info", "send", "expose-node",
    "add-node", "list-nodes", "remove-node", "view-node", 
    "activate", "deactivate", "attr-node", "info-node", "search-attr", 
    "look", "unpin",
    "upload", "find", "change", "wc", "wl", "cc",
    "unit-list", "view-unit", "sync", "unit-set", "view-cache", "push", 
    "coord-list", "connect", "ping",
    "help", "exit", "quit",
    NULL
};

const char* get_last_token(const char* input) {
    const char* last_space = strrchr(input, ' ');
    if (last_space) {
        return last_space + 1;
    }
    return input;
}

int is_exodus_command(const char* input) {
    for (int i = 0; exodus_commands[i] != NULL; i++) {
        if (strcmp(exodus_commands[i], input) == 0) {
            return 1;
        }
    }
    return 0;
}

int scan_token_for_suggestion(const char* token, char* suggestion_buf, size_t buf_size) {
    suggestion_buf[0] = '\0';
    if (!token || strlen(token) == 0) return 0;

    size_t token_len = strlen(token);
    int match_type = 0; 

    const char* best_cmd = NULL;
    for (int i = 0; exodus_commands[i] != NULL; i++) {
        if (strncmp(exodus_commands[i], token, token_len) == 0) {
            best_cmd = exodus_commands[i];
            match_type |= 1;
            break; 
        }
    }

    char best_dir[256] = "";
    DIR* d = opendir(".");
    if (d) {
        struct dirent* dir;
        while ((dir = readdir(d)) != NULL) {
            if (strncmp(dir->d_name, token, token_len) == 0) {
                if (dir->d_type == DT_DIR) {
                     strncpy(best_dir, dir->d_name, sizeof(best_dir)-1);
                     match_type |= 2;
                     break; 
                }
            }
        }
        closedir(d);
    }

    if (match_type & 1) {
        if (strlen(best_cmd) > token_len) {
             strncpy(suggestion_buf, best_cmd + token_len, buf_size - 1);
        }
    } else if (match_type & 2) {
        if (strlen(best_dir) > token_len) {
             strncpy(suggestion_buf, best_dir + token_len, buf_size - 1);
        }
    }
    
    return match_type;
}
