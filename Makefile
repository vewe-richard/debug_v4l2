# Determine architecture
ARCH := $(shell uname -m)

# Set variables based on architecture
ifeq ($(ARCH), x86_64)
    KERNELDIR := /home/richard/work/knet/linux-stable/
    EXTRA_CFLAGS := 
else
    KERNELDIR := /lib/modules/$(shell uname -r)/build
    EXTRA_CFLAGS := -DNVIDIA
endif

# Common Makefile directives
obj-m += debug_v4l2.o

all:
	make -C $(KERNELDIR) M=$(PWD) EXTRA_CFLAGS="$(EXTRA_CFLAGS)" modules

clean:
	make -C $(KERNELDIR) M=$(PWD) clean

