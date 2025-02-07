obj-m += l2cpu_net.o

KERNEL_SRC ?= ~/git/linux
CROSS_COMPILE ?= riscv64-unknown-linux-gnu-

all:
	make ARCH=riscv CROSS_COMPILE=$(CROSS_COMPILE) -C $(KERNEL_SRC) M=$(PWD) modules

clean:
	make -C $(KERNEL_SRC) M=$(PWD) clean
