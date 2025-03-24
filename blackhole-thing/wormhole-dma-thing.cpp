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
#include "noc.hpp"

using namespace tt;
static constexpr size_t DRAM_X = 9;
static constexpr size_t DRAM_Y = 6;


class Timestamp {
    std::chrono::steady_clock::time_point start;

public:
    Timestamp() : start(std::chrono::steady_clock::now()) {}

    void reset() { start = std::chrono::steady_clock::now(); }

    uint64_t nanoseconds() const {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count();
    }

    uint64_t microseconds() const {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::microseconds>(now - start).count();
    }

    uint64_t milliseconds() const {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
    }

    uint64_t seconds() const {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
    }

    std::string to_string() const {
        auto ns = nanoseconds();
        if (ns < 1000) {
            return fmt::format("{} ns", ns);
        }
        auto us = microseconds();
        if (us < 1000) {
            return fmt::format("{} μs", us);
        }
        auto ms = milliseconds();
        if (ms < 1000) {
            return fmt::format("{} ms", ms);
        }
        return fmt::format("{} s", seconds());
    }
};


typedef struct {
    uint32_t  chip_addr;
    uint32_t  host_phys_addr;
    uint32_t  completion_flag_phys_addr;
    uint32_t  size_bytes                  : 28;
    uint32_t  write                       : 1;
    uint32_t  pcie_msi_on_done            : 1;
    uint32_t  pcie_write_on_done          : 1;
    uint32_t  trigger                     : 1;
    uint32_t  repeat;
} arc_pcie_ctrl_dma_request_t; // 5 * 4 = 20B

class WormholeDMA
{
    int fd;
    size_t buffer_size;
    uint32_t buffer_phys_addr;
    uint32_t completion_phys_addr;

    uint8_t* bar0;
    uint8_t* bar2;
    void* buffer;
    void* completion_flag;

public:
    WormholeDMA(int fd, uint8_t* bar0, uint8_t* bar2, uint32_t buf_size = 1 << 20)
        : fd(fd)
        , buffer_size(buf_size)
        , bar0(bar0)
        , bar2(bar2)
    {
        tenstorrent_allocate_dma_buf dma_buf{
            .in = {
                .requested_size = buf_size + 0x1000,
                .buf_index = 0
            }
        };

        if (ioctl(fd, TENSTORRENT_IOCTL_ALLOCATE_DMA_BUF, &dma_buf)) {
            throw std::system_error(errno, std::generic_category(), "Failed to allocate DMA buffer");
        }

        printf("Allocated DMA buffer: %p\n", (void*)dma_buf.out.physical_address);
        printf("Completion address:   %p\n", (void*)(dma_buf.out.physical_address + buf_size));

        buffer_phys_addr = dma_buf.out.physical_address;
        completion_phys_addr = dma_buf.out.physical_address + buf_size;

        buffer = mmap(nullptr, buf_size + 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, dma_buf.out.mapping_offset);
        if (buffer == MAP_FAILED) {
            throw std::system_error(errno, std::generic_category(), "Failed to map DMA buffer");
        }
        completion_flag = (uint8_t*)buffer + buf_size;
    }

    void write_reg(uint32_t addr, uint32_t value)
    {
        printf("Writing 0x%08x to 0x%08x\n", value, addr);
        *(volatile uint32_t*)(bar0 + addr) = value;
    }

    void read_chunk(uint32_t src, void* dst, uint32_t size)
    {
        if (size > buffer_size) {
            throw std::runtime_error("Requested size is larger than buffer size");
        }

        // This interface is totally cursed
        arc_pcie_ctrl_dma_request_t req = {
            .chip_addr           = src,
            .host_phys_addr      = buffer_phys_addr,
            .completion_flag_phys_addr = completion_phys_addr,
            .size_bytes          = size,
            .write               = 0,
            .pcie_msi_on_done    = 0,
            .pcie_write_on_done  = 1,
            .trigger             = 1,
            .repeat              = 1
        };

        uint64_t c_CSM_PCIE_CTRL_DMA_REQUEST_OFFSET = 0x1fef84c8;
        uint64_t c_ARC_MISC_CNTL_ADDRESS = 0x1ff30100;
        *reinterpret_cast<volatile uint32_t*>(completion_flag) = 0;

        for (size_t i = 0; i < sizeof(req) / sizeof(uint32_t); i++) {
            write_reg(c_CSM_PCIE_CTRL_DMA_REQUEST_OFFSET + i * 4, ((uint32_t*)&req)[i]);
        }
        write_reg(c_ARC_MISC_CNTL_ADDRESS, 1 << 16);

        for (;;) {
            if (*(volatile uint32_t*)completion_flag == 0xfaca) {
                break;
            }
        }

        memcpy(dst, buffer, size);
    }

