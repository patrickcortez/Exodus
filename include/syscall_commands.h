#ifndef SYSCALL_COMMANDS_H
#define SYSCALL_COMMANDS_H

#include "kernel_repl.h"

typedef repl_value_t (*syscall_cmd_fn)(int argc, char **argv);

typedef struct {
    const char    *name;
    syscall_cmd_fn func;
} syscall_cmd_entry_t;

repl_value_t dispatch_syscall(const char *name, int argc, char **argv);
void syscall_commands_init(void);

#endif
