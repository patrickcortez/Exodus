/* tools/file-digest.c
 *
 * Reads a file, computes a Fletcher-32 checksum of its contents,
 * and sends the filename and checksum to the 'digest-check' tool.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include "cortez_ipc.h"
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

// Computes a Fletcher-32 checksum.
uint32_t fletcher32(const uint8_t *data, size_t len) {
    uint32_t sum1 = 0xffff, sum2 = 0xffff;
    while (len) {
        size_t tlen = len > 360 ? 360 : len;
        len -= tlen;
        do {
            sum1 += *data++;
            sum2 += sum1;
        } while (--tlen);
        sum1 = (sum1 & 0xffff) + (sum1 >> 16);
        sum2 = (sum2 & 0xffff) + (sum2 >> 16);
    }
    sum1 = (sum1 & 0xffff) + (sum1 >> 16);
    sum2 = (sum2 & 0xffff) + (sum2 >> 16);
    return sum2 << 16 | sum1;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: file-digest <filename>\n");
        return 1;
    }
    const char *filename = argv[1];

    // --- Read the target file into a buffer ---
    FILE *f = fopen(filename, "rb");
    if (!f) {
        perror("Failed to open file");
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *buffer = malloc(file_size);
    if (!buffer) {
        fprintf(stderr, "Failed to allocate memory\n");
        fclose(f);
        return 1;
    }

    if (fread(buffer, 1, file_size, f) != file_size) {
        fprintf(stderr, "Failed to read file\n");
        free(buffer);
        fclose(f);
        return 1;
    }
    fclose(f);

    // --- Compute the checksum ---
    printf("Calculating digest for '%s'...\n", filename);
    uint32_t checksum = fletcher32(buffer, file_size);
    free(buffer);

    char self_path[PATH_MAX];
    char receiver_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);

    if (len != -1) {
        self_path[len] = '\0';
        char *last_slash = strrchr(self_path, '/');
        if (last_slash) {
            *last_slash = '\0'; // The path is now the directory: /.../root/tools
            // Build the correct, absolute path to the receiver program
            snprintf(receiver_path, sizeof(receiver_path), "%s/digest-check", self_path);
        } else {
            // Fallback if path is unusual
            strcpy(receiver_path, "digest-check");
        }
    } else {
        // Fallback if readlink fails (e.g., non-Linux system)
        perror("readlink");
        strcpy(receiver_path, "digest-check");
    }

    // --- Send the filename (string) and checksum (blob) ---
    int result = cortez_ipc_send(
        receiver_path,
        CORTEZ_TYPE_STRING, filename,
        CORTEZ_TYPE_BLOB,   (size_t)sizeof(checksum), &checksum, // Send the 4-byte checksum as a blob
        0 // Terminator
    );

    if (result == -1) {
        fprintf(stderr, "file-digest: Failed to send IPC data.\n");
        return 1;
    }

    printf("Digest sent successfully.\n");
    return 0;
}