    void do_dma(uint32_t chip_addr, void* dst, uint32_t size)
    {
        const uint64_t DMA_WRITE_ENGINE_EN_OFF = 0xc;
        const uint64_t DMA_WRITE_INT_MASK_OFF = 0x54;
        const uint64_t DMA_CH_CONTROL1_OFF_WRCH_0 = 0x200;
        const uint64_t DMA_WRITE_DONE_IMWR_LOW_OFF = 0x60;
        const uint64_t DMA_WRITE_CH01_IMWR_DATA_OFF = 0x70;
        const uint64_t DMA_WRITE_DONE_IMWR_HIGH_OFF = 0x64;
        const uint64_t DMA_WRITE_ABORT_IMWR_LOW_OFF = 0x68;
        const uint64_t DMA_WRITE_ABORT_IMWR_HIGH_OFF = 0x6c;
        const uint64_t DMA_TRANSFER_SIZE_OFF_WRCH_0 = 0x208;
        const uint64_t DMA_SAR_LOW_OFF_WRCH_0 = 0x20c;
        const uint64_t DMA_SAR_HIGH_OFF_WRCH_0 = 0x210;
        const uint64_t DMA_DAR_LOW_OFF_WRCH_0 = 0x214;
        const uint64_t DMA_DAR_HIGH_OFF_WRCH_0 = 0x218;
        const uint64_t DMA_WRITE_DOORBELL_OFF = 0x10;

        auto write_dma_reg = [&](uint32_t offset, uint32_t value) {
            printf("Writing 0x%08x to 0x%08x\n", value, offset);
            *reinterpret_cast<volatile uint32_t*>(bar2 + offset) = value;
            uint32_t readback = *reinterpret_cast<volatile uint32_t*>(bar2 + offset);
            if (readback != value) {
                printf("Readback mismatch: 0x%08x\n", readback);
                std::exit(1);
            }
        };
        auto read_dma_reg = [&](uint32_t offset) {
            return *reinterpret_cast<volatile uint32_t*>(bar2 + offset);
        };

        auto snoopy = [&]() {
            auto sar = read_dma_reg(DMA_SAR_LOW_OFF_WRCH_0);
            auto dar = read_dma_reg(DMA_DAR_LOW_OFF_WRCH_0);
            auto flag = read_dma_reg(DMA_WRITE_CH01_IMWR_DATA_OFF);
            auto completion = *reinterpret_cast<volatile uint32_t*>(completion_flag);
            printf("SAR: 0x%08x, DAR: 0x%08x Flag: 0x%08x Completion: 0x%08x\n", sar, dar, flag, completion);
        };

        *reinterpret_cast<volatile uint32_t*>(completion_flag) = 0xbeefcaec;


        write_dma_reg(DMA_WRITE_ENGINE_EN_OFF, 0x1);
        write_dma_reg(DMA_WRITE_INT_MASK_OFF, 0);
        write_dma_reg(DMA_CH_CONTROL1_OFF_WRCH_0, 0x04000010);
        write_dma_reg(DMA_WRITE_DONE_IMWR_LOW_OFF, completion_phys_addr);
        write_dma_reg(DMA_WRITE_CH01_IMWR_DATA_OFF, 0xbee7);
        write_dma_reg(DMA_WRITE_DONE_IMWR_HIGH_OFF, 0);
        write_dma_reg(DMA_WRITE_ABORT_IMWR_LOW_OFF, 0);
        write_dma_reg(DMA_WRITE_ABORT_IMWR_HIGH_OFF, 0);
        write_dma_reg(DMA_TRANSFER_SIZE_OFF_WRCH_0, size);
        write_dma_reg(DMA_SAR_LOW_OFF_WRCH_0, chip_addr);
        write_dma_reg(DMA_SAR_HIGH_OFF_WRCH_0, 0);
        write_dma_reg(DMA_DAR_LOW_OFF_WRCH_0, buffer_phys_addr);
        write_dma_reg(DMA_DAR_HIGH_OFF_WRCH_0, 0);
        snoopy();
        write_dma_reg(DMA_WRITE_DOORBELL_OFF, 0);
        snoopy();



        Timestamp ts;
        for (;;) {
            // snoopy();
            if (ts.seconds() > 1) {
                snoopy();
                throw std::runtime_error("Timeout");
            }
            if (*(volatile uint32_t*)completion_flag == 0xbee7) {
                break;
            }
        }
        snoopy();

        memcpy(dst, buffer, size);
    }
};

