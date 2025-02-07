obj-m := l2cpu_noc.o

KERNEL_SRC ?= ~/git/linux
ARCH ?= riscv
CROSS_COMPILE ?= riscv64-unknown-linux-gnu-
ROOTFS_PATH ?= /home/joel/scrappy/riscv64-rootfs/root

CXX = $(CROSS_COMPILE)g++
CXX_LOCAL = g++
CXXFLAGS = -std=c++17 -O3 -static -march=rv64gcv -I$(PWD)/include/uapi
CXXFLAGS_LOCAL = -std=c++17 -O3 -I$(PWD)/include/uapi

# Tell kbuild where our source files are
l2cpu_noc-y := driver/l2cpu_noc.o
ccflags-y := -I$(PWD)/include/uapi

TEST_SRCS = tests/test.cpp
TEST_BINS = tests/test
TEST_DEPS = $(TEST_SRCS) $(wildcard include/uapi/*.h)

all: modules $(TEST_BINS)

dev: tests/test.local

modules:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

tests/test: tests/test.cpp $(TEST_DEPS)
	$(CXX) $(CXXFLAGS) -o tests/test tests/test.cpp

tests/test.local: tests/test.cpp $(TEST_DEPS)
	$(CXX_LOCAL) $(CXXFLAGS_LOCAL) -o tests/test.local tests/test.cpp

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) clean
	rm -f $(TEST_BINS) tests/*.local

deploy: all
	sudo cp l2cpu_noc.ko $(ROOTFS_PATH)/
	sudo cp $(TEST_BINS) $(ROOTFS_PATH)/

.PHONY: all dev modules clean deploy
