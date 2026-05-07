obj-m := vpipe.o
vpipe-objs := vpipe-m2m.o vpipe-meta.o vpipe-cv.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	if [ -d "$(KDIR)" ]; then \
		$(MAKE) -C $(KDIR) M=$(PWD) clean; \
	else \
		rm -f .*.cmd .*.o *.o *.ko *.mod *.mod.c modules.order Module.symvers; \
	fi
