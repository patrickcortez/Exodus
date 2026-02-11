#include "signals.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

volatile int sig_interrupt_flag = 0;

static void handle_sigint(int sig) {
    (void)sig;
    sig_interrupt_flag = 1;
    write(STDOUT_FILENO, "\n", 1);
}

static void handle_sig_ign(int sig) {
    (void)sig;
}

void setup_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    
    // SIGINT: Set flag and print newline
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; 
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction SIGINT");
    }

    // SIGQUIT (^\): Ignore to prevent dropping core
    sa.sa_handler = handle_sig_ign;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGQUIT, &sa, NULL);

    // SIGTSTP (^Z): Ignore to prevent shell from suspending itself
    sa.sa_handler = handle_sig_ign;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTSTP, &sa, NULL);

    // SIGTERM: Handle gracefully (optional, but good for robustness)
    sa.sa_handler = handle_sigint; // Reuse interrupt logic for now
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
}

void reset_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGTSTP, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}
