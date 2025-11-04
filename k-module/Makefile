# Makefile for the Cortez Tunnel Kernel Module and Userspace Programs

# --- Userspace Program Configuration ---
CC=gcc
CFLAGS=-Wall -O2
LDFLAGS=
USERSPACE_TARGETS=sender receiver

# --- Kernel Module Configuration ---
# The 'obj-m' syntax is specific to kernel module Makefiles.
# It tells the kernel build system to compile the source file as a loadable module.
obj-m += cortez_tunnel.o

# --- Build Targets ---

# The default target, 'all', depends on building the module and the userspace apps.
all: module $(USERSPACE_TARGETS)

# Target to build the kernel module.
# This command invokes the main kernel Makefile, which knows how to build modules.
# -C: Change directory to the kernel build directory.
# M=$(PWD): Specifies the location of our external module source code.
module:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

# Generic rule to build the userspace programs.
# This is a standard C compilation command.
$(USERSPACE_TARGETS): %: %.c cortez_tunnel_shared.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Target to clean up all generated files.
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
	rm -f $(USERSPACE_TARGETS)

