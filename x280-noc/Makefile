obj-m := l2cpu_noc.o

KERNEL_SRC ?= ~/git/linux
ARCH ?= riscv
CROSS_COMPILE ?= riscv64-unknown-linux-gnu-
ROOTFS_PATH ?= /localdev/joelsmith/riscv64-rootfs/root

CXX = $(CROSS_COMPILE)g++
CXXFLAGS = -O2 -static -march=rv64gc

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

user: user.cpp
	$(CXX) $(CXXFLAGS) -o user user.cpp

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) clean
	rm -f user

deploy: all user
	sudo cp l2cpu_noc.ko $(ROOTFS_PATH)/
	sudo cp user $(ROOTFS_PATH)/
	sudo cp dolt.sh $(ROOTFS_PATH)/
	sudo chmod a+x $(ROOTFS_PATH)/dolt.sh

