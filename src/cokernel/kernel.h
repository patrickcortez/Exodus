#ifndef COKERNEL_KERNEL_H
#define COKERNEL_KERNEL_H

#include <stddef.h>
#include <stdint.h>

// Co-Kernel Status Codes
typedef enum {
    CK_OK = 0,
    CK_ERR_GENERIC = -1,
    CK_ERR_NO_MEM = -2,
    CK_ERR_INVALID_PID = -3
} ck_status_t;

// Process ID type
typedef int ck_pid_t;

// Initialization and Main Loop
ck_status_t ck_init(void);
void ck_tick(void);
void ck_shutdown(void);

// Basic Kernel Info
const char* ck_get_version(void);
int ck_get_fd(void);

#endif // COKERNEL_KERNEL_H
