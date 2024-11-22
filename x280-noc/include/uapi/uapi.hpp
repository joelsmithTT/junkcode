/* SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <cstdint>
#include <string.h>

#include "ioctl.h"

#include <array>
#include <cassert>
#include <cstring>
#include <memory>
#include <memory>
#include <stdexcept>

class NocWindow
{
    enum Kind { Size2M, Size128G };

    int fd;
    Kind kind;
    noc_window_handle handle{};
    size_t mapped_size;
    uint8_t* window{nullptr};

public:
    NocWindow(int fd, uint64_t size, noc_window_config config)
        : fd(fd)
        , kind(size <= (1ULL << 21) ? Kind::Size2M : Kind::Size128G)
    {
        if (ioctl(fd, ioctl_alloc[kind], &handle) < 0) {
            throw std::runtime_error("Couldn't allocate window");
        }

        if (size > handle.mmap_size) {
            ioctl(fd, ioctl_dealloc[kind], &handle);
            throw std::runtime_error("Size too large for window");
        }

        config.window_id = handle.window_id;
        if (ioctl(fd, ioctl_config[kind], &config) < 0) {
            ioctl(fd, ioctl_dealloc[kind], &handle);
            throw std::runtime_error("Couldn't configure window");
        }

        void* mem = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, handle.mmap_offset);
        if (mem == MAP_FAILED) {
            ioctl(fd, ioctl_dealloc[kind], &handle);
            throw std::runtime_error("Couldn't map window");
        }

        mapped_size = size;
        window = static_cast<uint8_t*>(mem);
    }

    void reconfigure(noc_window_config config)
    {
        config.window_id = handle.window_id;
        if (ioctl(fd, ioctl_config[kind], &config) < 0) {
            throw std::runtime_error("Couldn't reconfigure window");
        }
    }

    size_t size() const { return mapped_size; }
    uint8_t* data() const { return window; }

    ~NocWindow()
    {
        if (window) {
            munmap(window, mapped_size);
        }
        ioctl(fd, ioctl_dealloc[kind], &handle);
    }

private:
    static constexpr uint32_t ioctl_alloc[] = { L2CPU_IOCTL_ALLOC_2M, L2CPU_IOCTL_ALLOC_128G };
    static constexpr uint32_t ioctl_dealloc[] = { L2CPU_IOCTL_DEALLOC_2M, L2CPU_IOCTL_DEALLOC_128G };
    static constexpr uint32_t ioctl_config[] = { L2CPU_IOCTL_CONFIG_2M, L2CPU_IOCTL_CONFIG_128G };
    static constexpr uint32_t window_shifts[] = { 21, 37 };
};

class NocDriver
{
    int fd;

public:
    NocDriver()
        : fd(open("/dev/l2cpu-noc", O_RDWR))
    {
        if (fd < 0) {
            throw std::runtime_error("Couldn't open driver");
        }
    }

    std::unique_ptr<NocWindow> open_window(uint64_t size, noc_window_config config)
    {
        return std::make_unique<NocWindow>(fd, size, config);
    }

    ~NocDriver()
    {
        if (fd >= 0) {
            close(fd);
        }
    }
};

// TODO: Maybe these things should share ownership of the NocWindow?  i.e.
// make the Driver return shared_ptr<NocWindow> instead of unique_ptr<NocWindow>
class Reader
{
    NocWindow& window;
public:
    Reader(NocWindow& window)
        : window(window)
    {
    }

    uint8_t read8(uint64_t address) { return read<uint8_t>(address); }
    uint16_t read16(uint64_t address) { return read<uint16_t>(address); }
    uint32_t read32(uint64_t address) { return read<uint32_t>(address); }
    uint64_t read64(uint64_t address) { return read<uint64_t>(address); }

    void read_block(uint64_t address, void* dst, size_t size)
    {
        if (address + size > window.size()) {
            throw std::out_of_range("NocWindow: Out of bounds access");
        }

        uint8_t* src = window.data() + address;
        std::memcpy(dst, src, size);
    }

private:
    template <class T>
    T read(uint64_t address)
    {
        auto src = reinterpret_cast<const uintptr_t>(window.data()) + address;

        if (address + sizeof(T) > window.size()) {
            throw std::out_of_range("Reader: Out of bounds access");
        }

        if (alignof(T) > 1 && (src & alignof(T) - 1)) {
            throw std::invalid_argument("Reader: Bad alignment");
        }

        return *reinterpret_cast<const volatile T*>(src);
    }
};

class Writer
{
    NocWindow& window;
public:
    Writer(NocWindow& window)
        : window(window)
    {
    }

    void write8(uint64_t address, uint8_t value) { write<uint8_t>(address, value); }
    void write16(uint64_t address, uint16_t value) { write<uint16_t>(address, value); }
    void write32(uint64_t address, uint32_t value) { write<uint32_t>(address, value); }
    void write64(uint64_t address, uint64_t value) { write<uint64_t>(address, value); }
    void write_block(uint64_t address, const void* src, size_t size)
    {
        if (address + size > window.size()) {
            throw std::out_of_range("Writer: Out of bounds access");
        }

        uint8_t* dst = window.data() + address;
        std::memcpy(dst, src, size);
    }

private:
    template <class T>
    void write(uint64_t address, T value)
    {
        auto dst = reinterpret_cast<uintptr_t>(window.data()) + address;

        if (address + sizeof(T) > window.size()) {
            throw std::out_of_range("Writer: Out of bounds access");
        }

        if (alignof(T) > 1 && (dst & alignof(T) - 1)) {
            throw std::invalid_argument("Writer: Bad alignment");
        }
        *reinterpret_cast<volatile T*>(dst) = value;
    }
};

struct point_t
{
    uint32_t x;
    uint32_t y;

    constexpr point_t(uint32_t x, uint32_t y) : x(x), y(y) {}
};

class Blackhole
{
public:
    static constexpr uint32_t GRID_WIDTH = 17;
    static constexpr uint32_t GRID_HEIGHT = 12;
    static constexpr std::array<point_t, 140> TENSIX_LOCATIONS = {{
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
        { 15, 11 }, { 16, 11 }
    }};

    static constexpr std::array<point_t, 8> DRAM_LOCATIONS = {{
        { 0, 0 }, { 0, 2 }, { 0, 9 }, { 0, 5 },
        { 9, 0 }, { 9, 2 }, { 9, 9 }, { 9, 5 },
    }};

public:
    Blackhole()
        : driver()
    {
    }

    bool is_dram(uint32_t x, uint32_t y) const
    {
        for (const auto& dram : DRAM_LOCATIONS) {
            if (dram.x == x && dram.y == y) {
                return true;
            }
        }
        return false;
    }

    bool is_tensix(uint32_t x, uint32_t y) const
    {
        for (const auto& tensix : TENSIX_LOCATIONS) {
            if (tensix.x == x && tensix.y == y) {
                return true;
            }
        }
        return false;
    }

    void reserve_window(uint32_t x, uint32_t y)
    {
        const size_t size = is_dram(x, y) ? 1ULL << 32 : 1ULL << 21;
        const noc_window_config config{ .x_end = x, .y_end = y, };

        static_windows[index(x, y)] = driver.open_window(size, config);
    }

    void write(uint32_t x, uint32_t y, uint64_t addr, const void* src, size_t size)
    {
        auto& window = static_windows[index(x, y)];

        // If we have no window or the window is too small, use dynamic access.
        if (!window || window->size() < addr + size) {
            return dynamic_write(x, y, addr, src, size);
        }

        Writer writer(*window);
        writer.write_block(addr, src, size);
    }

    void read(uint32_t x, uint32_t y, uint64_t addr, void* dst, size_t size)
    {
        auto& window = static_windows[index(x, y)];

        // If we have no window or the window is too small, use dynamic access.
        if (!window || window->size() < addr + size) {
            return dynamic_read(x, y, addr, dst, size);
        }

        Reader reader(*window);
        reader.read_block(addr, dst, size);
    }

private:
    uint32_t index(uint32_t x, uint32_t y) const
    {
        if (x >= GRID_WIDTH || y >= GRID_HEIGHT) {
            throw std::out_of_range("Blackhole: Out of bounds access");
        }

        return (y * GRID_WIDTH) + x;
    }

    void dynamic_write(uint32_t x, uint32_t y, uint64_t addr, const void* src, size_t size)
    {
        const size_t window_size = is_dram(x, y) ? 1ULL << 32 : 1ULL << 21;
        const uint64_t offset = addr & (window_size - 1);
        noc_window_config config{
            .addr = addr & ~(window_size - 1),
            .x_end = x,
            .y_end = y,
        };

        // TODO: Use a pool of windows.
        auto window = driver.open_window(window_size, config);
        Writer writer(*window);
        writer.write_block(offset, src, size);
    }

    void dynamic_read(uint32_t x, uint32_t y, uint64_t addr, void* dst, size_t size)
    {
        const size_t window_size = is_dram(x, y) ? 1ULL << 32 : 1ULL << 21;
        const uint64_t offset = addr & (window_size - 1);
        noc_window_config config{
            .addr = addr & ~(window_size - 1),
            .x_end = x,
            .y_end = y,
        };

        auto window = driver.open_window(window_size, config);
        Reader reader(*window);
        reader.read_block(offset, dst, size);
    }

    NocDriver driver;
    std::array<std::unique_ptr<NocWindow>, 256> static_windows;
};