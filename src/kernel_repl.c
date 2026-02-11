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
    state->func_count = 0;
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
    for (int i = state->var_count - 1; i >= 0; i--) {
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

static void remove_local_vars(repl_state_t *state, int saved_count) {
    state->var_count = saved_count;
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
                if (v->value.type == VAL_INT) {
                    int written = snprintf(output + oi, (size_t)(outsize - oi), "%ld", v->value.int_val);
                    if (written > 0) oi += written;
                } else {
                    int vlen = v->value.str_len;
                    if (oi + vlen >= outsize) vlen = outsize - oi - 1;
                    memcpy(output + oi, v->value.str_val, (size_t)vlen);
                    oi += vlen;
                }
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

static int parse_literal(const char *token, repl_value_t *val) {
    if (!token || !*token) return 0;

    if (token[0] == '"') {
        val->type = VAL_STRING;
        size_t tlen = strlen(token);
        if (tlen > 1 && token[tlen - 1] == '"') tlen--;
        const char *src = token + 1;
        size_t slen = tlen - 1;
        if (slen >= REPL_MAX_VAR_VALUE) slen = REPL_MAX_VAR_VALUE - 1;
        memcpy(val->str_val, src, slen);
        val->str_val[slen] = '\0';
        val->str_len = (int)slen;
        return 1;
    }

    if (strcmp(token, "true") == 0) {
        val->type = VAL_INT;
        val->int_val = 1;
        return 1;
    }
    if (strcmp(token, "false") == 0) {
        val->type = VAL_INT;
        val->int_val = 0;
        return 1;
    }
    if (strcmp(token, "null") == 0) {
        val->type = VAL_NONE;
        val->int_val = 0;
        return 1;
    }

    char *endptr = NULL;
    long num = 0;

    if (token[0] == '0' && (token[1] == 'x' || token[1] == 'X')) {
        num = strtol(token, &endptr, 16);
        if (endptr && *endptr == '\0') {
            val->type = VAL_INT;
            val->int_val = num;
            snprintf(val->str_val, REPL_MAX_VAR_VALUE, "0x%lx", num);
            val->str_len = (int)strlen(val->str_val);
            return 1;
        }
    }

    if (token[0] == '0' && (token[1] == 'b' || token[1] == 'B')) {
        num = strtol(token + 2, &endptr, 2);
        if (endptr && *endptr == '\0') {
            val->type = VAL_INT;
            val->int_val = num;
            snprintf(val->str_val, REPL_MAX_VAR_VALUE, "0b%ld", num);
            val->str_len = (int)strlen(val->str_val);
            return 1;
        }
    }

    int is_number = 1;
    int start = 0;
    if (token[0] == '-' || token[0] == '+') start = 1;
    if (token[start] == '\0') is_number = 0;
    for (int i = start; token[i] && is_number; i++) {
        if (!isdigit((unsigned char)token[i])) is_number = 0;
    }

    if (is_number) {
        num = strtol(token, &endptr, 10);
        if (endptr && *endptr == '\0') {
            val->type = VAL_INT;
            val->int_val = num;
            snprintf(val->str_val, REPL_MAX_VAR_VALUE, "%ld", num);
            val->str_len = (int)strlen(val->str_val);
            return 1;
        }
    }

    return 0;
}

static int try_parse_string_literal(const char *raw_rhs, repl_value_t *val) {
    while (*raw_rhs && isspace((unsigned char)*raw_rhs)) raw_rhs++;
    if (*raw_rhs != '"') return 0;

    const char *start = raw_rhs + 1;
    const char *end = strrchr(start, '"');
    if (!end) return 0;

    size_t slen = (size_t)(end - start);
    if (slen >= REPL_MAX_VAR_VALUE) slen = REPL_MAX_VAR_VALUE - 1;
    val->type = VAL_STRING;
    memcpy(val->str_val, start, slen);
    val->str_val[slen] = '\0';
    val->str_len = (int)slen;
    return 1;
}

static repl_func_t *find_func(repl_state_t *state, const char *name) {
    for (int i = 0; i < state->func_count; i++) {
        if (strcmp(state->funcs[i].name, name) == 0)
            return &state->funcs[i];
    }
    return NULL;
}

// Forward declaration
static int execute_block(repl_state_t *state, char lines[][REPL_MAX_LINE_LEN], int start, int end);

static void push_var(repl_state_t *state, const char *name, repl_value_t value) {
    if (state->var_count >= REPL_MAX_VARS) {
        fprintf(stderr, "[repl] too many variables\n");
        return;
    }
    repl_var_t *v = &state->vars[state->var_count++];
    strncpy(v->name, name, REPL_MAX_VAR_NAME - 1);
    v->name[REPL_MAX_VAR_NAME - 1] = '\0';
    v->value = value;
}

static int call_func(repl_state_t *state, repl_func_t *fn, int argc, char **argv) {
    int saved_var_count = state->var_count;

    for (int i = 0; i < fn->param_count && i < argc; i++) {
        repl_value_t pval;
        memset(&pval, 0, sizeof(pval));
        if (!parse_literal(argv[i], &pval)) {
            pval.type = VAL_STRING;
            strncpy(pval.str_val, argv[i], REPL_MAX_VAR_VALUE - 1);
            pval.str_len = (int)strlen(pval.str_val);
        }
        push_var(state, fn->params[i], pval);
    }

    // Execute function body as a block to support control flow
    int errors = execute_block(state, fn->body, 0, fn->body_count);

    remove_local_vars(state, saved_var_count);
    return errors;
}

static long resolve_value(repl_state_t *state, const char *token) {
    if (token[0] == '$') {
        repl_var_t *v = find_var(state, token + 1);
        if (v && v->value.type == VAL_INT) return v->value.int_val;
        if (v && v->value.type == VAL_STRING) return atol(v->value.str_val);
        return 0;
    }

    repl_value_t lit;
    memset(&lit, 0, sizeof(lit));
    if (parse_literal(token, &lit) && lit.type == VAL_INT)
        return lit.int_val;

    return atol(token);
}

static int is_numeric_string(const char *s) {
    if (!s || !*s) return 0;
    char *endptr;
    strtol(s, &endptr, 0);
    return *endptr == '\0';
}

static int eval_condition(repl_state_t *state, char **tokens, int count) {
    if (count < 3) return 0;

    char expanded_left[REPL_MAX_LINE_LEN];
    char expanded_right[REPL_MAX_LINE_LEN];
    expand_variables(state, tokens[0], expanded_left, (int)sizeof(expanded_left));
    expand_variables(state, tokens[2], expanded_right, (int)sizeof(expanded_right));

    const char *op = tokens[1];

    if (is_numeric_string(expanded_left) && is_numeric_string(expanded_right)) {
        long left = resolve_value(state, expanded_left);
        long right = resolve_value(state, expanded_right);

        if (strcmp(op, "==") == 0) return left == right;
        if (strcmp(op, "!=") == 0) return left != right;
        if (strcmp(op, ">") == 0)  return left > right;
        if (strcmp(op, "<") == 0)  return left < right;
        if (strcmp(op, ">=") == 0) return left >= right;
        if (strcmp(op, "<=") == 0) return left <= right;
    } else {
        // String comparison
        if (strcmp(op, "==") == 0) return strcmp(expanded_left, expanded_right) == 0;
        if (strcmp(op, "!=") == 0) return strcmp(expanded_left, expanded_right) != 0;
    }

    return 0;
}

static int execute_line(repl_state_t *state, const char *raw_line) {
    while (*raw_line && isspace((unsigned char)*raw_line)) raw_line++;
    if (!*raw_line || raw_line[0] == '#') return 0;

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

    if (count >= 3 && strcmp(tokens[0], "var") == 0 && strcmp(tokens[2], "=") == 0) {
        is_var_assign = 1;
        strncpy(var_name, tokens[1], REPL_MAX_VAR_NAME - 1);
        cmd_start = 3;
    }

    if (is_var_assign) {
        // Check for arithmetic: var x = A op B
        if (count - cmd_start >= 3) {
            const char *op = tokens[cmd_start + 1];
            if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0 || 
                strcmp(op, "*") == 0 || strcmp(op, "/") == 0 || strcmp(op, "%") == 0) {
                
                long val1 = resolve_value(state, tokens[cmd_start]);
                long val2 = resolve_value(state, tokens[cmd_start + 2]);
                long res = 0;
                
                if (strcmp(op, "+") == 0) res = val1 + val2;
                else if (strcmp(op, "-") == 0) res = val1 - val2;
                else if (strcmp(op, "*") == 0) res = val1 * val2;
                else if (strcmp(op, "/") == 0) res = (val2 != 0) ? (val1 / val2) : 0;
                else if (strcmp(op, "%") == 0) res = (val2 != 0) ? (val1 % val2) : 0;

                repl_value_t r;
                memset(&r, 0, sizeof(r));
                r.type = VAL_INT;
                r.int_val = res;
                snprintf(r.str_val, sizeof(r.str_val), "%ld", res);
                r.str_len = (int)strlen(r.str_val);
                set_var(state, var_name, r);
                return 0;
            }
        }

        if (cmd_start >= count) {
            repl_value_t empty;
            memset(&empty, 0, sizeof(empty));
            empty.type = VAL_NONE;
            set_var(state, var_name, empty);
            return 0;
        }

        char rhs_buf[REPL_MAX_LINE_LEN * 2] = {0};
        const char *eq_pos = strstr(raw_line, "=");
        if (eq_pos) {
            eq_pos++;
            char expanded_rhs[REPL_MAX_LINE_LEN * 2];
            expand_variables(state, eq_pos, expanded_rhs, (int)sizeof(expanded_rhs));
            strncpy(rhs_buf, expanded_rhs, sizeof(rhs_buf) - 1);
        }

        repl_value_t lit_val;
        memset(&lit_val, 0, sizeof(lit_val));
        if (try_parse_string_literal(rhs_buf, &lit_val)) {
            set_var(state, var_name, lit_val);
            return 0;
        }

        if (parse_literal(tokens[cmd_start], &lit_val)) {
            set_var(state, var_name, lit_val);
            return 0;
        }
    }

    int cmd_count = count - cmd_start;
    if (cmd_count == 0)
        return 0;

    char **cmd_tokens = &tokens[cmd_start];

    if (is_syscall_command(cmd_tokens[0])) {
        repl_value_t result = dispatch_syscall(cmd_tokens[0], cmd_count, cmd_tokens);

        if (is_var_assign) {
            set_var(state, var_name, result);
        } else {
            if (result.type == VAL_INT) {
                printf("%ld\n", result.int_val);
            } else if (result.type == VAL_STRING && result.str_len > 0) {
                printf("%s\n", result.str_val);
            }
        }
        return 0;
    }

    repl_func_t *fn = find_func(state, cmd_tokens[0]);
    if (fn) {
        return call_func(state, fn, cmd_count - 1, &cmd_tokens[1]);
    }

    fprintf(stderr, "[repl] unknown command: %s\n", cmd_tokens[0]);
    return -1;
}

static int check_keyword(const char *line, const char *kw) {
    size_t len = strlen(kw);
    if (strncmp(line, kw, len) != 0) return 0;
    char next = line[len];
    return next == '\0' || isspace((unsigned char)next);
}

static int find_matching_end(char lines[][REPL_MAX_LINE_LEN], int start, int total) {
    int depth = 0;
    for (int i = start; i < total; i++) {
        char trimmed[REPL_MAX_LINE_LEN];
        strncpy(trimmed, lines[i], REPL_MAX_LINE_LEN - 1);
        trimmed[REPL_MAX_LINE_LEN - 1] = '\0';
        char *p = trimmed;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p || p[0] == '#') continue;

        if (check_keyword(p, "if") || check_keyword(p, "fn") || check_keyword(p, "while"))
            depth++;
        else if (check_keyword(p, "end")) {
            if (depth == 0) return i;
            depth--;
        }
    }
    return total;
}

