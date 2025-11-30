#ifndef COKERNEL_INTERRUPTS_H
#define COKERNEL_INTERRUPTS_H

#include <stdint.h>

// Interrupt Numbers (simulated IRQs)
#define CK_IRQ_TIMER    0
#define CK_IRQ_KEYBOARD 1
#define CK_IRQ_MOUSE    2
#define CK_IRQ_NETWORK  3
#define CK_IRQ_MAX      16

// Interrupt Handler Type
typedef void (*ck_interrupt_handler_t)(int irq, void* data);

// Interrupt Management
void ck_interrupts_init(void);
int ck_register_interrupt(int irq, ck_interrupt_handler_t handler);
void ck_raise_interrupt(int irq, void* data);

// Listener Control
void ck_start_interrupt_listener(void);
void ck_stop_interrupt_listener(void);

#endif // COKERNEL_INTERRUPTS_H
