#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <vector>

#include "ioctl.h"

namespace noc {

// Represents the hardware resource of PCIe->NOC aperture.
class TlbHandle
{
    int fd;
    int tlb_id;
    uint8_t *tlb_base_uc;
    uint8_t *tlb_base_wc;
    size_t tlb_size;
    tenstorrent_noc_tlb_config tlb_config{};

public:
    TlbHandle(int parent_fd, size_t size, const tenstorrent_noc_tlb_config &config)
        : fd(fcntl(parent_fd, F_DUPFD_CLOEXEC, 0))
        , tlb_size(size)
    {
        if (fd < 0) {
            throw std::system_error(errno, std::generic_category(), "Failed to duplicate file descriptor");
        }

        tenstorrent_allocate_tlb allocate_tlb{};
        allocate_tlb.in.size = size;
        if (ioctl(fd, TENSTORRENT_IOCTL_ALLOCATE_TLB, &allocate_tlb) != 0) {
            close(fd);
            throw std::system_error(errno, std::generic_category(), "Failed to allocate TLB");
        }

        tlb_id = allocate_tlb.out.id;

        try {
            configure(config);

            void *uc = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, allocate_tlb.out.mmap_offset_uc);
            if (uc == MAP_FAILED) {
                throw std::system_error(errno, std::generic_category(), "Failed to map TLB");
            }

            void *wc = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, allocate_tlb.out.mmap_offset_wc);
            if (wc == MAP_FAILED) {
                munmap(uc, size);
                throw std::system_error(errno, std::generic_category(), "Failed to map TLB");
            }

            tlb_base_uc = reinterpret_cast<uint8_t *>(uc);
            tlb_base_wc = reinterpret_cast<uint8_t *>(wc);
        } catch (...) {
            free_tlb();
            close(fd);
            throw;
        }
    }

    ~TlbHandle() noexcept
    {
        munmap(tlb_base_uc, tlb_size);
        munmap(tlb_base_wc, tlb_size);
        free_tlb();
        close(fd);
    }

    void configure(const tenstorrent_noc_tlb_config& new_config)
    {
        tenstorrent_configure_tlb configure_tlb{};
        configure_tlb.in.id = tlb_id;
        configure_tlb.in.config = new_config;

        if (std::memcmp(&new_config, &tlb_config, sizeof(new_config)) == 0) {
            return;
        }

        if (ioctl(fd, TENSTORRENT_IOCTL_CONFIGURE_TLB, &configure_tlb) != 0) {
            throw std::system_error(errno, std::generic_category(), "Failed to configure TLB");
        }

        tlb_config = new_config;
    }

    uint8_t* get_base_uc() { return tlb_base_uc; }
    uint8_t* get_base_wc() { return tlb_base_wc; }
    size_t get_size() const { return tlb_size; }
    const tenstorrent_noc_tlb_config& get_config() const { return tlb_config; }

private:
    void free_tlb() noexcept
    {
        tenstorrent_free_tlb free_tlb{};
        free_tlb.in.id = tlb_id;
        ioctl(fd, TENSTORRENT_IOCTL_FREE_TLB, &free_tlb);
    }
};

// Takes ownership of a TlbHandle to provide memory and register access.
class TlbWindow
{
    std::unique_ptr<TlbHandle> handle;

public:
    enum class CacheMode { UC, WC };

    TlbWindow(std::unique_ptr<TlbHandle> handle)
        : handle(std::move(handle))
    {
    }

    // Memory access with selectable caching
    void write32(uint64_t offset, uint32_t value, CacheMode mode = CacheMode::WC)
    {
        validate(offset, sizeof(uint32_t));
        *reinterpret_cast<volatile uint32_t*>(base(mode) + offset) = value;
    }

    uint32_t read32(uint64_t offset, CacheMode mode = CacheMode::WC)
    {
        validate(offset, sizeof(uint32_t));
        return *reinterpret_cast<volatile uint32_t*>(base(mode) + offset);
    }

