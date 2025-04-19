#include "atomic.hpp"
#include "utility.hpp"
#include "fmt/core.h"
#include <chrono>
#include <string>
#include "ioctl.h"
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>
#include <span>
#include <thread>
#include "noc.hpp"

using namespace tt;

static constexpr uint32_t WH_PCIE_X = 0;
static constexpr uint32_t WH_PCIE_Y = 3;

void look_at_dbi_and_bar2(uint8_t *bar0, uint8_t *bar2)
{
    static constexpr uint64_t DBI_BASE = 0x300000 | 0x8'0000'0000ULL; // in PCIE
    static constexpr uint64_t BAR2_BASE = 0x1200; // in BAR2
    static constexpr uint64_t DBI = 0x1FF30078;

    auto bar0_write32 = [&](uint64_t offset, uint32_t value) {
        *reinterpret_cast<volatile uint32_t*>(bar0 + offset) = value;
    };
    auto bar0_read32 = [&](uint64_t offset) {
        return *reinterpret_cast<volatile uint32_t*>(bar0 + offset);
    };

    auto bar2_read32 = [&](uint64_t offset) {
        return *reinterpret_cast<volatile uint32_t*>(bar2 + offset);
    };

    auto enable_dbi = [&](bool enable) {
        uint32_t value = enable ? 0x200000 : 0x0;

        bar0_write32(DBI + 0, value);
        bar0_write32(DBI + 4, value);
    };

    auto gen_outbound_limit_address = [&](uint32_t region) {
        return 0x10 + (region * 0x200); // 0x10, 0x210, 0x410, ..., 0x1c10, 0x1e10
    };
    auto gen_inbound_limit_address = [&](uint32_t region) {
        return 0x110 + (region * 0x200); // 0x110, 0x310, 0x510, ..., 0x1d10, 0x1f10
    };
    auto gen_lower_target_address_inbound = [&](uint32_t region) {
        return 0x114 + (region * 0x200); // 0x114, 0x314, 0x514, ..., 0x1d14, 0x1f14
    };

    std::vector<uint32_t> limits_via_dbi;
    std::vector<uint32_t> limits_via_bar2;
    noc::NocAccess noc("/dev/tenstorrent/0");

    std::cout << "0x" << std::hex << bar0_read32(DBI + 0) << std::endl;
    std::cout << "0x" << std::hex << bar0_read32(DBI + 4) << std::endl;

#if 1
    enable_dbi(true);
    for (size_t i = 0; i < 16; ++i) {
        auto outbound_limit_address = gen_outbound_limit_address(i);
        auto inbound_limit_address = gen_inbound_limit_address(i);
        auto target_address = gen_lower_target_address_inbound(i);
        {
            auto outbound_limit_value = noc.map_tlb_2M(WH_PCIE_X, WH_PCIE_Y, DBI_BASE + outbound_limit_address)->read32(0);
            auto inbound_limit_value = noc.map_tlb_2M(WH_PCIE_X, WH_PCIE_Y, DBI_BASE + inbound_limit_address)->read32(0);
            std::cout << "region " << std::dec << i << ": ";
            std::cout << "outbound: 0x" << std::hex << outbound_limit_value << " inbound: 0x" << inbound_limit_value << std::endl;
        }
    }
    enable_dbi(false);
#endif
    for (size_t i = 0; i < 16; ++i) {
        auto outbound_limit_address = gen_outbound_limit_address(i);
        auto inbound_limit_address = gen_inbound_limit_address(i);
        {
            auto outbound_limit_value = bar2_read32(BAR2_BASE + outbound_limit_address);
            auto inbound_limit_value = bar2_read32(BAR2_BASE + inbound_limit_address);
            std::cout << "region " << std::dec << i << ": ";
            std::cout << "outbound: 0x" << std::hex << outbound_limit_value << " inbound: 0x" << inbound_limit_value << std::endl;
        }
    }
}

