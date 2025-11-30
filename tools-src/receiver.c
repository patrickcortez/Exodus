#include "cortez_ipc.h"
#include <stdio.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    // The IPC library now needs argc/argv to find the tunnel name
    CortezIPCData *data_head = cortez_ipc_receive(argc, argv);
    
    if (!data_head) {
        fprintf(stderr, "receiver: Failed to receive IPC data.\n");
        return 1;
    }

    printf("--- receiver: received data, printing blob to stdout ---\n");

    CortezIPCData *current = data_head;
    while(current) {
        if (current->type == CORTEZ_TYPE_BLOB) {
            // Write the received blob data to standard output
            write(STDOUT_FILENO, current->data.blob_val, current->length);
        }
        current = current->next;
    }

    cortez_ipc_free_data(data_head);
    return 0;
}