void fucking_dbi(uint8_t* bar0, uint8_t* bar2)
{
    static constexpr uint64_t ARC_RESET = 0x1FF30000;
    static constexpr uint64_t PCI_RESERVED = 0x0078;
    static constexpr uint64_t DBI = ARC_RESET | PCI_RESERVED;
    static constexpr uint32_t WH_PCIE_X = 0;
    static constexpr uint32_t WH_PCIE_Y = 3;

    auto bar0_write32 = [&](uint64_t offset, uint32_t value) {
        *reinterpret_cast<volatile uint32_t*>(bar0 + offset) = value;
    };

    auto bar2_read32 = [&](uint64_t offset) {
        return *reinterpret_cast<volatile uint32_t*>(bar2 + offset);
    };

    // auto iatu_1 = bar2_read32(0x1514);
    // std::cout << "0x" << std::hex << iatu_1 << std::endl;

#if 1
    static constexpr uint64_t DBI_IATU_BASE = 0x300000;
    static constexpr uint64_t DBI_DMA_BASE = 0x380000;

    // uint64_t addr = 0;

    // noc::NocAccess noc("/dev/tenstorrent/0");
    // auto tlb = noc.map_tlb_2M(WH_PCIE_X, WH_PCIE_Y, DBI_IATU_BASE);
    // auto data = tlb->read32(addr);

    bar0_write32(DBI + 0, 0x00200000);
    bar0_write32(DBI + 4, 0x00200000);

    // data = tlb->read32(addr);
    // std::cout << "0x" << std::hex << data << std::endl;

    // bar0_write32(DBI + 0, 0);
    // bar0_write32(DBI + 4, 0);
    // std::exit(0);
#endif
}

void fill_with_random(std::vector<uint32_t>& vec)
{
    std::random_device rd;
    std::mt19937 gen(rd());

    for (auto& element : vec) {
        element = gen();
    }
}

void kmd_dma()
{
    noc::NocAccess noc("/dev/tenstorrent/0");
    auto window = noc.map_tlb_1M(0, 0, 0x0);
    size_t buffer_size = 0x1000;
    void* buffer = std::aligned_alloc(0x1000, buffer_size);
    uint64_t iova = noc.map_for_dma(buffer, buffer_size);
    int fd = noc.get_fd();

    std::vector<uint8_t> pattern(buffer_size);
    std::iota(pattern.begin(), pattern.end(), 0);
    pattern[0] = 0x42;
    pattern[1] = 0x17;
    pattern[2] = 0x69;

    window->write_block(0, &pattern[0], pattern.size());


    printf("Mapped buffer to IOVA: 0x%lx\n", iova);
    tenstorrent_dma dma{
        .in = {
            .tlb_id = (uint32_t)window->handle_ref().get_id(),
            .offset = 0,
            .iova = iova,
            .size = buffer_size
        }
    };
    int ret = ioctl(fd, TENSTORRENT_IOCTL_DMA, &dma);

    printf("DMA request returned %d\n", ret);

}

int main(int argc, char** argv)
{
    kmd_dma();
    return 0;
    int fd = open("/dev/tenstorrent/0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "Failed to open device");
    }
    uint8_t* bar0 = pci::map_bar0(fd, 1 << 29);
    uint8_t* bar2 = pci::map_bar2(fd, 1 << 20);
    // fucking_dbi(bar0, bar2);
    WormholeDMA dma(fd, bar0, bar2);

    noc::NocAccess noc("/dev/tenstorrent/0");
    std::vector<uint32_t> data((1 << 19) / sizeof(uint32_t));
    fill_with_random(data);

    noc.write_block(0, 0, 0x0, data.data(), data.size() * sizeof(uint32_t));

    std::vector<uint32_t> result(data.size());

    uint32_t src = 0x0;
    auto tlb = noc.map_tlb_1M(0, 0, 0x0);
    int id = tlb->handle_ref().get_id();
    if (id == 0) {
        // dma.read_chunk(src, result.data(), result.size() * sizeof(uint32_t));
        dma.do_dma(src, result.data(), result.size() * sizeof(uint32_t));
    } else {
        std::cout << "OH NO" << std::endl;
        return 1;
    }

    for (size_t i = 0; i < data.size(); ++i) {
        if (data[i] != result[i]) {
            std::printf("Mismatch at index %zu: expected %u, got %u\n", i, data[i], result[i]);
            return 1;
        }
    }

    return 0;
}