static int execute_block(repl_state_t *state, char lines[][REPL_MAX_LINE_LEN], int start, int end);

static int handle_if_block(repl_state_t *state, char lines[][REPL_MAX_LINE_LEN], int if_line, int end_line) {
    typedef struct { int start; int end; int is_else; char cond_line[REPL_MAX_LINE_LEN]; } branch_t;
    branch_t branches[64];
    int branch_count = 0;

    branches[0].start = if_line + 1;
    branches[0].is_else = 0;
    strncpy(branches[0].cond_line, lines[if_line], REPL_MAX_LINE_LEN - 1);
    branch_count = 1;

    for (int i = if_line + 1; i < end_line; i++) {
        char *p = lines[i];
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p || p[0] == '#') continue;

        int nested_depth = 0;
        for (int j = if_line + 1; j < i; j++) {
            char *q = lines[j];
            while (*q && isspace((unsigned char)*q)) q++;
            if (!*q || q[0] == '#') continue;
            if (check_keyword(q, "if")) nested_depth++;
            else if (check_keyword(q, "end")) {
                if (nested_depth > 0) nested_depth--;
            }
        }
        if (nested_depth > 0) continue;

        if (check_keyword(p, "else if")) {
            branches[branch_count - 1].end = i;
            branches[branch_count].start = i + 1;
            branches[branch_count].is_else = 0;
            strncpy(branches[branch_count].cond_line, p, REPL_MAX_LINE_LEN - 1);
            branch_count++;
        } else if (check_keyword(p, "else")) {
            branches[branch_count - 1].end = i;
            branches[branch_count].start = i + 1;
            branches[branch_count].end = end_line;
            branches[branch_count].is_else = 1;
            branches[branch_count].cond_line[0] = '\0';
            branch_count++;
        }
    }
    branches[branch_count - 1].end = end_line;

    for (int b = 0; b < branch_count; b++) {
        if (branches[b].is_else) {
            execute_block(state, lines, branches[b].start, branches[b].end);
            return 0;
        }

        char cond_copy[REPL_MAX_LINE_LEN];
        strncpy(cond_copy, branches[b].cond_line, REPL_MAX_LINE_LEN - 1);
        cond_copy[REPL_MAX_LINE_LEN - 1] = '\0';

        char *p = cond_copy;
        while (*p && isspace((unsigned char)*p)) p++;
        if (check_keyword(p, "if")) p += 2;
        else if (check_keyword(p, "else if")) p += 7;

        char expanded_cond[REPL_MAX_LINE_LEN * 2];
        expand_variables(state, p, expanded_cond, (int)sizeof(expanded_cond));

        char cond_work[REPL_MAX_LINE_LEN * 2];
        strncpy(cond_work, expanded_cond, sizeof(cond_work) - 1);
        cond_work[sizeof(cond_work) - 1] = '\0';

        char *cond_tokens[64];
        int cond_count = tokenize_line(cond_work, cond_tokens, 64);

        if (eval_condition(state, cond_tokens, cond_count)) {
            execute_block(state, lines, branches[b].start, branches[b].end);
            return 0;
        }
    }

    return 0;
}

