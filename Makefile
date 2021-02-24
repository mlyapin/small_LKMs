obj-m := virtrtc.o

KERNELDIR ?= /lib/modules/$(shell uname -r)/build

all: modules
install: Modules_install

modules modules_install help clean:
	$(MAKE) -C $(KERNELDIR) M=$(shell pwd) $@
