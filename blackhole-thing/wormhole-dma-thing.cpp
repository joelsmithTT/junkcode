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
    void* buffer;
    void* completion_flag;

public:
    WormholeDMA(int fd, uint8_t* bar0, uint32_t buf_size = 1 << 20)
        : fd(fd)
        , buffer_size(buf_size)
        , bar0(bar0)
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
};

int main(int argc, char** argv)
{
    int fd = open("/dev/tenstorrent/0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "Failed to open device");
    }
    uint8_t* bar0 = pci::map_bar0(fd, 1 << 29);
    WormholeDMA dma(fd, bar0);

    noc::NocAccess noc("/dev/tenstorrent/0");
    std::vector<uint32_t> data(0x1000);
    std::iota(data.begin(), data.end(), 0);
    noc.write_block(9, 5, 0x0, data.data(), data.size() * sizeof(uint32_t));

    std::vector<uint32_t> result(data.size());
    // noc.read_block(9, 5, 0x0, result.data(), result.size() * sizeof(uint32_t));


    uint32_t src = 0x0;
    auto tlb = noc.map_tlb_1M(9, 5, 0x0);
    int id = tlb->handle_ref().get_id();
    if (id == 0) {
        dma.read_chunk(src, result.data(), result.size() * sizeof(uint32_t));
    }

    for (size_t i = 0; i < data.size(); ++i) {
        if (data[i] != result[i]) {
            std::printf("Mismatch at index %zu: expected %u, got %u\n", i, data[i], result[i]);
            return 1;
        }
    }

    return 0;
}
