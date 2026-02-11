#include "child_handler.h"
#include "interrupts.h" // For enable/disable_raw_mode
#include "signals.h"    // For setup_signals, if available
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

// Extern declarations if not in header (checked: they are in interrupts.h now)

int run_interactive_command(const char* command, char* const argv[]) {
    shell_disable_raw_mode();

    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);

        execv(command, argv);
        
        perror("execv failed");
        exit(1);
    } else if (pid < 0) {
        perror("fork failed");
        // Re-enable raw mode before returning
        shell_enable_raw_mode();
        return -1;
    }

    // --- PARENT ---
    // Ignore signals while waiting for child
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    int status;
    waitpid(pid, &status, 0);

    shell_enable_raw_mode();
    setup_signals(); // Restore shell signal handlers

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}
