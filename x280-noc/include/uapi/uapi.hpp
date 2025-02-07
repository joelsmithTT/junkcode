/* SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <cstring>

#include "ioctl.h"

constexpr size_t FOUR_GIGS = 1ULL << 32;
constexpr size_t TWO_MEGS = 1ULL << 21;

class NocWindow
{
    enum Kind { Size2M, Size128G };

    int fd;
    Kind kind;
    noc_window_handle handle{};
    noc_window_config config{};
    size_t mapped_size;
    uint8_t* window{nullptr};

public:
    NocWindow(int fd, uint64_t size, noc_window_config initial_config)
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

        initial_config.window_id = handle.window_id;
        if (ioctl(fd, ioctl_config[kind], &initial_config) < 0) {
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
        config = initial_config;
    }

    void reconfigure(noc_window_config new_config)
    {
        new_config.window_id = handle.window_id;

        if (memcmp(&new_config, &config, sizeof(noc_window_config)) == 0) {
            return;
        }

        if (ioctl(fd, ioctl_config[kind], &new_config) < 0) {
            throw std::runtime_error("Couldn't reconfigure window");
        }

        config = new_config;
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
};

class SmartNocWindow
{
    NocWindow window;

    // This is used to keep settings like ordering, posted writes, etc. for the
    // lifetime of the window, even if it is hopped around while it is used.
    const noc_window_config initial_config;
public:
    SmartNocWindow(int fd, uint64_t size, noc_window_config initial_config = {})
        : window(fd, size, initial_config)
        , initial_config(initial_config)
    {
    }

    size_t size() const { return window.size(); }
    void* window_base() const { return window.data(); }

    uint32_t read32(uint32_t x, uint32_t y, uint64_t addr)
    {
        uint32_t value;
        read_block(x, y, addr, &value, sizeof(value));
        return value;
    }

    void write32(uint32_t x, uint32_t y, uint64_t addr, uint32_t val)
    {
        write_block(x, y, addr, &val, sizeof(val));
    }

    void read_block(uint32_t x, uint32_t y, uint64_t addr, void* dst, size_t size)
    {
        size_t window_size = window.size();
        uint64_t window_addr = addr & ~(window_size - 1);
        uint64_t offset = addr & (window_size - 1);
        void* src = window.data() + offset;

        if (size > window_size) {
            throw std::runtime_error("Read size too large");
        }

        if (size + offset > window_size) {
            throw std::runtime_error("Read crosses window boundary");
        }

        if (size <= 8 && (addr & (size - 1))) {
            throw std::runtime_error("Unaligned read");
        }

        noc_window_config config = initial_config;
        config.addr = window_addr;
        config.x_end = x;
        config.y_end = y;

        window.reconfigure(config);

        switch (size) {
        case 1:
            *reinterpret_cast<uint8_t*>(dst) = *reinterpret_cast<volatile uint8_t*>(src);
            break;
        case 2:
            *reinterpret_cast<uint16_t*>(dst) = *reinterpret_cast<volatile uint16_t*>(src);
            break;
        case 4:
            *reinterpret_cast<uint32_t*>(dst) = *reinterpret_cast<volatile uint32_t*>(src);
            break;
        case 8:
            *reinterpret_cast<uint64_t*>(dst) = *reinterpret_cast<volatile uint64_t*>(src);
            break;
        default:
            memcpy(dst, src, size);
            break;
        }
    }

    void write_block(uint32_t x, uint32_t y, uint64_t addr, const void* src, size_t size)
    {
        size_t window_size = window.size();
        uint64_t window_addr = addr & ~(window_size - 1);
        uint64_t offset = addr & (window_size - 1);
        void* dst = window.data() + offset;

        if (size > FOUR_GIGS) {
            throw std::runtime_error("Read size too large");
        }

        if (size + offset > window_size) {
            throw std::runtime_error("Read crosses window boundary");
        }

        if (size <= 8 && (addr & (size - 1))) {
            throw std::runtime_error("Unaligned write");
        }

        noc_window_config config = initial_config;
        config.addr = window_addr;
        config.x_end = x;
        config.y_end = y;

        window.reconfigure(config);

        switch (size) {
        case 1:
            *reinterpret_cast<volatile uint8_t*>(dst) = *reinterpret_cast<const uint8_t*>(src);
            break;
        case 2:
            *reinterpret_cast<volatile uint16_t*>(dst) = *reinterpret_cast<const uint16_t*>(src);
            break;
        case 4:
            *reinterpret_cast<volatile uint32_t*>(dst) = *reinterpret_cast<const uint32_t*>(src);
            break;
        case 8:
            *reinterpret_cast<volatile uint64_t*>(dst) = *reinterpret_cast<const uint64_t*>(src);
            break;
        default:
            memcpy(dst, src, size);
            break;
        }
    }
};

class NOC
{
    int fd;

public:
    NOC()
        : fd(open("/dev/l2cpu-noc", O_RDWR))
    {
        if (fd < 0) {
            throw std::runtime_error("Couldn't open driver");
        }
    }

    std::unique_ptr<SmartNocWindow> open_window_2M(noc_window_config config = {})
    {
        return std::make_unique<SmartNocWindow>(fd, TWO_MEGS, config);
    }

    std::unique_ptr<SmartNocWindow> open_window_4G(noc_window_config config = {})
    {
        return std::make_unique<SmartNocWindow>(fd, FOUR_GIGS, config);
    }

    uint32_t read32(uint32_t x, uint32_t y, uint64_t addr)
    {
        return open_window_2M()->read32(x, y, addr);
    }

    void write32(uint32_t x, uint32_t y, uint64_t addr, uint32_t val)
    {
        open_window_2M()->write32(x, y, addr, val);
    }

    void read_block(uint32_t x, uint32_t y, uint64_t addr, void* dst, size_t size)
    {
        if (size <= TWO_MEGS) {
            open_window_2M()->read_block(x, y, addr, dst, size);
        } else {
            open_window_4G()->read_block(x, y, addr, dst, size);
        }
    }

    void write_block(uint32_t x, uint32_t y, uint64_t addr, const void* src, size_t size)
    {
        if (size <= TWO_MEGS) {
            open_window_2M()->write_block(x, y, addr, src, size);
        } else {
            open_window_4G()->write_block(x, y, addr, src, size);
        }
    }

    ~NOC()
    {
        if (fd >= 0) {
            close(fd);
        }
    }
};

struct xy_t {
    uint32_t x, y;
    constexpr xy_t(uint32_t x_, uint32_t y_) : x(x_), y(y_) {}
};

template<std::size_t... Is>
constexpr auto make_tensix_locations(std::index_sequence<Is...>) {
    return std::array<xy_t, sizeof...(Is)>{
        xy_t{
            ((Is % 14) < 7) ? (1 + (Is % 7)) : (10 + (Is % 7)), // X
            2 + (Is / 14)                                       // Y
        }...
    };
}

struct Blackhole
{
    static constexpr uint32_t GRID_WIDTH = 17;
    static constexpr uint32_t GRID_HEIGHT = 12;
    static constexpr auto TENSIX_COUNT = 140;
    static constexpr auto TENSIX_LOCATIONS = make_tensix_locations(std::make_index_sequence<TENSIX_COUNT>{});
    static constexpr std::array<xy_t, 24> DRAM_LOCATIONS_ALL {{
        { 0, 0 }, { 0, 1 }, { 0, 11 },
        { 0, 2 }, { 0, 10 }, { 0, 3 },
        { 0, 9 }, { 0, 4 }, { 0, 8 },
        { 0, 5 }, { 0, 7 }, { 0, 6 },
        { 9, 0 }, { 9, 1 }, { 9, 11 },
        { 9, 2 }, { 9, 10 }, { 9, 3 },
        { 9, 9 }, { 9, 4 }, { 9, 8 },
        { 9, 5 }, { 9, 7 }, { 9, 6 }
    }};
    static constexpr std::array<xy_t, 8> DRAM_LOCATIONS {{
        { 0, 0 }, { 0, 2 }, { 0, 9 }, { 0, 5 },
        { 9, 0 }, { 9, 2 }, { 9, 9 }, { 9, 5 },
    }};

    static constexpr bool is_dram(uint32_t x, uint32_t y)
    {
        for (const auto& dram : DRAM_LOCATIONS_ALL) {
            if (dram.x == x && dram.y == y) {
                return true;
            }
        }
        return false;
    }

    static constexpr bool is_tensix(uint32_t x, uint32_t y)
    {
        return (y >= 2 && y <= 11) &&     // Valid y range
               ((x >= 1 && x <= 7) ||     // Left block
                (x >= 10 && x <= 16));    // Right block
    }
};

class Tensix
{
    NOC& noc;
    uint32_t x, y;
    std::unique_ptr<SmartNocWindow> window;
public:
    static constexpr uint64_t NOC_NODE_ID   = 0xffb20044ULL;
    static constexpr uint64_t RESET         = 0xffb121b0ULL;

    Tensix(NOC& noc, uint32_t x, uint32_t y)
        : noc(noc)
        , x(x)
        , y(y)
        , window(noc.open_window_2M())
    {
        if (!Blackhole::is_tensix(x, y)) {
            throw std::runtime_error("Invalid tensix location");
        }

        auto node_id = window->read32(x, y, NOC_NODE_ID);
        auto node_x = (node_id >> 0) & 0x3f;
        auto node_y = (node_id >> 6) & 0x3f;

        if (node_x != x || node_y != y) {
            throw std::runtime_error("Invalid tensix node id");
        }
    }
};