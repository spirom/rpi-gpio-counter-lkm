obj-m := gpiocount.o

PWD       := $(shell pwd)

EXTRA_CFLAGS := -std=gnu99 -Wno-declaration-after-statement

all:
	echo PWD=$(PWD)
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD)

modules_install:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) modules_install

