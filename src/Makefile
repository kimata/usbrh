obj-m           := usbrh.o
cmd-depmod      := depmod
KERNELDIR       ?= /lib/modules/$(shell uname -r)/build
PWD             := $(shell pwd)

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install
	depmod

clean:
	rm -f *.o *.ko .*.cmd *.mod.c Module.symvers modules.order
	rm -rf .tmp_versions
