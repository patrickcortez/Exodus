/*
 * signals.h
 * Signal handling for the Exodus shell.
 */

#ifndef SIGNALS_H
#define SIGNALS_H

// flags to control signal behavior
extern volatile int sig_interrupt_flag;

// Setup signal handlers
void setup_signals(void);

// Reset signal handlers to default
void reset_signals(void);

#endif // SIGNALS_H
