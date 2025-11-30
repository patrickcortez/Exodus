#ifndef COKERNEL_SYSCALLS_H
#define COKERNEL_SYSCALLS_H

#include <stddef.h>

// Syscall Numbers
#define CK_SYS_PRINT    1
#define CK_SYS_EXIT     2
#define CK_SYS_SPAWN    3
#define CK_SYS_READ     4
#define CK_SYS_WRITE    5

// Syscall Interface
// Returns a generic status/result code (0 for success, negative for error usually)
long ck_syscall(int sys_id, void* arg1, void* arg2, void* arg3);

#endif // COKERNEL_SYSCALLS_H
