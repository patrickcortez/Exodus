#include "kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <unistd.h>
#include "interrupts.h"

// Internal Kernel State
typedef struct {
    int running;
    uint64_t tick_count;
    int fd; // File descriptor for /dev/cortez_cokernel
} CoKernel;

static CoKernel g_kernel = {0};

ck_status_t ck_init(void) {
    if (g_kernel.running) {
        return CK_ERR_GENERIC; // Already initialized
    }

    g_kernel.fd = open("/dev/cortez_cokernel", O_RDWR);
    if (g_kernel.fd < 0) {
        perror("[CoKernel] Failed to open /dev/cortez_cokernel");
        printf("[CoKernel] Make sure the kernel module is loaded!\n");
        return CK_ERR_GENERIC;
    }

    g_kernel.running = 1;
    g_kernel.tick_count = 0;

    ck_start_interrupt_listener();

    printf("[CoKernel] Initialized Ring 0 Connection. Version: %s\n", ck_get_version());
    return CK_OK;
}

void ck_tick(void) {
    if (!g_kernel.running) return;

    g_kernel.tick_count++;
    // Future: Schedule processes, handle deferred interrupts
}

void ck_shutdown(void) {
    g_kernel.running = 0;
    printf("[CoKernel] Shutting down.\n");
}

const char* ck_get_version(void) {
    return "0.1.0-alpha";
}

int ck_get_fd(void) {
    return g_kernel.fd;
}
