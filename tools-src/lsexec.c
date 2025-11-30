#include "cortez_ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define READ_BUF_SIZE 4096

int main() {
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        return 1;
    }

    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        return 1;
    }

    if (pid == 0) { // Child process to run "ls -l"
        close(pipefd[0]); // Close unused read end
        dup2(pipefd[1], STDOUT_FILENO); // Redirect stdout to the pipe
        close(pipefd[1]);

        execlp("ls", "ls", "-l", (char *)NULL);
        perror("execlp ls"); // Should not be reached
        _exit(1);

    } else { // Parent process captures output
        close(pipefd[1]); // Close unused write end

        // Read all output from ls into a dynamic buffer
        char *ls_output = NULL;
        size_t total_size = 0;
        size_t capacity = 0;
        char buffer[READ_BUF_SIZE];
        ssize_t bytes_read;

        while ((bytes_read = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
            if (total_size + bytes_read > capacity) {
                // A simple growth strategy
                capacity = (total_size + bytes_read) * 2;
                ls_output = realloc(ls_output, capacity);
                if (!ls_output) {
                    perror("realloc");
                    return 1;
                }
            }
            memcpy(ls_output + total_size, buffer, bytes_read);
            total_size += bytes_read;
        }

        close(pipefd[0]);
        wait(NULL); // Wait for the "ls" child to finish

        if (ls_output) {
            printf("--- lsexec: sending %zu bytes to receiver via Cortez Tunnel ---\n\n", total_size);
            // Use the IPC library to send the captured output to the receiver
            cortez_ipc_send("./receiver", CORTEZ_TYPE_BLOB, total_size, ls_output, 0);
            free(ls_output);
        } else {
             printf("--- lsexec: 'ls -l' produced no output to send ---\n");
        }
    }
    return 0;
}