static int execute_block(repl_state_t *state, char lines[][REPL_MAX_LINE_LEN], int start, int end) {
    int errors = 0;
    int i = start;

    while (i < end) {
        char *p = lines[i];
        while (*p && isspace((unsigned char)*p)) p++;

        if (!*p || p[0] == '#') {
            i++;
            continue;
        }

        if (check_keyword(p, "fn")) {
            char fn_work[REPL_MAX_LINE_LEN];
            strncpy(fn_work, p + 2, sizeof(fn_work) - 1);
            fn_work[sizeof(fn_work) - 1] = '\0';

            char *fn_tokens[64];
            int fn_tok_count = tokenize_line(fn_work, fn_tokens, 64);

            if (fn_tok_count < 1) {
                fprintf(stderr, "[repl] fn requires a name\n");
                i++;
                errors++;
                continue;
            }

            int fn_end = find_matching_end(lines, i + 1, end);
            if (fn_end >= end) {
                fprintf(stderr, "[repl] fn '%s' missing 'end'\n", fn_tokens[0]);
                i++;
                errors++;
                continue;
            }

            if (state->func_count >= REPL_MAX_FUNCS) {
                fprintf(stderr, "[repl] too many functions\n");
                i = fn_end + 1;
                continue;
            }

            repl_func_t *fn_slot = find_func(state, fn_tokens[0]);
            if (!fn_slot) {
                fn_slot = &state->funcs[state->func_count++];
            }
            memset(fn_slot, 0, sizeof(repl_func_t));
            strncpy(fn_slot->name, fn_tokens[0], REPL_MAX_VAR_NAME - 1);

            fn_slot->param_count = 0;
            for (int pi = 1; pi < fn_tok_count && fn_slot->param_count < REPL_MAX_FUNC_PARAMS; pi++) {
                strncpy(fn_slot->params[fn_slot->param_count], fn_tokens[pi], REPL_MAX_VAR_NAME - 1);
                fn_slot->param_count++;
            }

            fn_slot->body_count = 0;
            for (int bi = i + 1; bi < fn_end && fn_slot->body_count < REPL_MAX_FUNC_LINES; bi++) {
                strncpy(fn_slot->body[fn_slot->body_count], lines[bi], REPL_MAX_LINE_LEN - 1);
                fn_slot->body_count++;
            }

            i = fn_end + 1;
            continue;
        }

        if (check_keyword(p, "if")) {
            int if_end = find_matching_end(lines, i + 1, end);
            if (if_end >= end) {
                fprintf(stderr, "[repl] if missing 'end'\n");
                i++;
                errors++;
                continue;
            }

            handle_if_block(state, lines, i, if_end);
            i = if_end + 1;
            continue;
        }

        if (check_keyword(p, "while")) {
            int while_end = find_matching_end(lines, i + 1, end);
            if (while_end >= end) {
                fprintf(stderr, "[repl] while missing 'end'\n");
                i++;
                errors++;
                continue;
            }

            while (1) {
                char cond_work[REPL_MAX_LINE_LEN];
                strncpy(cond_work, lines[i], REPL_MAX_LINE_LEN - 1);
                cond_work[REPL_MAX_LINE_LEN - 1] = '\0';
                
                char *cp = cond_work;
                while (*cp && isspace((unsigned char)*cp)) cp++;
                if (check_keyword(cp, "while")) cp += 5;

                char expanded_cond[REPL_MAX_LINE_LEN * 2];
                expand_variables(state, cp, expanded_cond, (int)sizeof(expanded_cond));
                
                char *cond_tokens[64];
                int cond_count = tokenize_line(expanded_cond, cond_tokens, 64);

                if (!eval_condition(state, cond_tokens, cond_count)) {
                    break;
                }
                
                execute_block(state, lines, i + 1, while_end);
            }

            i = while_end + 1;
            continue;
        }

        if (execute_line(state, lines[i]) < 0)
            errors++;
        i++;
    }

    return errors;
}

int repl_execute(repl_state_t *state) {
    int errors = execute_block(state, state->lines, 0, state->line_count);
    repl_reset_block(state);
    return errors;
}

void repl_run_script(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        fprintf(stderr, "[repl] could not open script: %s\n", filename);
        return;
    }

    repl_state_t state;
    repl_init(&state);

    char line[REPL_MAX_LINE_LEN];
    int executed = 0;

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[len - 1] = '\0';
            len--;
        }

        if (len == 0) continue;

        if (strcmp(line, "[EXECUTE]") == 0) {
            if (state.line_count > 0) {
                repl_execute(&state);
                executed = 1;
            }
            continue;
        }

        repl_add_line(&state, line);
    }
    fclose(f);

    if (!executed) {
        fprintf(stderr, "[repl] script error: missing [EXECUTE] command\n");
    }
}
