
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <cstdint>
#include <errno.h>
#include <string.h>

#include "ioctl.h"

#include <cassert>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

struct point_t {
    uint32_t x;
    uint32_t y;
};

bool operator<(const point_t& lhs, const point_t& rhs) {
    if (lhs.x != rhs.x) return lhs.x < rhs.x;
    return lhs.y < rhs.y;
}

static const std::vector<point_t> TENSIX_LOCATIONS {
	{ 1, 2 },   { 2, 2 },	{ 3, 2 },   { 4, 2 },	{ 5, 2 },   { 6, 2 },
	{ 7, 2 },   { 10, 2 },	{ 11, 2 },  { 12, 2 },	{ 13, 2 },  { 14, 2 },
	{ 15, 2 },  { 16, 2 },	{ 1, 3 },   { 2, 3 },	{ 3, 3 },   { 4, 3 },
	{ 5, 3 },   { 6, 3 },	{ 7, 3 },   { 10, 3 },	{ 11, 3 },  { 12, 3 },
	{ 13, 3 },  { 14, 3 },	{ 15, 3 },  { 16, 3 },	{ 1, 4 },   { 2, 4 },
	{ 3, 4 },   { 4, 4 },	{ 5, 4 },   { 6, 4 },	{ 7, 4 },   { 10, 4 },
	{ 11, 4 },  { 12, 4 },	{ 13, 4 },  { 14, 4 },	{ 15, 4 },  { 16, 4 },
	{ 1, 5 },   { 2, 5 },	{ 3, 5 },   { 4, 5 },	{ 5, 5 },   { 6, 5 },
	{ 7, 5 },   { 10, 5 },	{ 11, 5 },  { 12, 5 },	{ 13, 5 },  { 14, 5 },
	{ 15, 5 },  { 16, 5 },	{ 1, 6 },   { 2, 6 },	{ 3, 6 },   { 4, 6 },
	{ 5, 6 },   { 6, 6 },	{ 7, 6 },   { 10, 6 },	{ 11, 6 },  { 12, 6 },
	{ 13, 6 },  { 14, 6 },	{ 15, 6 },  { 16, 6 },	{ 1, 7 },   { 2, 7 },
	{ 3, 7 },   { 4, 7 },	{ 5, 7 },   { 6, 7 },	{ 7, 7 },   { 10, 7 },
	{ 11, 7 },  { 12, 7 },	{ 13, 7 },  { 14, 7 },	{ 15, 7 },  { 16, 7 },
	{ 1, 8 },   { 2, 8 },	{ 3, 8 },   { 4, 8 },	{ 5, 8 },   { 6, 8 },
	{ 7, 8 },   { 10, 8 },	{ 11, 8 },  { 12, 8 },	{ 13, 8 },  { 14, 8 },
	{ 15, 8 },  { 16, 8 },	{ 1, 9 },   { 2, 9 },	{ 3, 9 },   { 4, 9 },
	{ 5, 9 },   { 6, 9 },	{ 7, 9 },   { 10, 9 },	{ 11, 9 },  { 12, 9 },
	{ 13, 9 },  { 14, 9 },	{ 15, 9 },  { 16, 9 },	{ 1, 10 },  { 2, 10 },
	{ 3, 10 },  { 4, 10 },	{ 5, 10 },  { 6, 10 },	{ 7, 10 },  { 10, 10 },
	{ 11, 10 }, { 12, 10 }, { 13, 10 }, { 14, 10 }, { 15, 10 }, { 16, 10 },
	{ 1, 11 },  { 2, 11 },	{ 3, 11 },  { 4, 11 },	{ 5, 11 },  { 6, 11 },
	{ 7, 11 },  { 10, 11 }, { 11, 11 }, { 12, 11 }, { 13, 11 }, { 14, 11 },
	{ 15, 11 }, { 16, 11 },
};

static const std::vector<point_t> DRAM_LOCATIONS {
	{ 0, 0 }, { 0, 2 },
	{ 0, 9 }, { 0, 5 },
	{ 9, 0 }, /* { 9, 2 }, */
	{ 9, 9 }, { 9, 5 },
};

class Xoroshiro128Plus
{
    uint64_t s[2];
public:
    Xoroshiro128Plus(uint64_t seed) {
        s[0] = seed;
        s[1] = 0xdeadbeef;
    }

    uint64_t rotl(const uint64_t x, int k) {
        return (x << k) | (x >> (64 - k));
    }

