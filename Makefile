# Determine architecture
ARCH := $(shell uname -m)


# Set variables based on architecture
ifeq ($(ARCH), x86_64)
    KERNELDIR := /home/richard/work/knet/linux-stable/
    INCLUDE_DIR1 = /home/richard/work/knet/Linux_for_Tegra/source/public/kernel/nvidia/include/
    INCLUDE_DIR2 = /home/richard/work/knet/Linux_for_Tegra/source/public/kernel/nvidia/drivers/video/tegra/host/
    EXTRA_CFLAGS := -I$(INCLUDE_DIR1) -I$(INCLUDE_DIR2)
    obj-m += debug_v4l2.o graph.o camera_version_utils.o
else
    KERNELDIR := /lib/modules/$(shell uname -r)/build
    EXTRA_CFLAGS := -DNVIDIA
    obj-m += debug_v4l2.o graph.o
endif

all:
	make -C $(KERNELDIR) M=$(PWD) EXTRA_CFLAGS="$(EXTRA_CFLAGS)" modules

clean:
	make -C $(KERNELDIR) M=$(PWD) clean

