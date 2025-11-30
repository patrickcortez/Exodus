#include "syscalls.h"
#include "kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include "../../k-module/cortez_cokernel/cortez_cokernel.h"

long ck_syscall(int sys_id, void* arg1, void* arg2, void* arg3) {
    int fd = ck_get_fd();
    if (fd < 0) {
        // Fallback or error if kernel not connected
        printf("[CoKernel] Error: Kernel not connected (fd < 0)\n");
        return -1;
    }

    ck_syscall_args_t args;
    args.sys_id = sys_id;
    args.arg1 = (unsigned long)arg1;
    args.arg2 = (unsigned long)arg2;
    args.arg3 = (unsigned long)arg3;
    args.result = 0;

    if (ioctl(fd, CK_IOCTL_SYSCALL, &args) < 0) {
        perror("[CoKernel] Syscall ioctl failed");
        return -1;
    }

    return args.result;
}
