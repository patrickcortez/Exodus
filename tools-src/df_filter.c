//gcc -Wall digest-filter.c cortez_ipc.o -o digest-filter

#include "cortez_ipc.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[]) {
    CortezIPCData *data_head = cortez_ipc_receive(argc, argv);
    if (!data_head || !data_head->next) {
        fprintf(stderr, "df-filter: Did not receive enough data (expected blob and string).\n");
        cortez_ipc_free_data(data_head);
        return 1;
    }

    CortezIPCData *blob_node = data_head;
    CortezIPCData *string_node = data_head->next;

    if (blob_node->type != CORTEZ_TYPE_BLOB || string_node->type != CORTEZ_TYPE_STRING) {
        fprintf(stderr, "df-filter: Received data in the wrong format.\n");
        cortez_ipc_free_data(data_head);
        return 1;
    }

    char *file_content = (char*)blob_node->data.blob_val;
    // We need to ensure the content is null-terminated for strtok_r
    file_content[blob_node->length - 1] = '\0';
    char *keyword = string_node->data.string_val;

    printf("Filtered lines containing \"%s\":\n", keyword);
    printf("----------------------------------------\n");

    // Use strtok_r for safe line-by-line tokenizing
    char *saveptr;
    char *line = strtok_r(file_content, "\n", &saveptr);

    while (line != NULL) {
        if (strstr(line, keyword)) {
            printf("%s\n", line);
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    printf("----------------------------------------\n");


    cortez_ipc_free_data(data_head);
    return 0;
}
