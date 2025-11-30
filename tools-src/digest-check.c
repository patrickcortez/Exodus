/* tools/digest-check.c
 *
 * Receives a filename and a binary checksum blob via Cortez IPC
 * and displays them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "cortez_ipc.h"

int main(int argc, char *argv[]) {
    CortezIPCData *data_head = cortez_ipc_receive();
    if (!data_head) {
        fprintf(stderr, "digest-check: No data received.\n");
        return 1;
    }

    // We expect exactly two data nodes: a string and a blob.
    CortezIPCData *file_node = data_head;
    CortezIPCData *digest_node = file_node ? file_node->next : NULL;

    if (!file_node || file_node->type != CORTEZ_TYPE_STRING ||
        !digest_node || digest_node->type != CORTEZ_TYPE_BLOB) {
        fprintf(stderr, "digest-check: Received malformed data packet.\n");
        cortez_ipc_free_data(data_head);
        return 1;
    }
    
    printf("--- Received File Digest ---\n");
    printf("  File    : %s\n", file_node->data.string_val);
    printf("  Checksum: 0x");

    // Cast the void* blob data to unsigned bytes to print them
    uint8_t *checksum_bytes = (uint8_t *)digest_node->data.blob_val;
    for (size_t i = 0; i < digest_node->length; ++i) {
        printf("%02x", checksum_bytes[i]); // Print each byte as a 2-digit hex number
    }
    printf("\n");
    printf("----------------------------\n");

    cortez_ipc_free_data(data_head);
    return 0;
}