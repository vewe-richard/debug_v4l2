obj-m +=  debug_v4l2.o

all:
	make -C /home/richard/work/knet/linux-stable/ M=$(PWD) modules

clean:
	make -C /home/richard/work/knet/linux-stable/ M=$(PWD) clean
