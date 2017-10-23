obj-m := dt_overlay.o

KERNEL_SRC ?= /home/eichenberger/projects/nbhw16/linux-stable/

PWD := $(shell pwd)

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) modules

modules_install:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) modules_install

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) clean

.PHONY: all modules_install clean test 
