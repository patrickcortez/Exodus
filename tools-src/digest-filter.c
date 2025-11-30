//gcc -Wall digest-filter.c cortez_ipc.o -o digest-filter
#include "cortez_ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <filename> <keyword>\n", argv[0]);
        return 1;
    }

    const char *filename = argv[1];
    const char *keyword = argv[2];

    // 1. Open and read the entire file into a memory buffer
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        perror("fopen");
        return 1;
    }

    struct stat st;
    if (stat(filename, &st) != 0) {
        perror("stat");
        fclose(fp);
        return 1;
    }
    size_t file_size = st.st_size;

    char *file_buffer = malloc(file_size);
    if (!file_buffer) {
        fprintf(stderr, "Failed to allocate %zu bytes for file.\n", file_size);
        fclose(fp);
        return 1;
    }

    if (fread(file_buffer, 1, file_size, fp) != file_size) {
        fprintf(stderr, "Failed to read the entire file.\n");
        fclose(fp);
        free(file_buffer);
        return 1;
    }
    fclose(fp);

    printf("--- digest-filter: Read %zu bytes from '%s'. Dispatching to workers. ---\n\n", file_size, filename);

    // 2. Dispatch the entire file buffer to the digest worker
    printf("--- Dispatching to df-digest... ---\n");
    cortez_ipc_send("./df-digest", CORTEZ_TYPE_BLOB, file_size, file_buffer, 0);
    printf("--- df-digest finished. ---\n\n");


    // 3. Dispatch the same buffer and the keyword to the filter worker
    printf("--- Dispatching to df-filter... ---\n");
    cortez_ipc_send("./df-filter",
                    CORTEZ_TYPE_BLOB, file_size, file_buffer,
                    CORTEZ_TYPE_STRING, keyword,
                    0);
    printf("--- df-filter finished. ---\n");


    // 4. Clean up
    free(file_buffer);
    return 0;
}
