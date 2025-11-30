/* lib/cortez_ipc.h
 *
 * Cortez Inter-Process Communication API - Public Header
 *
 * Defines the interface for the IPC library. A tool author includes this
 * header and links against the Cortez IPC static library (-lcortez_ipc).
 */

#ifndef CORTEZ_IPC_H
#define CORTEZ_IPC_H

#include <stddef.h>
#include <stdint.h>

// Enum to identify the type of data in a packet
typedef enum {
    CORTEZ_TYPE_INT = 0x01,
    CORTEZ_TYPE_STRING = 0x02,
    CORTEZ_TYPE_BLOB = 0x03 // For generic raw data
} CortezDataType;

// A structure to hold a single piece of received data.
// It's a linked list node to handle multiple data items.
typedef struct CortezIPCData {
    CortezDataType type;
    size_t length; // Length of the data in the union
    union {
        int32_t int_val;
        char *string_val;
        void *blob_val;
    } data;
    struct CortezIPCData *next;
} CortezIPCData;


/**
 * @brief Sends multiple data items to a target executable via a pipe.
 *
 * @param target_exe The path to the executable to run and send data to.
 * @param ... A variable list of arguments terminated by a 0. Each data item
 * is a type-value pair. Example:
 * cortez_ipc_send("./receiver", CORTEZ_TYPE_STRING, "hello", CORTEZ_TYPE_INT, (int32_t)123, 0);
 * @return 0 on success, -1 on failure.
 */
int cortez_ipc_send(const char *target_exe, ...);

/**
 * @brief Receives and deserializes all data from standard input.
 *
 * @return A pointer to a linked list of CortezIPCData structs, or NULL on failure.
 * The caller is responsible for freeing this list using cortez_ipc_free_data().
 */
CortezIPCData* cortez_ipc_receive();

/**
 * @brief Frees the linked list of data returned by cortez_ipc_receive().
 */
void cortez_ipc_free_data(CortezIPCData *head);

#endif // CORTEZ_IPC_H

