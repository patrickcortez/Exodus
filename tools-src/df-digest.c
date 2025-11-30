//gcc -Wall df-digest.c cortez_ipc.o -o df-digest
#include "cortez_ipc.h"
#include <stdio.h>
#include <stdint.h>

// A simple but fast checksum algorithm (djb2)
unsigned long hash_djb2(const unsigned char *str, size_t len) {
    unsigned long hash = 5381;
    for (size_t i = 0; i < len; i++) {
        hash = ((hash << 5) + hash) + str[i]; // hash * 33 + c
    }
    return hash;
}

int main(int argc, char *argv[]) {
    CortezIPCData *data_head = cortez_ipc_receive(argc, argv);
    if (!data_head) {
        fprintf(stderr, "df-digest: Failed to receive IPC data.\n");
        return 1;
    }

    if (data_head->type == CORTEZ_TYPE_BLOB) {
        unsigned char *file_content = (unsigned char *)data_head->data.blob_val;
        size_t content_length = data_head->length;

        unsigned long checksum = hash_djb2(file_content, content_length);

        printf("File Checksum (djb2): %lu\n", checksum);
    } else {
        fprintf(stderr, "df-digest: Expected a data blob but received another type.\n");
    }

    cortez_ipc_free_data(data_head);
    return 0;
}
