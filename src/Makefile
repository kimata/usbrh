# $Id: Makefile 33 2008-07-27 17:47:46Z svn $
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
	rm -f *.o *.ko .*.cmd *.mod.c Module.symvers
	rm -rf .tmp_versions
