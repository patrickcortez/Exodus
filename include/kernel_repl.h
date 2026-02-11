#ifndef KERNEL_REPL_H
#define KERNEL_REPL_H

#define REPL_MAX_LINES       256
#define REPL_MAX_LINE_LEN    1024
#define REPL_MAX_VARS        128
#define REPL_MAX_VAR_NAME    64
#define REPL_MAX_VAR_VALUE   4096
#define REPL_MAX_FUNCS       32
#define REPL_MAX_FUNC_PARAMS 8
#define REPL_MAX_FUNC_LINES  128

typedef enum {
    VAL_NONE,
    VAL_INT,
    VAL_STRING,
    VAL_LIST,
    VAL_BYTES
} repl_val_type_t;

typedef struct {
    repl_val_type_t type;
    long            int_val;
    char            str_val[REPL_MAX_VAR_VALUE];
    int             str_len;
    char            **list_items;
    int             list_count;
} repl_value_t;

typedef struct {
    char          name[REPL_MAX_VAR_NAME];
    repl_value_t  value;
} repl_var_t;

typedef struct {
    char name[REPL_MAX_VAR_NAME];
    char params[REPL_MAX_FUNC_PARAMS][REPL_MAX_VAR_NAME];
    int  param_count;
    char body[REPL_MAX_FUNC_LINES][REPL_MAX_LINE_LEN];
    int  body_count;
} repl_func_t;

typedef struct {
    char         lines[REPL_MAX_LINES][REPL_MAX_LINE_LEN];
    int          line_count;
    repl_var_t   vars[REPL_MAX_VARS];
    int          var_count;
    int          in_block;
    repl_func_t  funcs[REPL_MAX_FUNCS];
    int          func_count;
} repl_state_t;

void repl_init(repl_state_t *state);
void repl_reset_block(repl_state_t *state);
void repl_add_line(repl_state_t *state, const char *line);
int  repl_execute(repl_state_t *state);
int  is_syscall_command(const char *cmd);

// Executes a script file (.atom)
void repl_run_script(const char *filename);

#endif