    // Register access (always 32-bit, always uncached)
    void write_register(uint64_t offset, uint32_t value)
    {
        write32(offset, value, CacheMode::UC);
    }

    uint32_t read_register(uint64_t offset)
    {
        return read32(offset, CacheMode::UC);
    }

    // Block transfers are limited to 32-bit aligned sizes.
    virtual void write_block(uint64_t offset, const void* data, size_t size, CacheMode mode = CacheMode::WC)
    {
        size_t n = size / sizeof(uint32_t);
        auto* src = static_cast<const uint32_t*>(data);
        auto* dst = reinterpret_cast<volatile uint32_t*>(base(mode) + offset);

        validate(offset, size);

        for (size_t i = 0; i < n; i++) {
            dst[i] = src[i];
        }
    }

    virtual void read_block(uint64_t offset, void* data, size_t size, CacheMode mode = CacheMode::WC)
    {
        size_t n = size / sizeof(uint32_t);
        auto* src = reinterpret_cast<const volatile uint32_t*>(base(mode) + offset);
        auto* dst = static_cast<uint32_t*>(data);

        validate(offset, size);

        for (size_t i = 0; i < n; i++) {
            dst[i] = src[i];
        }
    }

    TlbHandle& handle_ref() { return *handle; }

    virtual size_t get_size() const { return handle->get_size(); }

protected:
    inline virtual uint8_t* base(CacheMode mode)
    {
        return (mode == CacheMode::UC ? handle->get_base_uc() : handle->get_base_wc());
    }

private:
    // For simplicity and correctness, only allow 32-bit aligned accesses.
    // There exist platform and device specific considerations for unaligned
    // accesses which are not addressed here.
    inline void validate(uint64_t offset, size_t size) const
    {
        if ((offset + size) > get_size()) {
            throw std::out_of_range("Out of bounds access");
        }

        if (offset & (sizeof(uint32_t) - 1)) {
            throw std::runtime_error("Bad alignment");
        }
    }
};

// If you want a window abstraction over an address range that doesn't start at
// a boundary aligned with the TLB window size, you can use this class - just
// be aware that the size of the window will be reduced by the offset.
class OffsetTlbWindow : public TlbWindow
{
    const uint64_t base_offset;

public:
    OffsetTlbWindow(std::unique_ptr<TlbHandle> handle, uint64_t offset)
        : TlbWindow(std::move(handle))
        , base_offset(offset)
    {
    }

    virtual size_t get_size() const override
    {
        return TlbWindow::get_size() - base_offset;
    }

protected:
    inline uint8_t* base(CacheMode mode) override
    {
        return TlbWindow::base(mode) + base_offset;
    }
};

class NocAccess
{
    int fd;

public:
    NocAccess(const std::string& chardev_path)
        : fd(open(chardev_path.c_str(), O_RDWR | O_CLOEXEC))
    {
        if (fd < 0) {
            throw std::system_error(errno, std::generic_category(), "Failed to open device");
        }
    }

    ~NocAccess() noexcept
    {
        close(fd);
    }

    void write_register(uint32_t x, uint32_t y, uint64_t address, uint32_t value)
    {
        auto window = map_tlb_2M(x, y, address);
        window->write_register(0, value);
    }

    uint32_t read_register(uint32_t x, uint32_t y, uint64_t address)
    {
        auto window = map_tlb_2M(x, y, address);
        return window->read_register(0);
    }

    void write_block(uint32_t x, uint32_t y, uint64_t address, const void* data, size_t size)
    {
        auto window = map_tlb_2M(x, y, address);
        bool chunked = size > window->get_size();
        if (chunked) {
            throw std::runtime_error("TODO: DMA");
        }

        window->write_block(0, data, size);
    }

    virtual void read_block(uint32_t x, uint32_t y, uint64_t address, void* data, size_t size)
    {
        auto window = map_tlb_2M(x, y, address);
        window->read_block(0, data, size);
    }