    uint64_t next() {
        const uint64_t s0 = s[0];
        uint64_t s1 = s[1];
        const uint64_t result = s0 + s1;

        s1 ^= s0;
        s[0] = rotl(s0, 24) ^ s1 ^ (s1 << 16); // a, b
        s[1] = rotl(s1, 37); // c

        return result;
    }
};

class NocWindow
{
    // This is disgusting and I am ashamed.
    // Maybe the sizes should be hidden behind the driver interface.  I don't
    // know.  This is just a test program.
    const std::unordered_map<size_t, uint64_t> ioctl_alloc {
        { 1ULL << 21, L2CPU_IOCTL_ALLOC_2M },
        { 1ULL << 37, L2CPU_IOCTL_ALLOC_128G },
    };
    const std::unordered_map<size_t, uint64_t> ioctl_dealloc {
        { 1ULL << 21, L2CPU_IOCTL_DEALLOC_2M },
        { 1ULL << 37, L2CPU_IOCTL_DEALLOC_128G },
    };
    const std::unordered_map<size_t, uint64_t> ioctl_config {
        { 1ULL << 21, L2CPU_IOCTL_CONFIG_2M },
        { 1ULL << 37, L2CPU_IOCTL_CONFIG_128G },
    };

    int fd;
    const size_t window_size;
    noc_window_handle handle{};
    uint8_t* base{nullptr};

public:
    NocWindow(int fd, size_t size)
        : fd(fd)
        , window_size(size)
    {
        if (ioctl_alloc.find(size) == ioctl_alloc.end()) {
            throw std::runtime_error("Invalid window size");
        }

        if (ioctl(fd, ioctl_alloc.at(window_size), &handle) < 0) {
            throw std::runtime_error("Failed to allocate window");
        }

        // HACK: limit mmap size to 4GB, mapping 128GB takes too long and is
        // basically useless for testing.
        handle.mmap_size = std::min(handle.mmap_size, 4ULL * 1024 * 1024 * 1024);

        void* mapping = mmap(nullptr, handle.mmap_size, PROT_READ | PROT_WRITE,
                          MAP_SHARED, fd, handle.mmap_offset);

        if (mapping == MAP_FAILED) {
            ioctl(fd, ioctl_dealloc.at(window_size), &handle);
            throw std::runtime_error("mmap failed");
        }

        base = static_cast<uint8_t*>(mapping);
    }

    void aim(uint32_t x, uint32_t y, uint64_t addr)
    {
        const uint64_t mask = window_size - 1;

        // TODO: could handle this by aligning the address and tracking the
        // offset into the window... but this is easier for now.

        if (addr & mask) {
            throw std::runtime_error("Bad address alignment");
        }

        noc_window_config config = {
            .window_id = handle.window_id,
            .addr = addr,
            .x_end = x,
            .y_end = y,
        };

        if (ioctl(fd, ioctl_config.at(window_size), &config) < 0) {
            throw std::runtime_error("Failed to configure window");
        }
    }

    size_t size() const { return window_size; }
    void write8(uint64_t address, uint8_t value) { write<uint8_t>(address, value); }
    void write16(uint64_t address, uint16_t value) { write<uint16_t>(address, value); }
    void write32(uint64_t address, uint32_t value) { write<uint32_t>(address, value); }
    void write64(uint64_t address, uint64_t value) { write<uint64_t>(address, value); }
    uint8_t read8(uint64_t address) { return read<uint8_t>(address); }
    uint16_t read16(uint64_t address) { return read<uint16_t>(address); }
    uint32_t read32(uint64_t address) { return read<uint32_t>(address); }
    uint64_t read64(uint64_t address) { return read<uint64_t>(address); }

    void write_block(uint64_t address, const void* buffer, size_t size)
    {
        if (address + size > window_size) {
            throw std::out_of_range("Out of bounds access");
        }

        // TODO: is this safe?  What about alignment?
        std::memcpy(base + address, buffer, size);
    }

    void read_block(uint64_t address, void* buffer, size_t size)
    {
        if (address + size > window_size) {
            throw std::out_of_range("Out of bounds access");
        }

        // TODO: is this safe?  What about alignment?
        std::memcpy(buffer, base + address, size);
    }

