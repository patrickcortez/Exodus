/*
 * cortez_su_helper.c
 * A simple SetUID wrapper to execute commands as root.
 *
 * COMPILE:
 * gcc -o cortez_su_helper cortez_su_helper.c
 *
 * SETUP (CRITICAL):
 * 1. sudo chown root:root cortez_su_helper
 * 2. sudo chmod u+s cortez_su_helper
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <command> [args...]\n", argv[0]);
        return 1;
    }

    execvp(argv[1], &argv[1]);

    // If execvp returns, it means an error occurred.
    perror("execvp failed");
    return 127; // Standard exit code for "command not found"
}