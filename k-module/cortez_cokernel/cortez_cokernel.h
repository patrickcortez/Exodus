#ifndef CORTEZ_COKERNEL_SHARED_H
#define CORTEZ_COKERNEL_SHARED_H

#include <linux/ioctl.h>

#define COKERNEL_DEVICE_NAME "cortez_cokernel"
#define COKERNEL_CLASS_NAME  "cortez"

// Interrupt Event Structure
typedef struct {
    int irq;
    unsigned long data;
    unsigned long timestamp;
} ck_interrupt_t;

// Syscall Arguments Structure
typedef struct {
    int sys_id;
    unsigned long arg1;
    unsigned long arg2;
    unsigned long arg3;
    long result;
} ck_syscall_args_t;

// IOCTL Commands
#define CK_IOCTL_MAGIC 'k'
#define CK_IOCTL_SYSCALL      _IOWR(CK_IOCTL_MAGIC, 1, ck_syscall_args_t)
#define CK_IOCTL_REGISTER_IRQ _IO(CK_IOCTL_MAGIC, 2)

// Syscall Numbers (Must match user-space)
#define CK_SYS_PRINT    1
#define CK_SYS_EXIT     2
#define CK_SYS_SPAWN    3
#define CK_SYS_READ     4
#define CK_SYS_WRITE    5

#endif // CORTEZ_COKERNEL_SHARED_H
