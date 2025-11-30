obj-m += cortez_root.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install: cortez_root.ko
	sudo insmod cortez_root.ko

uninstall:
	sudo rmmod cortez_root