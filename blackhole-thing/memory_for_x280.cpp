
#include <filesystem>
#include <iostream>
#include <fstream>
#include <unistd.h>
#include <system_error>

#include "blackhole_pcie.hpp"
#include "l2cpu_core.hpp"
#include "pcie_core.hpp"

#include "fmt/core.h"

#define SYSTEM_PORT 0x30000000ULL

#define WINDOW_128G_COUNT 32
#define WINDOW_128G_SHIFT 37
#define WINDOW_128G_SIZE (1UL << WINDOW_128G_SHIFT)
#define WINDOW_128G_BASE (((1UL << 43) | (1UL << 37) | SYSTEM_PORT))
#define WINDOW_128G_ADDR(n) (WINDOW_128G_BASE + (WINDOW_128G_SIZE * (n)))

using namespace tt;

class PageAlignedBuffer {
public:
    explicit PageAlignedBuffer(size_t size) {
        size_t page_size = 4096;
        size_ = (size + page_size - 1) & ~(page_size - 1);

        void* ptr = nullptr;
        if (posix_memalign(&ptr, page_size, size_) != 0) {
            throw std::runtime_error("Failed to allocate aligned buffer");
        }
        buffer_ = ptr;
    }

    ~PageAlignedBuffer() {
        free(buffer_);
    }

    void* data() { return buffer_; }
    const void* data() const { return buffer_; }
    size_t size() const { return size_; }

    // Prevent copying
    PageAlignedBuffer(const PageAlignedBuffer&) = delete;
    PageAlignedBuffer& operator=(const PageAlignedBuffer&) = delete;

private:
    void* buffer_;
    size_t size_;
};

static constexpr size_t L2CPU_X = 8;
static constexpr size_t L2CPU_Y = 3;
static constexpr size_t PCIE_X = 11;
static constexpr size_t PCIE_Y = 0;

struct xy {
    size_t x;
    size_t y;
};
std::vector<xy> drams {
    { 9, 6 },
    { 9, 8 },
    { 9, 3 },
};
std::vector<xy> l2cpus {
    // { 8, 6 },
    { 8, 7 },
    { 8, 5 },
    // { 8, 8 },
    // { 8, 4 },
    { 8, 9 },
    { 8, 3 },
    // { 8, 10 }
};

