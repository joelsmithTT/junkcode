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

namespace pci {

inline tenstorrent_mapping get_mapping(int fd, int id)
{
    static const size_t NUM_MAPPINGS = 8; // TODO(jms) magic 8
    struct
    {
        tenstorrent_query_mappings query_mappings{};
        tenstorrent_mapping mapping_array[NUM_MAPPINGS];
    } mappings;

    mappings.query_mappings.in.output_mapping_count = NUM_MAPPINGS;

    ioctl(fd, TENSTORRENT_IOCTL_QUERY_MAPPINGS, &mappings.query_mappings);

    for (size_t i = 0; i < NUM_MAPPINGS; i++) {
        if (mappings.mapping_array[i].mapping_id == id) {
            return mappings.mapping_array[i];
        }
    }

    throw std::runtime_error("Unknown mapping");
}

inline uint8_t* map_bar2(int fd, size_t size)
{
    auto uc_resource = get_mapping(fd, TENSTORRENT_MAPPING_RESOURCE1_UC); // BAR2 is index 1
    void* bar2 = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, uc_resource.mapping_base);

    if (bar2 == MAP_FAILED) {
        throw std::system_error(errno, std::generic_category(), "Failed to map BAR2");
    }

    if (size != uc_resource.mapping_size) {
        throw std::runtime_error("BAR2 size mismatch");
    }

    return static_cast<uint8_t*>(bar2);
}

static uint8_t* map_bar0(int fd, size_t size)
{
    auto wc_resource = get_mapping(fd, TENSTORRENT_MAPPING_RESOURCE0_WC);
    auto uc_resource = get_mapping(fd, TENSTORRENT_MAPPING_RESOURCE0_UC);

    // There exists a convention that BAR0 is divided into write-combined (lower) and uncached (upper) mappings.
    auto wc_size = (156 * (1 << 20)) + (10 * (1 << 21)) + (19 * (1 << 24));
    auto uc_size = uc_resource.mapping_size - wc_size;
    auto wc_offset = 0;
    auto uc_offset = wc_size;

    uc_resource.mapping_base += wc_size;

    auto* bar0 = static_cast<uint8_t*>(mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));

    if (bar0 == MAP_FAILED) {
        throw std::system_error(errno, std::generic_category(), "Failed to map BAR0");
    }

    void* wc = mmap(bar0 + wc_offset, wc_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, wc_resource.mapping_base);
    void* uc = mmap(bar0 + uc_offset, uc_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, uc_resource.mapping_base);

    if (uc == MAP_FAILED || wc == MAP_FAILED) {
        throw std::system_error(errno, std::generic_category(), "Failed to map BAR0");
    }

    return bar0;
}

namespace noc
{
    class NocAccess;
} // namespace noc

class Device
{
    int fd;
    uint8_t *bar0;
    uint8_t *bar2;
    noc::NocAccess *noc;
public:
    Device(const std::string& chardev_path)
        : fd(open(chardev_path.c_str(), O_RDWR | O_CLOEXEC))
        , bar0(map_bar0(fd, 1 << 29))
        , bar2(map_bar2(fd, 1 << 20))
    {
    }

    void map_for_dma(void* buffer, size_t size, uint64_t noc_addr)
    {
        tenstorrent_pin_pages pin{};
        pin.in.output_size_bytes = sizeof(pin.out);
        pin.in.virtual_address = reinterpret_cast<uint64_t>(buffer);
        pin.in.size = size;

        if (ioctl(fd, TENSTORRENT_IOCTL_PIN_PAGES, &pin) != 0) {
            throw std::system_error(errno, std::generic_category(), "Failed to pin pages");
        }

        uint64_t iova = pin.out.physical_address;
        // TODO: configure iATU, ugh.

        configure_iatu(0, (size - 1), noc_addr, iova);
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

private:
    void bar0_write32(uint64_t offset, uint32_t value)
    {
        *reinterpret_cast<volatile uint32_t*>(bar0 + offset) = value;
    }

    void bar2_write32(uint64_t offset, uint32_t value)
    {
        *reinterpret_cast<volatile uint32_t*>(bar2 + offset) = value;
    }

    void configure_iatu(uint32_t region, uint32_t limit, uint64_t chip_address, uint64_t bus_address)
    {
#if GEEZ_PUT_THIS_IN_THE_DRIVER_ALREADY
        auto write_iatu_reg = [&](uint32_t addr, uint32_t val) {
            bar2_write32(0x1200 + addr, val);
        };

        //uint64_t regs = 0x300000 + (0x200 * region);
        uint64_t regs = (0x200 * region);

        uint32_t ctrl1 = 0x00000000;
        uint32_t crtl2 = limit == 0 ? 0 : 0x88280000;
        uint32_t lower_base = chip_address & 0xFFFFFFFF;
        uint32_t upper_base = (chip_address >> 32) & 0xFFFFFFFF;
        limit = limit == 0 ? 0x0 : limit - 1;
        uint32_t lower_target = bus_address & 0xFFFFFFFF;
        uint32_t upper_target = (bus_address >> 32) & 0xFFFFFFFF;

        write_iatu_reg(regs + 0x00, ctrl1);
        write_iatu_reg(regs + 0x04, crtl2);
        write_iatu_reg(regs + 0x08, lower_base);
        write_iatu_reg(regs + 0x0C, upper_base);
        write_iatu_reg(regs + 0x10, limit);
        write_iatu_reg(regs + 0x14, lower_target);
        write_iatu_reg(regs + 0x18, upper_target);
#endif
    }

};

class DmaBuffer
{
    Device& device;
    size_t size;
    void* buffer;
    uint64_t iova;

public:
    DmaBuffer(Device& device, size_t size, uint64_t iova)
        : device(device)
        , size(size)
        , buffer(std::aligned_alloc(0x1000, size))
        , iova(0)
    {
        if (!buffer) {
            throw std::bad_alloc();
        }

        try {
            device.map_for_dma(buffer, size, iova);
        } catch (...) {
            std::free(buffer);
            throw;
        }
    }

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

