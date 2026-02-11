/* gcc -Wall -Wextra -O2 -c kernel_repl.c -o kernel_repl.o -Iinclude */
#include "kernel_repl.h"
#include "syscall_commands.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

void repl_init(repl_state_t *state) {
    memset(state, 0, sizeof(repl_state_t));
    state->line_count = 0;
    state->var_count = 0;
    state->in_block = 0;
}

void repl_reset_block(repl_state_t *state) {
    state->line_count = 0;
    state->in_block = 0;
}

void repl_add_line(repl_state_t *state, const char *line) {
    if (state->line_count >= REPL_MAX_LINES)
        return;
    strncpy(state->lines[state->line_count], line, REPL_MAX_LINE_LEN - 1);
    state->lines[state->line_count][REPL_MAX_LINE_LEN - 1] = '\0';
    state->line_count++;
    state->in_block = 1;
}

static repl_var_t *find_var(repl_state_t *state, const char *name) {
    for (int i = 0; i < state->var_count; i++) {
        if (strcmp(state->vars[i].name, name) == 0)
            return &state->vars[i];
    }
    return NULL;
}

static repl_var_t *set_var(repl_state_t *state, const char *name, repl_value_t value) {
    repl_var_t *v = find_var(state, name);
    if (!v) {
        if (state->var_count >= REPL_MAX_VARS) {
            fprintf(stderr, "[repl] too many variables\n");
            return NULL;
        }
        v = &state->vars[state->var_count++];
        strncpy(v->name, name, REPL_MAX_VAR_NAME - 1);
        v->name[REPL_MAX_VAR_NAME - 1] = '\0';
    }
    v->value = value;
    return v;
}

static char *expand_variables(repl_state_t *state, const char *input, char *output, int outsize) {
    int oi = 0;
    int len = (int)strlen(input);

    for (int i = 0; i < len && oi < outsize - 1; i++) {
        if (input[i] == '$') {
            i++;
            char varname[REPL_MAX_VAR_NAME];
            int vi = 0;
            while (i < len && (isalnum((unsigned char)input[i]) || input[i] == '_') && vi < REPL_MAX_VAR_NAME - 1) {
                varname[vi++] = input[i++];
            }
            varname[vi] = '\0';
            i--;

            repl_var_t *v = find_var(state, varname);
            if (v) {
                int vlen = v->value.str_len;
                if (oi + vlen >= outsize) vlen = outsize - oi - 1;
                memcpy(output + oi, v->value.str_val, (size_t)vlen);
                oi += vlen;
            } else {
                int written = snprintf(output + oi, (size_t)(outsize - oi), "$%s", varname);
                if (written > 0) oi += written;
            }
        } else {
            output[oi++] = input[i];
        }
    }
    output[oi] = '\0';
    return output;
}

static int tokenize_line(char *line, char **tokens, int max_tokens) {
    int count = 0;
    char *p = line;

    while (*p && count < max_tokens) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        if (*p == '"') {
            p++;
            tokens[count++] = p;
            while (*p && *p != '"') p++;
            if (*p == '"') *p++ = '\0';
        } else {
            tokens[count++] = p;
            while (*p && !isspace((unsigned char)*p)) p++;
            if (*p) *p++ = '\0';
        }
    }
    return count;
}

static int execute_line(repl_state_t *state, const char *raw_line) {
    char expanded[REPL_MAX_LINE_LEN * 2];
    expand_variables(state, raw_line, expanded, (int)sizeof(expanded));

    char work[REPL_MAX_LINE_LEN * 2];
    strncpy(work, expanded, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';

    char *tokens[64];
    int count = tokenize_line(work, tokens, 64);
    if (count == 0)
        return 0;

    int is_var_assign = 0;
    char var_name[REPL_MAX_VAR_NAME] = {0};
    int cmd_start = 0;

    if (count >= 4 && strcmp(tokens[0], "var") == 0 && strcmp(tokens[2], "=") == 0) {
        is_var_assign = 1;
        strncpy(var_name, tokens[1], REPL_MAX_VAR_NAME - 1);
        cmd_start = 3;
    }

    int cmd_count = count - cmd_start;
    if (cmd_count == 0) {
        if (is_var_assign) {
            repl_value_t empty;
            memset(&empty, 0, sizeof(empty));
            empty.type = VAL_NONE;
            set_var(state, var_name, empty);
        }
        return 0;
    }

    char **cmd_tokens = &tokens[cmd_start];

    if (is_syscall_command(cmd_tokens[0])) {
        repl_value_t result = dispatch_syscall(cmd_tokens[0], cmd_count, cmd_tokens);

        if (is_var_assign) {
            set_var(state, var_name, result);
        } else if (strcmp(cmd_tokens[0], "sys-print") != 0 &&
                   strcmp(cmd_tokens[0], "sys-hex") != 0 &&
                   strcmp(cmd_tokens[0], "fds") != 0 &&
                   strcmp(cmd_tokens[0], "maps") != 0 &&
                   strcmp(cmd_tokens[0], "env") != 0) {
            if (result.type == VAL_INT) {
                printf("%ld\n", result.int_val);
            } else if (result.type == VAL_STRING && result.str_len > 0) {
                printf("%s\n", result.str_val);
            }
        }
    } else {
        fprintf(stderr, "[repl] unknown command: %s\n", cmd_tokens[0]);
        return -1;
    }

    return 0;
}

int repl_execute(repl_state_t *state) {
    int errors = 0;

    for (int i = 0; i < state->line_count; i++) {
        if (execute_line(state, state->lines[i]) < 0)
            errors++;
    }

    repl_reset_block(state);
    return errors;
}