// This was annoying because touching the wrong thing kills the card!
// Also wasn't documented anywhere I could easily find.
// So here it is, for posterity:
//
// DRAM 9,6 -> L2CPUs at (8,7), (8,5)
// DRAM 9,8 -> L2CPU at (8,9)
// DRAM 9,3 -> L2CPU at (8,3)
// So I guess I'll use DRAM 9,8 and L2CPU 8,9 for my experiment...
void figure_x280_dram_relationship()
{
    BlackholePciDevice device("/dev/tenstorrent/0");
    size_t n = 0;
    for (auto dram : drams) {
        device.map_tlb_2M_UC(dram.x, dram.y, 0)->write32(0, 0xbeef0000 | n);
        n += 1;
    }
    for (auto dram : drams) {
        auto val = device.map_tlb_2M_UC(dram.x, dram.y, 0)->read32(0);
        fmt::print("DRAM {},{}: {:#x}\n", dram.x, dram.y, val);
    }
    for (auto l2cpu : l2cpus) {
        auto sp = device.map_tlb_2M_UC(l2cpu.x, l2cpu.y, 0x3000'0000)->read32(0);
        auto mp = device.map_tlb_2M_UC(l2cpu.x, l2cpu.y, 0x4000'3000'0000ULL)->read32(0);
        fmt::print("L2CPU {},{} SP: {:#x} MP: {:#x}\n", l2cpu.x, l2cpu.y, sp, mp);
    }
}

static constexpr size_t OTHER_L2CPU_X = 8;
static constexpr size_t OTHER_L2CPU_Y = 9;
static constexpr size_t OTHER_L2CPU_DRAM_X = 9;
static constexpr size_t OTHER_L2CPU_DRAM_Y = 8;

int wtf()
{
    BlackholePciDevice device("/dev/tenstorrent/0");
    L2CPU x280(device, L2CPU_X, L2CPU_Y);
    PCIeCore pcie_core(device, 11, 0);

    // Something fishy is going on with X280's NOC TLBs of the 128G variety.
    // BUt I am not sure if it is an X280 or PCIe issue.

    {
        NocTlbData dbi{ .dbi = 1 };
        auto address = pcie_core.configure_noc_tlb_data(0, dbi);
        address = x280.configure_noc_tlb_128G(0, PCIE_X, PCIE_Y, address);
        fmt::print("DBI in PCIe mapped to {:#x} in X280 address space\n", address);
        // That works (read from the base of the 128G window in X280 address space)
        int x;
        std::cin >> x;
    }

    {
        NocTlbData not_dbi{ .dbi = 0, .atu_bypass = 1 };
        auto address = pcie_core.configure_noc_tlb_data(1, not_dbi);
        address = x280.configure_noc_tlb_128G(1, PCIE_X, PCIE_Y, address);
        fmt::print("Not DBI in PCIe mapped to {:#x} in X280 address space\n", address);
        // That does not work (read from the base of the 128G window in X280 address space)
        // It generates a page fault at 0x1c00000000
        // Reading at base + 0x400000000 generates a page fault at 0x0
        int x;
        std::cin >> x;
    }

    // Put a pattern at the bottom of the DRAM
    uint32_t magic = 0x55a1'b7ef;
    device.map_tlb_2M_UC(OTHER_L2CPU_DRAM_X, OTHER_L2CPU_DRAM_Y, 0)->write32(0, magic);

    // map X280 TLBs to the other X280's DRAM
    uint64_t mem_port = 0x0000'3000'0000ULL;
    uint64_t address_128G = x280.configure_noc_tlb_128G(0, OTHER_L2CPU_X, OTHER_L2CPU_Y, mem_port);
    uint64_t address_2M = x280.configure_noc_tlb_2M(0, OTHER_L2CPU_X, OTHER_L2CPU_Y, mem_port);
    fmt::print("Other X280 DRAM mapped to (128G) {:#x} in X280 address space\n", address_128G);
    fmt::print("Other X280 DRAM mapped to (2M) {:#x} in X280 address space\n", address_2M);


    return 0;
}

#include <sys/mman.h>
#include <fcntl.h>


int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <filename>\n";
        return 1;
    }

    try {
        BlackholePciDevice device("/dev/tenstorrent/0");
        L2CPU x280(device, L2CPU_X, L2CPU_Y);
        std::filesystem::path filepath(argv[1]);
        auto file_size = std::filesystem::file_size(filepath);

        std::cout << "Allocating buffer of size " << file_size << std::endl;
        auto buffer = PageAlignedBuffer(file_size);
        std::cout << "... done" << std::endl;

        std::cout << "IOMMU mapping buffer" << std::endl;
        uint64_t iova = device.map_for_dma(buffer.data(), buffer.size());
        if (iova == 0) {
            throw std::runtime_error("Failed to map buffer for DMA");
        }
        std::cout << "... done" << std::endl;

        std::cout << "Reading file into buffer" << std::endl;
        std::ifstream file(filepath, std::ios::binary);
        if (!file) {
            throw std::system_error(errno, std::system_category(), "Failed to open file");
        }
        file.read(static_cast<char*>(buffer.data()), file_size);
        std::cout << "... done" << std::endl;

        std::cout << "iATU..." << std::endl;
        device.configure_iatu_region(0, 0, iova, buffer.size());
        std::cout << "... done" << std::endl;

        // 4th NOC->PCIe window does not bypass ATU.
        // We need ATU because the IOVA is not predictable.
        // But the address I have in my X280 device tree for pmem is fixed.
        uint64_t pcie_addr = (4ULL << 58);

        // TODO: magic zero
        std::cout << "X280/NOC TLB..." << std::endl;
        auto x280_addr = x280.configure_noc_tlb_128G(0, PCIE_X, PCIE_Y, pcie_addr);
        std::cout << "Buffer mapped at 0x" << std::hex << x280_addr << std::dec
                  << ", size " << buffer.size() << " in X280 address space\n";
        std::cout << "...done" << std::endl;

        // TODO: this program makes a kernel mess if I leave it running when I
        // kick the card off the PCIe bus so I can power cycle it.
        // Great news, that's fixed!

        std::cout << "OK, you can use it.\nIOVA: 0x" << std::hex << iova << std::endl;
        std::cout << "X280: 0x" << x280_addr << std::endl;

        pause();

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}