    virtual ~NocWindow()
    {
        munmap(base, handle.mmap_size);
        ioctl(fd, ioctl_dealloc.at(window_size), &handle);
    }

private:
    template <class T> void write(uint64_t address, T value)
    {
        auto dst = reinterpret_cast<uintptr_t>(base) + address;

        if (address + sizeof(T) > window_size) {
            throw std::out_of_range("Out of bounds access");
        }

        if (alignof(T) > 1 && (dst & alignof(T) - 1)) {
            throw std::runtime_error("Bad alignment");
        }
        *reinterpret_cast<volatile T*>(dst) = value;
    }

    template <class T> T read(uint64_t address)
    {
        auto src = reinterpret_cast<const uintptr_t>(base) + address;

        if (address + sizeof(T) > window_size) {
            throw std::out_of_range("Out of bounds access");
        }

        if (alignof(T) > 1 && (src & alignof(T) - 1)) {
            throw std::runtime_error("Bad alignment");
        }

        return *reinterpret_cast<const volatile T*>(src);
    }
};

class Driver
{
    int fd;

public:
    Driver()
        : fd(open("/dev/l2cpu-noc", O_RDWR))
    {
        if (fd < 0) {
            throw std::runtime_error("Couldn't open device");
        }
    }

    std::unique_ptr<NocWindow> map_2M(uint32_t x, uint32_t y, uint64_t addr)
    {
        auto window = std::make_unique<NocWindow>(fd, 1ULL << 21);
        window->aim(x, y, addr);
        return window;
    }

    std::unique_ptr<NocWindow> map_128G(uint32_t x, uint32_t y, uint64_t addr)
    {
        auto window = std::make_unique<NocWindow>(fd, 1ULL << 37);
        window->aim(x, y, addr);
        return window;
    }

    // TODO: this isn't very well conceived because NocWindows can outlive the
    // Driver instance.  But it's good enough for now.
    ~Driver()
    {
        close(fd);
    }
};

void test_basic_window_ops()
{
    Driver driver;

    // Test single window allocation and access
    auto window = driver.map_2M(0, 0, 0);
    window->write32(0, 0xdeadbeef);
    assert(window->read32(0) == 0xdeadbeef);
}

void test_many_windows()
{
    Driver driver;
    std::vector<std::unique_ptr<NocWindow>> windows;

    // Test multiple window allocation (should be able to get > 100 2M windows)
    for (int i = 0; i < 100; i++) {
        try {
            windows.push_back(driver.map_2M(0, 0, i * (1<<21)));
        } catch (std::runtime_error& e) {
            std::cerr << "Failed to allocate window " << i << std::endl;
            throw;
        }
    }
}

void test_tensix()
{
    Driver driver;
    const size_t ITERATIONS = 250;

    auto window_128G = driver.map_128G(0, 0, 0);
    for (size_t i = 0; i < ITERATIONS; i++) {
        auto tensix_x = TENSIX_LOCATIONS[i % TENSIX_LOCATIONS.size()].x;
        auto tensix_y = TENSIX_LOCATIONS[i % TENSIX_LOCATIONS.size()].y;
        uint32_t value = 0xbeef0000;

        value |= tensix_x;
        value |= tensix_y << 16;

        auto window_2M = driver.map_2M(tensix_x, tensix_y, 0);
        window_2M->write32(0, value);

        window_128G->aim(tensix_x, tensix_y, 0);
        window_128G->write32(4, 0xffff0000 | value);
    }

    auto window_2M = driver.map_2M(0, 0, 0);
    auto window_128G_v2 = driver.map_128G(0, 0, 0);
    for (const auto& t : TENSIX_LOCATIONS) {
        window_2M->aim(t.x, t.y, 0);
        auto value = window_2M->read32(0);
        uint32_t expected = 0xbeef0000 | t.x | (t.y << 16);
        if (value != expected) {
            throw std::runtime_error("Unexpected value");
        }

        window_128G_v2->aim(t.x, t.y, 0);
        value = window_128G_v2->read32(4);
        expected = 0xffff0000 | t.x | (t.y << 16);
        if (value != expected) {
            throw std::runtime_error("Unexpected value");
        }
    }
}