void dbi_thing(uint8_t* bar0, uint8_t* bar2)
{

    auto bar0_write32 = [&](uint64_t offset, uint32_t value) {
        *reinterpret_cast<volatile uint32_t*>(bar0 + offset) = value;
    };

    auto bar2_read32 = [&](uint64_t offset) {
        return *reinterpret_cast<volatile uint32_t*>(bar2 + offset);
    };

#if 1
    static constexpr uint64_t DBI_IATU_BASE = 0x300000;
    static constexpr uint64_t DBI_DMA_BASE = 0x380000;

    // uint64_t addr = 0;

    // noc::NocAccess noc("/dev/tenstorrent/0");
    // auto tlb = noc.map_tlb_2M(WH_PCIE_X, WH_PCIE_Y, DBI_IATU_BASE);
    // auto data = tlb->read32(addr);


    // data = tlb->read32(addr);
    // std::cout << "0x" << std::hex << data << std::endl;

    // bar0_write32(DBI + 0, 0);
    // bar0_write32(DBI + 4, 0);
    // std::exit(0);
#endif
}

std::vector<uint8_t> random_buffer(size_t num_bytes)
{
    std::vector<uint8_t> buffer(num_bytes);
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint8_t> dis(0, 255);
    for (size_t i = 0; i < num_bytes; ++i) {
        buffer[i] = dis(gen);
    }
    return buffer;
}

struct noc_endpoint_t {
    uint32_t x;
    uint32_t y;
    uint32_t addr;
    int noc;
};

class NocAccessibleMemory
{
    int fd;
    size_t size;
    void* buffer;
    uint64_t iova;

public:
    // base of 0x0 corresponds to 0x8_0000_0000 in PCIe (for WH)
    NocAccessibleMemory(int fd, size_t size, uint64_t base)
        : fd(fd), size(size)
    {
        buffer = std::aligned_alloc(0x1000, size);
        if (!buffer) {
            throw std::bad_alloc();
        }

        tenstorrent_pin_pages pin{};
        pin.in.output_size_bytes = sizeof(pin.out);
        pin.in.virtual_address = reinterpret_cast<uint64_t>(buffer);
        pin.in.size = size;

        if (ioctl(fd, TENSTORRENT_IOCTL_PIN_PAGES, &pin) != 0) {
            std::free(buffer);
            throw std::system_error(errno, std::generic_category(), "Failed to pin pages");
        }
        iova = pin.out.physical_address;

        std::cout << "IOVA: 0x" << std::hex << iova << std::endl;

        tenstorrent_configure_atu atu{};
        atu.in.base = base;
        atu.in.limit = atu.in.base + (size - 1);
        atu.in.target = iova;
        std::cout << "ATU: 0x" << std::hex << atu.in.base << "-" << "0x" << atu.in.limit << " -> 0x" << iova << std::endl;
        if (ioctl(fd, TENSTORRENT_IOCTL_CONFIGURE_ATU, &atu) != 0) {
            unpin();
            std::free(buffer);
            throw std::system_error(errno, std::generic_category(), "Failed to configure ATU");
        }

    }

    NocAccessibleMemory(const NocAccessibleMemory&) = delete;
    NocAccessibleMemory& operator=(const NocAccessibleMemory&) = delete;
    NocAccessibleMemory(NocAccessibleMemory&& other) noexcept
        : fd(other.fd), size(other.size), buffer(other.buffer)
    {
        other.buffer = nullptr;
    }

    template <typename T> T* as() noexcept { return reinterpret_cast<T*>(buffer); }
    size_t get_size() const noexcept { return size; }
    uint64_t get_iova() const noexcept { return iova; }

    template<typename T>
    std::span<T> as_span() noexcept
    {
        return std::span<T>(reinterpret_cast<T*>(buffer), size / sizeof(T));
    }

    ~NocAccessibleMemory() noexcept
    {
        unpin();
        if (buffer) {
            std::free(buffer);
        }
    }

private:
    void unpin() noexcept
    {
        tenstorrent_unpin_pages unpin{};
        unpin.in.virtual_address = reinterpret_cast<uint64_t>(buffer);
        unpin.in.size = size;
        ioctl(fd, TENSTORRENT_IOCTL_UNPIN_PAGES, &unpin);
    }
};

