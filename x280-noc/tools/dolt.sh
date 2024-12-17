insmod l2cpu_noc.ko
cd umd
LD_LIBRARY_PATH=`pwd` ./unit_tests