    // This is horrible, but I need to know the DMA address of the window.
    // Since the id is just the TLB index (an undocumented detail), I can infer
    // the DMA address from the index + knowledge of the HW.
    int get_id() const { return tlb_id; }

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

    virtual void read_block(uint64_t offset, void* data, size_t size, CacheMode mode = CacheMode::UC)
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

    int get_fd() const { return fd; }

    ~NocAccess() noexcept
    {
        close(fd);
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

        uint64_t iova = pin.out.physical_address;
        return iova;
    }

    void unmap_for_dma(void *buffer)
    {
        tenstorrent_unpin_pages unpin{};
        unpin.in.virtual_address = reinterpret_cast<uintptr_t>(buffer);

        if (ioctl(fd, TENSTORRENT_IOCTL_UNPIN_PAGES, &unpin) != 0) {
            throw std::system_error(errno, std::generic_category(), "Failed to unpin pages");
        }
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

    std::unique_ptr<TlbWindow> map_tlb_1M(uint32_t x, uint32_t y, uint64_t address, int noc = 0)
    {
        static constexpr uint64_t WINDOW_SIZE = 1 << 20;
        static constexpr uint64_t WINDOW_MASK = WINDOW_SIZE - 1;

        tenstorrent_noc_tlb_config config{
            .addr = address & ~WINDOW_MASK,
            .x_end = x,
            .y_end = y,
            .noc = (uint8_t)noc,
        };

        auto handle = std::make_unique<TlbHandle>(fd, WINDOW_SIZE, config);
        auto offset = address & WINDOW_MASK;
        return offset ?
            std::make_unique<OffsetTlbWindow>(std::move(handle), offset) :
            std::make_unique<TlbWindow>(std::move(handle));
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

    std::unique_ptr<TlbWindow> map_tlb_16M(uint32_t x, uint32_t y, uint64_t address, int noc = 0)
    {
        static constexpr uint64_t WINDOW_SIZE = 1 << 24;
        static constexpr uint64_t WINDOW_MASK = WINDOW_SIZE - 1;

        tenstorrent_noc_tlb_config config{
            .addr = address & ~WINDOW_MASK,
            .x_end = x,
            .y_end = y,
            .noc = (uint8_t)noc,
        };

        auto handle = std::make_unique<TlbHandle>(fd, WINDOW_SIZE, config);
        auto offset = address & WINDOW_MASK;
        return offset ?
            std::make_unique<OffsetTlbWindow>(std::move(handle), offset) :
            std::make_unique<TlbWindow>(std::move(handle));
    }
};

} // namespace noc


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
                printf("Node ID mismatch at (%d, %d): expected (%d, %d), got (%d, %d)\n",
                       x, y, x, y, node_id_x, node_id_y);
                throw std::runtime_error("Something is screwed up");
            }
        }
    }
}

inline void wormhole_sanity_test()
{
    noc::NocAccess noc("/dev/tenstorrent/0");
    {
        constexpr uint32_t ARC_X = 0;
        constexpr uint32_t ARC_Y = 10;
        constexpr uint64_t ARC_NOC_NODE_ID = 0xFFFB2002CULL;

        auto node_id = noc.read_register(ARC_X, ARC_Y, ARC_NOC_NODE_ID);
        auto x = (node_id >> 0x0) & 0x3f;
        auto y = (node_id >> 0x6) & 0x3f;
        if (x != ARC_X || y != ARC_Y)
            throw std::runtime_error("Something is screwed up with the chip");
        else
            printf("ARC node_id: %08x\n", node_id);
    }

    {
        constexpr uint32_t DDR_X = 0;
        constexpr uint32_t DDR_Y = 11;
        constexpr uint64_t DDR_NOC_NODE_ID = 0x10009002CULL;

        auto node_id = noc.read_register(DDR_X, DDR_Y, DDR_NOC_NODE_ID);
        auto x = (node_id >> 0x0) & 0x3f;
        auto y = (node_id >> 0x6) & 0x3f;
        if (x != DDR_X || y != DDR_Y)
            throw std::runtime_error("Something is screwed up with the chip");
        else
            printf("DDR node_id: %08x\n", node_id);
    }
}

} // namespace detail
