#include "interrupts.h"
#include "kernel.h"
#include "../../k-module/cortez_cokernel/cortez_cokernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <poll.h>

static ck_interrupt_handler_t g_idt[CK_IRQ_MAX];
static pthread_t g_irq_thread;
static int g_running = 0;

void ck_interrupts_init(void) {
    for (int i = 0; i < CK_IRQ_MAX; ++i) {
        g_idt[i] = NULL;
    }
    printf("[CoKernel] Interrupt Descriptor Table initialized.\n");
}

int ck_register_interrupt(int irq, ck_interrupt_handler_t handler) {
    if (irq < 0 || irq >= CK_IRQ_MAX) return -1;
    g_idt[irq] = handler;
    return 0;
}

void ck_raise_interrupt(int irq, void* data) {
    // In Ring 0 mode, this might be used to simulate SW interrupts locally
    // or we could write to the kernel device to inject it back.
    if (irq < 0 || irq >= CK_IRQ_MAX) return;
    if (g_idt[irq]) {
        g_idt[irq](irq, data);
    }
}

// --- Interrupt Listener Thread ---

static void* irq_listener_loop(void* arg) {
    int fd = ck_get_fd();
    if (fd < 0) return NULL;

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;

    printf("[CoKernel] Interrupt Listener Thread started.\n");

    while (g_running) {
        int ret = poll(&pfd, 1, 1000); // 1s timeout
        if (ret > 0) {
            if (pfd.revents & POLLIN) {
                ck_interrupt_t irq;
                if (read(fd, &irq, sizeof(irq)) == sizeof(irq)) {
                    // Dispatch
                    // printf("[CoKernel] Received IRQ %d\n", irq.irq);
                    if (irq.irq >= 0 && irq.irq < CK_IRQ_MAX && g_idt[irq.irq]) {
                        g_idt[irq.irq](irq.irq, (void*)irq.data);
                    }
                }
            }
        }
    }
    return NULL;
}

void ck_start_interrupt_listener(void) {
    if (g_running) return;
    g_running = 1;
    pthread_create(&g_irq_thread, NULL, irq_listener_loop, NULL);
}

void ck_stop_interrupt_listener(void) {
    g_running = 0;
    pthread_join(g_irq_thread, NULL);
}
