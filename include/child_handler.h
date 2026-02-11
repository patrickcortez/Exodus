#ifndef EXODUS_CHILD_HANDLER_H
#define EXODUS_CHILD_HANDLER_H

#include <sys/types.h>

/**
 * Runs an interactive command (e.g., node-edit, vim, nano) safely.
 *
 * This function:
 * 1. Temporarily restores the original terminal attributes (cooked mode).
 * 2. Resets signal handlers to default (so the child behaves normally).
 * 3. Forks and executes the command.
 * 4. Waits for the child process to complete.
 * 5. Restores the shell's signal handlers.
 * 6. Re-enables raw mode for the shell.
 *
 * @param command The path to the executable.
 * @param argv NULL-terminated array of arguments (argv[0] should be the command name).
 * @return The exit status of the child, or -1 on error.
 */
int run_interactive_command(const char* command, char* const argv[]);

#endif