template <typename T>
void fill_with_random_data(std::span<T>& buffer)
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<T> dis(0, std::numeric_limits<T>::max());
    for (auto& elem : buffer) {
        elem = dis(gen);
    }
}

void test_all_sixteen_iatus()
{
    int fd = open("/dev/tenstorrent/0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "Failed to open device");
    }

    const size_t NUM_IATUS = 16;
    const uint64_t noc_base = 0x8'0000'0000ULL;

    std::vector<NocAccessibleMemory> buffers;
    uint64_t offset = 0;
    size_t size = 0x2000;
    for (size_t i = 0; i < NUM_IATUS; ++i) {
        NocAccessibleMemory mem(fd, size, offset);
        buffers.push_back(std::move(mem));
        offset += size;
        size *= 2;
    }

    std::vector<uint8_t> random(1024ULL * 1024 * 1024);
    std::span<uint32_t> span = std::span<uint32_t>(reinterpret_cast<uint32_t*>(random.data()), random.size() / sizeof(uint32_t));
    fill_with_random_data(span);

    uint8_t* src = random.data();

    noc::NocAccess noc("/dev/tenstorrent/0");
    for (size_t i = 0; i < 64; ++i) {
        size_t window_size = 1 << 24;   // 16M
        uint64_t address = noc_base + (i * window_size);
        auto window = noc.map_tlb_16M(WH_PCIE_X, WH_PCIE_Y, address);
        uint32_t* data = (uint32_t*)window->handle_ref().get_base_uc();
        std::memcpy(data, src, window_size);
        src += window_size;
    }

    src = random.data();
    size = 0x1000;
    for (size_t i = 0; i < NUM_IATUS; ++i) {
        NocAccessibleMemory& mem = buffers[i];

        int r = std::memcmp(mem.as_span<uint8_t>().data(), src, mem.get_size());
        if (r != 0) {
            std::cout << "Mismatch at iatu " << i << ": " << r << std::endl;
            std::terminate();
        }
        std::cout << "IATU " << i << " verified successfully." << std::endl;


        src += mem.get_size();
    }
}

void test_eight_iatus(size_t offset)
{
    const uint64_t noc_base = 0x8'0000'0000ULL;
    int fd = open("/dev/tenstorrent/0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "Failed to open device");
    }
    size_t original_offset = offset;

    const size_t NUM_IATUS = 8;

    std::vector<NocAccessibleMemory> buffers;
    size_t size = 0x1000;
    for (size_t i = 0; i < NUM_IATUS; ++i) {
        NocAccessibleMemory mem(fd, size, offset);
        buffers.push_back(std::move(mem));
        offset += size;
        size *= 2;
    }

    std::vector<uint8_t> random(512ULL * 1024 * 1024);
    std::span<uint32_t> span = std::span<uint32_t>(reinterpret_cast<uint32_t*>(random.data()), random.size() / sizeof(uint32_t));
    fill_with_random_data(span);

    uint8_t* src = random.data();

    noc::NocAccess noc("/dev/tenstorrent/0");
    for (size_t i = 0; i < 32; ++i) {
        size_t window_size = 1 << 24;   // 16M
        uint64_t address = noc_base + (i * window_size);
        auto window = noc.map_tlb_16M(WH_PCIE_X, WH_PCIE_Y, original_offset + address);
        uint32_t* data = (uint32_t*)window->handle_ref().get_base_uc();
        std::memcpy(data, src, window_size);
        src += window_size;
    }

    src = random.data();
    size = 0x1000;
    for (size_t i = 0; i < NUM_IATUS; ++i) {
        NocAccessibleMemory& mem = buffers[i];

        int r = std::memcmp(mem.as_span<uint8_t>().data(), src, mem.get_size());
        if (r != 0) {
            std::cout << "Mismatch at iatu " << i << ": " << r << std::endl;
            std::terminate();
        }
        std::cout << "IATU " << i << " verified successfully." << std::endl;


        src += mem.get_size();
    }
}

void double_eight()
{
    std::thread t1(test_eight_iatus, 0);
    std::thread t2(test_eight_iatus, 1073741824);

    t1.join();
    t2.join();
    int x;
    std::cin >> x;
}