void test_dram()
{
    Driver driver;
    Xoroshiro128Plus rng(0x17);
    std::vector<std::unique_ptr<NocWindow>> windows;
    auto window_128G = driver.map_128G(0, 0, 0);
    for (size_t i = 0; i < 8; i++) {
        auto dram_x = DRAM_LOCATIONS[i % DRAM_LOCATIONS.size()].x;
        auto dram_y = DRAM_LOCATIONS[i % DRAM_LOCATIONS.size()].y;
        auto window0 = driver.map_2M(dram_x, dram_y, 0);
        auto window1 = driver.map_128G(dram_x, dram_y, 0);

        for (size_t j = 0; j < 8192; j += 8) {
            auto value = rng.next();
            window0->write64(j, value);
            auto readback = window1->read64(j);
            if (value != readback) {
                throw std::runtime_error("DRAM readback mismatch");
            }
        }
        windows.push_back(std::move(window0));
        windows.push_back(std::move(window1));
    }
}

std::vector<std::unique_ptr<NocWindow>> slurp_all_the_2M_tlbs(Driver& driver)
{
    std::vector<std::unique_ptr<NocWindow>> windows;
    for (;;) {
        try {
            windows.push_back(driver.map_2M(0, 0, 0));
        } catch (...) {
            break;
        }
    }
    std::cout << "I have " << windows.size() << " 2M windows" << std::endl;
    return windows;
}

std::vector<std::unique_ptr<NocWindow>> slurp_all_the_128G_tlbs(Driver& driver)
{
    std::vector<std::unique_ptr<NocWindow>> windows;
    for (;;) {
        try {
            windows.push_back(driver.map_128G(0, 0, 0));
        } catch (...) {
            break;
        }
    }
    std::cout << "I have " << windows.size() << " 128G windows" << std::endl;
    return windows;
}

uintptr_t random_address(uintptr_t lo, uintptr_t hi) {
    if (lo >= hi) {
        return lo;
    }

    uintptr_t range = hi - lo;
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uintptr_t> dist(0, range);

    return lo + dist(gen);
}

void test_closing_fd_before_unmap()
{
    int fd = open("/dev/l2cpu-noc", O_RDWR);
    if (fd < 0) {
        throw std::runtime_error("Couldn't open device");
    }

    struct noc_window_handle handle = { 0 };
    if (ioctl(fd, L2CPU_IOCTL_ALLOC_2M, &handle) < 0) {
        throw std::runtime_error("Failed to allocate window");
    }

    struct noc_window_config config = {
        .window_id = handle.window_id,
        .addr = 1 << 21,
        .x_end = 9,
        .y_end = 6,
    };

    void* mapping1 = mmap(nullptr, handle.mmap_size, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, handle.mmap_offset);

    void* mapping2 = mmap(nullptr, handle.mmap_size, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, handle.mmap_offset);

    if (mapping1 == MAP_FAILED) {
        throw std::runtime_error("mmap failed");
    }
    if (mapping2 == MAP_FAILED) {
        throw std::runtime_error("mmap failed");
    }

    close(fd);

    uint32_t* base1 = static_cast<uint32_t*>(mapping1);
    uint32_t* base2 = static_cast<uint32_t*>(mapping2);
    *base1 = 0xdeadbeef;

    if (*base2 != 0xdeadbeef) {
        throw std::runtime_error("Memory not shared");
    }

    munmap(mapping1, handle.mmap_size);
    munmap(mapping2, handle.mmap_size);
}

void test_8(std::vector<std::unique_ptr<NocWindow>>& windows, const std::vector<point_t>& locations, size_t upper, size_t n) {
    Xoroshiro128Plus rng(0x17);
    std::map<std::pair<point_t, uint64_t>, uint8_t> states;
    for (size_t i = 0; i < n; ++i) {
        const auto& noc = locations[i % locations.size()];
        auto& window = windows[i % windows.size()];
        auto address = random_address(0, upper);
        auto value = rng.next() & 0xff;

        window->aim(noc.x, noc.y, 0);
        window->write8(address, value);
        states[{noc, address}] = value;
    }

    for (const auto& state : states) {
        const auto& [noc, address] = state.first;
        const auto value = state.second;
        auto& window = windows[noc.x % windows.size()];

        window->aim(noc.x, noc.y, 0);
        auto readback = window->read8(address);
        if (readback != value) {
            std::cerr << std::dec;
            std::cerr << "Mismatch at (" << noc.x << "," << noc.y << ") 0x" << std::hex << address;
            std::cerr << ": expected 0x" << (int)value << " got 0x" << (int)readback << std::endl;
            std::terminate();
        }
    }
}


