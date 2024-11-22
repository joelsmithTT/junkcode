obj-m := l2cpu_noc.o

KERNEL_SRC ?= ~/git/linux
ARCH ?= riscv
CROSS_COMPILE ?= riscv64-unknown-linux-gnu-
ROOTFS_PATH ?= /home/joel/scrappy/riscv64-rootfs/root

CXX = $(CROSS_COMPILE)g++
CXXFLAGS = -O3 -static -march=rv64gcv -I$(PWD)/include/uapi

# Tell kbuild where our source files are
l2cpu_noc-y := driver/l2cpu_noc.o
ccflags-y := -I$(PWD)/include/uapi

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

test:
	$(CXX) $(CXXFLAGS) -o tests/test tests/test.cpp
	$(CXX) $(CXXFLAGS) -o tests/uapi_test tests/uapi_test.cpp
#	$(CXX) $(CXXFLAGS) -o tests/benchmark tests/benchmark.cpp

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) clean
	rm -f tests/test tests/uapi_test

deploy: all test
	sudo cp l2cpu_noc.ko $(ROOTFS_PATH)/
#	sudo cp tests/benchmark $(ROOTFS_PATH)/
	sudo cp tests/test $(ROOTFS_PATH)/
	sudo cp tests/uapi_test $(ROOTFS_PATH)/
	sudo cp tools/dolt.sh $(ROOTFS_PATH)/
	sudo cp tools/umd-dolt.sh $(ROOTFS_PATH)/
	sudo mkdir -p $(ROOTFS_PATH)/umd
	sudo cp /home/joel/git/tt-umd/cmake-build/device/libdevice.so $(ROOTFS_PATH)/umd/
	sudo cp /home/joel/git/tt-umd/cmake-build/test/umd/x280/unit_tests $(ROOTFS_PATH)/umd/
	sudo cp /home/joel/git/tt-umd/tests/soc_descs/blackhole_x280.yaml $(ROOTFS_PATH)/
	sudo chmod a+x $(ROOTFS_PATH)/dolt.sh
	sudo chmod a+x $(ROOTFS_PATH)/umd-dolt.sh
