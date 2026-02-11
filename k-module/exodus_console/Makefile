# Build: make
# Load: sudo insmod exodus_console.ko
# Unload: sudo rmmod exodus_console

obj-m += exodus_console.o

all: module

module:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
