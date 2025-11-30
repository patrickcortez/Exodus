#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "../nodefs/nodefs.h"

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <disk_image> <nodefs_filename> <host_output_path>\n", argv[0]);
        return 1;
    }

    const char* disk_path = argv[1];
    const char* node_filename = argv[2];
    const char* host_path = argv[3];

    printf("Exporting '%s' from '%s' to '%s'...\n", node_filename, disk_path, host_path);

    if (nodefs_mount(disk_path) < 0) {
        fprintf(stderr, "Failed to mount disk image.\n");
        return 1;
    }

    int node_id = nodefs_find_node(node_filename);
    if (node_id < 0) {
        fprintf(stderr, "File '%s' not found in NodeFS.\n", node_filename);
        nodefs_unmount();
        return 1;
    }

    // Stream file content
    const size_t CHUNK_SIZE = 4096;
    char* buffer = malloc(CHUNK_SIZE);
    if (!buffer) {
        fprintf(stderr, "Memory allocation failed.\n");
        nodefs_unmount();
        return 1;
    }

    uint64_t offset = 0;
    int total_bytes = 0;
    int host_fd = open(host_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (host_fd < 0) {
        perror("Failed to open host file for writing");
        free(buffer);
        nodefs_unmount();
        return 1;
    }

    while (1) {
        int bytes_read = nodefs_read_data(node_id, offset, buffer, CHUNK_SIZE);
        if (bytes_read < 0) {
            fprintf(stderr, "Failed to read file data at offset %lu.\n", offset);
            close(host_fd);
            free(buffer);
            nodefs_unmount();
            return 1;
        }
        
        if (bytes_read == 0) break; // EOF

        if (write(host_fd, buffer, bytes_read) != bytes_read) {
            perror("Failed to write data to host file");
            close(host_fd);
            free(buffer);
            nodefs_unmount();
            return 1;
        }
        
        offset += bytes_read;
        total_bytes += bytes_read;
    }

    printf("Successfully exported %d bytes.\n", total_bytes);

    close(host_fd);
    free(buffer);
    nodefs_unmount();
    return 0;
}