int main(int argc, char** argv)
{
    int fd = open("/dev/tenstorrent/0", O_RDWR | O_CLOEXEC);
    if (fd < 0)
        throw std::system_error(errno, std::generic_category(), "Failed to open device");
    uint8_t* bar0 = pci::map_bar0(fd, 1 << 29);
    uint8_t* bar2 = pci::map_bar2(fd, 1 << 20);
    look_at_dbi_and_bar2(bar0, bar2);
    test_all_sixteen_iatus();
    look_at_dbi_and_bar2(bar0, bar2);
    // double_eight();
    std::exit(0);

    // test_all_sixteen_iatus();
    double_eight();
    // test_eight_iatus(std::stoi(argv[1]));
    // std::cin >> argc;

    return 0;
    static const uint64_t noc_base = 0x8'0000'0000ULL;

    noc::NocAccess noc("/dev/tenstorrent/0");

    std::vector<NocAccessibleMemory> buffers;
    std::vector<std::vector<uint32_t>> blobs;
    std::vector<std::vector<uint32_t>> top_blobs;

    size_t size = 1 << 30;
    uint32_t blob_size = 1 << 24;
    uint64_t offset = 0;
    for (size_t i = 0; i < 4; ++i) {
        std::vector<uint32_t> blob(blob_size / sizeof(uint32_t));
        std::span<uint32_t> blob_span = std::span<uint32_t>(blob);
        fill_with_random_data(blob_span);

        NocAccessibleMemory mem(fd, size, offset);

        auto window = noc.map_tlb_16M(WH_PCIE_X, WH_PCIE_Y, noc_base + offset);
        window->write_block(0, blob.data(), blob_size);

        if (i != 3) {
            window = noc.map_tlb_16M(WH_PCIE_X, WH_PCIE_Y, noc_base + offset + (size - blob_size));
            window->write_block(0, blob.data(), blob_size);
        }

        blobs.push_back(std::move(blob));
        buffers.push_back(std::move(mem));
        offset += size;
    }

    for (size_t i = 0; i < blobs.size(); ++i) {
        auto& blob = blobs[i];
        auto& mem = buffers[i];

        auto s1 = std::span<uint32_t>(blob);
        auto s2 = mem.as_span<uint32_t>();
        for (size_t j = 0; j < blob.size() / sizeof(uint32_t); ++j) {
            size_t k = j + ((size - blob_size) / sizeof(uint32_t));
            if (s1[j] != s2[j]) {
                std::cout << i << " " << "Mismatch at index " << j << ": " << s1[j] << " != " << s2[j] << std::endl;
                std::terminate();
            }
            if (i != 3 && s1[j] != s2[k]) {
                std::terminate();
            }
            if (j % 0x1000 == 0) {
                std::cout << "Buffer " << i << " value: " << s1[j] << " -> " << k << std::endl;
            }

        }

        std::cout << "Buffer " << i << " verified successfully." << std::endl;
    }



#if 0
    size_t size = 0x1000;
    void *buffer = std::aligned_alloc(0x1000, size);

    tenstorrent_pin_pages pin{};
    pin.in.output_size_bytes = sizeof(pin.out);
    pin.in.virtual_address = reinterpret_cast<uint64_t>(buffer);
    pin.in.size = size;

    if (ioctl(fd, TENSTORRENT_IOCTL_PIN_PAGES, &pin) != 0) {
        throw std::system_error(errno, std::generic_category(), "Failed to pin pages");
    }
    uint64_t iova = pin.out.physical_address;
    std::cout << "IOVA: 0x" << std::hex << iova << std::endl;

    tenstorrent_configure_atu atu{};
    atu.in.base = 0;
    atu.in.limit = atu.in.base + (size - 1);
    atu.in.target = iova;

    if (ioctl(fd, TENSTORRENT_IOCTL_CONFIGURE_ATU, &atu) != 0) {
        throw std::system_error(errno, std::generic_category(), "Failed to configure ATU");
    }
    std::cout << "ATU: 0x" << std::hex << atu.in.base << " -> 0x" << atu.in.target << std::endl;
#endif


    return 0;
}
