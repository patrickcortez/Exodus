#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "../src/nodefs/nodefs.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("Usage: import_bin <host_file> <nodefs_name>\n");
        return 1;
    }

    if (nodefs_mount("cortez_drive.img") < 0) {
        printf("Failed to mount NodeFS\n");
        return 1;
    }

    // Read host file
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror("Failed to open host file"); return 1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = malloc(size);
    fread(buf, 1, size, f);
    fclose(f);

    // Create Node
    int parent_id = 0; // Root
    int id = nodefs_create_node(parent_id, argv[2], NODE_TYPE_FILE);
    if (id < 0) {
        printf("Failed to create node '%s'\n", argv[2]);
        free(buf);
        return 1;
    }

    // Write Data
    if (nodefs_write_data(id, buf, size) < 0) {
        printf("Failed to write data to node %d\n", id);
    } else {
        printf("Imported %s to NodeFS as '%s' (ID: %d, Size: %ld)\n", argv[1], argv[2], id, size);
    }

    free(buf);
    return 0;
}