void stress_test()
{
    Driver driver;
    auto windows_2M = slurp_all_the_2M_tlbs(driver);
    auto windows_128G = slurp_all_the_128G_tlbs(driver);

    size_t n = 2048;
    size_t total = 0;
    for (;;) {

        // std::cout << "2M 10-6" << std::endl;
        test_8(windows_2M, TENSIX_LOCATIONS, 1<<20, n);
        // std::cout << "2M DRAM" << std::endl;
        test_8(windows_2M, DRAM_LOCATIONS, (1<<21)-1, n);

        // std::cout << "128G 10-6" << std::endl;
        test_8(windows_128G, TENSIX_LOCATIONS, 1<<20, n);
        // std::cout << "128G DRAM" << std::endl;
        test_8(windows_128G, DRAM_LOCATIONS, 0xf0000000, n);

        total += (4 * n);
        std::cout << "Total: " << total << std::endl;
    }
}

void test_bogus_mappings_2M()
{
    int fd = open("/dev/l2cpu-noc", O_RDWR);
    if (fd < 0) {
        throw std::runtime_error("Couldn't open device");
    }

    struct noc_window_handle handle = { 0 };
    if (ioctl(fd, L2CPU_IOCTL_ALLOC_2M, &handle) < 0) {
        throw std::runtime_error("Failed to allocate window");
    }

    struct noc_window_config config = {
        .window_id = handle.window_id,
        .addr = 1 << 21,
        .x_end = 9,
        .y_end = 6,
    };

    if (ioctl(fd, L2CPU_IOCTL_CONFIG_2M, &config) < 0) {
        throw std::runtime_error("Failed to configure window");
    }

    void *mapping1 = mmap(nullptr, handle.mmap_size + 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, handle.mmap_offset);
    if (mapping1 != MAP_FAILED) {
        throw std::runtime_error("Mapping outside window succeeded");
    }

    if (ioctl(fd, L2CPU_IOCTL_DEALLOC_2M, &handle) < 0) {
        throw std::runtime_error("Failed to deallocate window");
    }

    void* mapping2 = mmap(nullptr, handle.mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, handle.mmap_offset);
    if (mapping2 != MAP_FAILED) {
        throw std::runtime_error("Mapping non-owned window succeeded");
    }
    close(fd);
}

void test_bogus_mappings_128G()
{
    int fd = open("/dev/l2cpu-noc", O_RDWR);
    if (fd < 0) {
        throw std::runtime_error("Couldn't open device");
    }

    struct noc_window_handle handle = { 0 };
    if (ioctl(fd, L2CPU_IOCTL_ALLOC_128G, &handle) < 0) {
        throw std::runtime_error("Failed to allocate window");
    }

    struct noc_window_config config = {
        .window_id = handle.window_id,
        .addr = 0,
        .x_end = 9,
        .y_end = 6,
    };
    if (ioctl(fd, L2CPU_IOCTL_CONFIG_128G, &config) < 0) {
        throw std::runtime_error("Failed to configure window");
    }

    void *mapping1 = mmap(nullptr, handle.mmap_size + 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, handle.mmap_offset);
    if (mapping1 != MAP_FAILED) {
        throw std::runtime_error("Mapping outside window succeeded");
    }

    if (ioctl(fd, L2CPU_IOCTL_DEALLOC_128G, &handle) < 0) {
        throw std::runtime_error("Failed to deallocate window");
    }

    void* mapping2 = mmap(nullptr, handle.mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, handle.mmap_offset);
    if (mapping2 != MAP_FAILED) {
        throw std::runtime_error("Mapping non-owned window succeeded");
    }
    close(fd);
}

int main()
{
    std::cout << "test_bogus_mappings_2M" << std::endl;
    test_bogus_mappings_2M();
    std::cout << "test_bogus_mappings_128G" << std::endl;
    test_bogus_mappings_128G();
    std::cout << "test_basic_window_ops" << std::endl;
    test_basic_window_ops();
    std::cout << "test_many_windows" << std::endl;
    test_many_windows();
    std::cout << "test_tensix" << std::endl;
    test_tensix();
    std::cout << "test_dram" << std::endl;
    test_dram();
    std::cout << "test_closing_fd_before_unmap" << std::endl;
    test_closing_fd_before_unmap();
    std::cout << "stress_test" << std::endl;
    stress_test();

    return 0;
}


// TODO:
// 1. Test that we can't map outside of the window
// 2. Test that we can't map a window we don't own