    std::unique_ptr<TlbWindow> map_tlb_2M(uint32_t x, uint32_t y, uint64_t address)
    {
        static constexpr uint64_t WINDOW_SIZE = 1 << 21;
        static constexpr uint64_t WINDOW_MASK = WINDOW_SIZE - 1;

        tenstorrent_noc_tlb_config config{
            .addr = address & ~WINDOW_MASK,
            .x_end = x,
            .y_end = y,
        };

        auto handle = std::make_unique<TlbHandle>(fd, WINDOW_SIZE, config);
        auto offset = address & WINDOW_MASK;
        return offset ?
            std::make_unique<OffsetTlbWindow>(std::move(handle), offset) :
            std::make_unique<TlbWindow>(std::move(handle));
    }
};

} // namespace noc

namespace pci {


class Device
{
    int fd;
public:
    Device(const std::string& chardev_path)
        : fd(open(chardev_path.c_str(), O_RDWR | O_CLOEXEC))
    {
    }

    uint64_t map_for_dma(void* buffer, size_t size)
    {
        tenstorrent_pin_pages pin{};
        pin.in.output_size_bytes = sizeof(pin.out);
        pin.in.virtual_address = reinterpret_cast<uint64_t>(buffer);
        pin.in.size = size;

        if (ioctl(fd, TENSTORRENT_IOCTL_PIN_PAGES, &pin) != 0) {
            throw std::system_error(errno, std::generic_category(), "Failed to pin pages");
        }

        return pin.out.physical_address;
    }

    void unmap_for_dma(void *buffer)
    {
        tenstorrent_unpin_pages unpin{};
        unpin.in.virtual_address = reinterpret_cast<uintptr_t>(buffer);

        if (ioctl(fd, TENSTORRENT_IOCTL_UNPIN_PAGES, &unpin) != 0) {
            throw std::system_error(errno, std::generic_category(), "Failed to unpin pages");
        }
    }

    ~Device()
    {
        close(fd);
    }
};

class DmaBuffer
{
    Device& device;
    size_t size;
    void* buffer;
    uint64_t iova;

public:
    DmaBuffer(Device& device, size_t size)
        : device(device)
        , size(size)
        , buffer(std::aligned_alloc(0x1000, size))
        , iova(0)
    {
        if (!buffer) {
            throw std::bad_alloc();
        }

        try {
            iova = device.map_for_dma(buffer, size);
        } catch (...) {
            std::free(buffer);
            throw;
        }
    }

    uint64_t dma_address() const { return iova; }
    uint8_t* data() { return static_cast<uint8_t*>(buffer); }
    size_t length() const { return size; }

    ~DmaBuffer() noexcept
    {
        try {
            device.unmap_for_dma(buffer);
            std::free(buffer);
        } catch (...) {
        }
    }
};
} // namespace pci

namespace detail {

inline void blackhole_noc_sanity_check()
{
    static constexpr uint32_t BH_GRID_X = 17;
    static constexpr uint32_t BH_GRID_Y = 12;
    static constexpr uint64_t NOC_NODE_ID = 0xffb20044ULL;

    noc::NocAccess noc("/dev/tenstorrent/0");

    auto is_tensix = [](uint32_t x, uint32_t y) -> bool {
        return (y >= 2 && y <= 11) &&   // Valid y range
            ((x >= 1 && x <= 7) ||      // Left block
            (x >= 10 && x <= 16));      // Right block
    };

    for (uint32_t x = 0; x < BH_GRID_X; ++x) {
        for (uint32_t y = 0; y < BH_GRID_Y; ++y) {

            if (!is_tensix(x, y))
                continue;

            uint32_t node_id = noc.read_register(x, y, NOC_NODE_ID);
            uint32_t node_id_x = (node_id >> 0x0) & 0x3f;
            uint32_t node_id_y = (node_id >> 0x6) & 0x3f;

            if (node_id_x != x || node_id_y != y) {
                std::cout << "Node ID mismatch at (" << x << ", " << y << "): "
                          << "expected (" << x << ", " << y << "), "
                          << "got (" << node_id_x << ", " << node_id_y << ")" << std::endl;
            }
        }
    }
}

} // namespace detail