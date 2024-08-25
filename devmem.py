import os, mmap, struct
addr = 0x2030a00000

f = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
mem = mmap.mmap(f, 4096, mmap.MAP_SHARED, mmap.PROT_READ, offset=addr)
x = mem[0:4]
print(hex(struct.unpack('I', x)[0]))

"""
import os, mmap, struct
addrs = [0x2030a00000,0x8c030000000 ]
f = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
for addr in addrs:
    mem = mmap.mmap(f, 4096, mmap.MAP_SHARED, mmap.PROT_READ, offset=addr)
    x = mem[0:4]
    print(hex(struct.unpack('I', x)[0]))
"""