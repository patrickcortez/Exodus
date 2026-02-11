/*
 * interrupts.h
 * Advanced input handling: raw mode, history, signals, navigation.
 */

#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#include <stddef.h>
#include "ctz-set.h" // For history storage

// Initializes the history engine.
// Expects: <exe_dir>/data folder to exist, creates if not.
void history_init(const char* exe_dir);

// Saves a command to history.
void history_add(const char* command);

// Closes and saves the history database.
void history_close(void);

// Reads a line of input from the user with full editing capabilities.
// Returns 0 on success (line in buf), -1 on error/EOF, 1 on interrupt (Ctrl+C).
int shell_read_line_robust(char* buf, size_t size, const char* prompt);

void shell_enable_raw_mode(void);
void shell_disable_raw_mode(void);

#endif // INTERRUPTS_H
