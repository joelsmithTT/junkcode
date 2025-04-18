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
#include <thread>
#include <vector>
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
            *reinterpret_cast<volatile uint32_t*>(bar2 + offset) = value;
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
        write_dma_reg(DMA_WRITE_DOORBELL_OFF, 0);

        Timestamp ts;
        for (;;) {
            if (ts.seconds() > 1) {
                throw std::runtime_error("Timeout");
            }
            if (*(volatile uint32_t*)completion_flag == 0xbee7) {
                break;
            }
        }
        memcpy(dst, buffer, size);
    }
};

void dbi_thing(uint8_t* bar0, uint8_t* bar2)
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

void csm_lookie(volatile uint8_t* bar0)
{
    uint64_t csm = 0x1FE80000;
    uint64_t dump = 0x20000;
    uint64_t size = 88064;
    for (size_t i = 0; i < size; i += 4) {
        uint32_t value = *(volatile uint32_t*)(bar0 + csm + dump + i);
        printf("0x%08lx: 0x%08x\n", csm + dump + i, value);
    }
    std::exit(0);
}

void dump_csm(volatile uint8_t* bar0, uint64_t start, size_t size)
{
    for (size_t i = 0; i < size; i += 4) {
        uint32_t value = *(volatile uint32_t*)(bar0 + start + i);
        printf("0x%08lx: 0x%08x\n", start + i, value);
    }
}

void csm_find_largest_zero_region(volatile uint8_t* bar0)
{
    uint64_t csm = 0x1FE80000;
    uint64_t dump = 0x20000;
    uint64_t size = 88064;

    size_t max_zero_start = 0;
    size_t max_zero_length = 0;
    size_t current_zero_start = 0;
    size_t current_zero_length = 0;
    bool in_zero_region = false;

    for (size_t i = 0; i < size; i += 4) {
        uint32_t value = *(volatile uint32_t*)(bar0 + csm + dump + i);

        if (value == 0) {
            if (!in_zero_region) {
                // Start of a new zero region
                in_zero_region = true;
                current_zero_start = i;
                current_zero_length = 4;
            } else {
                // Continuing zero region
                current_zero_length += 4;
            }
        } else {
            if (in_zero_region) {
                // End of a zero region
                in_zero_region = false;
                if (current_zero_length > max_zero_length) {
                    max_zero_length = current_zero_length;
                    max_zero_start = current_zero_start;
                }
            }
        }
    }

    // Check if we ended in a zero region
    if (in_zero_region && current_zero_length > max_zero_length) {
        max_zero_length = current_zero_length;
        max_zero_start = current_zero_start;
    }

    // Convert to offset from base
    uint64_t start_offset = csm + dump + max_zero_start;

    printf("Largest contiguous region of zeros:\n");
    printf("Start index: 0x%08lx (offset from base: 0x%08lx)\n", max_zero_start, start_offset);
    printf("Length: %lu bytes\n", max_zero_length);
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

void dma_test2()
{
    const size_t max_concurrent_dmas = 2;

    std::vector<noc_endpoint_t> endpoints {
        { 5, 6, 0x4000'0000 },
        { 0, 0, 0x0000'0000, 0},
        { 5, 9, 0x4000'0000 },
        { 9, 4, 0x0000'0000, 1 },
        { 5, 4, 0x0000'0000 },
        { 0, 7, 0x4000'0000 },
        { 5, 6, 0x0000'0000 },
        { 5, 1, 0x0000'0000 },
        { 5, 4, 0x4000'0000 },
        { 5, 9, 0x0000'0000 },
        { 0, 0, 0x4000'0000 },
        { 5, 1, 0x4000'0000 },
    };

    noc::NocAccess noc("/dev/tenstorrent/0");
    std::vector<std::unique_ptr<noc::TlbWindow>> windows;
    std::vector<uint8_t*> buffers;
    std::vector<uint64_t> iovas;
    size_t buffer_size = 1 << 24; // 16 MiB

    for (const auto& endpoint : endpoints) {
        auto window = noc.map_tlb_16M(endpoint.x, endpoint.y, endpoint.addr, endpoint.noc);
        windows.push_back(std::move(window));

        void* buffer = std::aligned_alloc(0x1000, buffer_size);
        if (!buffer) {
             perror("Failed to allocate aligned memory");
             exit(EXIT_FAILURE);
        }

        uint64_t iova = noc.map_for_dma(buffer, buffer_size);
        buffers.push_back(static_cast<uint8_t*>(buffer));
        iovas.push_back(iova);
    }

    std::vector<std::thread> active_threads;
    active_threads.reserve(max_concurrent_dmas);
    std::mutex stats_mutex;
    double total_megabytes = 0.0;

    Timestamp ts;

    for (size_t j = 0; j < 1024; ++j) {
        int i = j % endpoints.size();
        if (active_threads.size() >= max_concurrent_dmas) {
            active_threads[0].join();
            active_threads.erase(active_threads.begin());
        }

        uint32_t tlb_id = windows[i]->handle_ref().get_id();
        uint64_t current_iova = iovas[i];

        auto dma_task = [
            i,
            &noc,
            tlb_id,
            current_iova,
            buffer_size,
            &stats_mutex,
            &total_megabytes
        ]() {
            tenstorrent_dma dma{
                .in = {
                    .tlb_id = tlb_id,
                    .flags = TENSTORRENT_DMA_H2D,
                    .offset = 0,
                    .iova = current_iova,
                    .size = buffer_size
                }
            };

            int fd = noc.get_fd();
            int ret = ioctl(fd, TENSTORRENT_IOCTL_DMA, &dma);

            if (ret != 0) {
                fprintf(stderr, "DMA request failed for TLB ID %u: ret=%d, errno=%d\n", tlb_id, ret, errno);
            } else {
                std::lock_guard<std::mutex> lock(stats_mutex);
                total_megabytes += static_cast<double>(buffer_size) / (1024. * 1024.);
            }
        };

        active_threads.emplace_back(dma_task);
    }

    for (auto& t : active_threads) {
        t.join();
    }

    double elapsed_ns = ts.nanoseconds();
    double elapsed_s = elapsed_ns / 1e9;
    double megabytes_per_second = (elapsed_s > 0) ? (total_megabytes / elapsed_s) : 0.0;

    printf("Total DMA took %.3f ms (%.2f MB/s)\n", elapsed_ns / 1e6, megabytes_per_second);
}

void dma_test3()
{
    const size_t max_concurrent_dmas = 8;

    std::vector<noc_endpoint_t> endpoints {
        { 9, 5, 0x0 },
        { 9, 7, 0x0 },
        { 9, 4, 0x0 },
        { 9, 8, 0x0 },
        { 9, 8, 0x0 },
        { 9, 9, 0x0 },
        { 9, 2, 0x0 },
        // { 0, 5, 0x0, 1 },
        // { 0, 6, 0x0, 1 },
        // { 0, 4, 0x0, 1 },
        // { 0, 7, 0x0, 1 },
        // { 0, 3, 0x0, 1 },
        // { 0, 8, 0x0, 1 },
        { 1, 5, 0x0 },
        { 1, 7, 0x0 },
        { 1, 4, 0x0 },
        { 1, 8, 0x0 },
        { 1, 8, 0x0 },
        { 1, 9, 0x0 },
        { 1, 2, 0x0 },
        { 8, 5, 0x0 },
        { 8, 7, 0x0 },
        { 8, 4, 0x0 },
        { 8, 8, 0x0 },
        { 8, 8, 0x0 },
        { 8, 9, 0x0 },
        { 8, 2, 0x0 },
        { 2, 5, 0x0 },
        { 2, 7, 0x0 },
        { 2, 4, 0x0 },
        { 2, 8, 0x0 },
        { 2, 8, 0x0 },
        { 2, 9, 0x0 },
        { 2, 2, 0x0 },
        { 7, 5, 0x0 },
        { 7, 7, 0x0 },
        { 7, 4, 0x0 },
        { 7, 8, 0x0 },
        { 7, 8, 0x0 },
        { 7, 9, 0x0 },
        { 7, 2, 0x0 },
        { 3, 5, 0x0 },
        { 3, 7, 0x0 },
        { 3, 4, 0x0 },
        { 3, 8, 0x0 },
        { 3, 8, 0x0 },
        { 3, 9, 0x0 },
        { 3, 2, 0x0 },
        { 6, 5, 0x0 },
        { 6, 7, 0x0 },
        { 6, 4, 0x0 },
        { 6, 8, 0x0 },
        { 6, 8, 0x0 },
        { 6, 9, 0x0 },
        { 6, 2, 0x0 },
        { 4, 5, 0x0 },
        { 4, 7, 0x0 },
        { 4, 4, 0x0 },
        { 4, 8, 0x0 },
        { 4, 8, 0x0 },
        { 4, 9, 0x0 },
        { 4, 2, 0x0 },
    };

    std::random_shuffle(endpoints.begin(), endpoints.end());

    noc::NocAccess noc("/dev/tenstorrent/0");
    std::vector<std::unique_ptr<noc::TlbWindow>> windows;
    std::vector<uint8_t*> buffers;
    std::vector<uint64_t> iovas;
    size_t buffer_size = 1 << 20; // 1 MiB

    for (const auto& endpoint : endpoints) {
        auto window = noc.map_tlb_1M(endpoint.x, endpoint.y, endpoint.addr);
        windows.push_back(std::move(window));

        void* buffer = std::aligned_alloc(0x1000, buffer_size);
        if (!buffer) {
             perror("Failed to allocate aligned memory");
             exit(EXIT_FAILURE);
        }

        uint64_t iova = noc.map_for_dma(buffer, buffer_size);
        buffers.push_back(static_cast<uint8_t*>(buffer));
        iovas.push_back(iova);
    }

    std::vector<std::thread> active_threads;
    active_threads.reserve(max_concurrent_dmas);
    std::mutex stats_mutex;
    double total_megabytes = 0.0;

    Timestamp ts;

    for (size_t j = 0; j < 1024; ++j) {
        int i = j % endpoints.size();
        if (active_threads.size() >= max_concurrent_dmas) {
            active_threads[0].join();
            active_threads.erase(active_threads.begin());
        }

        uint32_t tlb_id = windows[i]->handle_ref().get_id();
        uint64_t current_iova = iovas[i];

        auto dma_task = [
            i,
            &noc,
            tlb_id,
            current_iova,
            buffer_size,
            &stats_mutex,
            &total_megabytes
        ]() {
            tenstorrent_dma dma{
                .in = {
                    .tlb_id = tlb_id,
                    .flags = TENSTORRENT_DMA_H2D,
                    .offset = 0,
                    .iova = current_iova,
                    .size = buffer_size
                }
            };

            int fd = noc.get_fd();
            int ret = ioctl(fd, TENSTORRENT_IOCTL_DMA, &dma);

            if (ret != 0) {
                fprintf(stderr, "DMA request failed for TLB ID %u: ret=%d, errno=%d\n", tlb_id, ret, errno);
            } else {
                std::lock_guard<std::mutex> lock(stats_mutex);
                total_megabytes += static_cast<double>(buffer_size) / (1024. * 1024.);
            }
        };

        active_threads.emplace_back(dma_task);
    }

    for (auto& t : active_threads) {
        t.join();
    }

    double elapsed_ns = ts.nanoseconds();
    double elapsed_s = elapsed_ns / 1e9;
    double megabytes_per_second = (elapsed_s > 0) ? (total_megabytes / elapsed_s) : 0.0;

    printf("Total DMA took %.3f ms (%.2f MB/s)\n", elapsed_ns / 1e6, megabytes_per_second);
}

void dma_test()
{
    std::vector<noc_endpoint_t> endpoints {
        { 0, 0, 0x0000'0000 },
        { 0, 0, 0x4000'0000 },
        { 0, 7, 0x0000'0000 },
        { 0, 7, 0x4000'0000 },
        { 5, 6, 0x0000'0000 },
        { 5, 6, 0x4000'0000 },
        { 5, 4, 0x0000'0000 },
        { 5, 4, 0x4000'0000 },
        { 5, 9, 0x0000'0000 },
        { 5, 9, 0x4000'0000 },
        { 5, 1, 0x0000'0000 },
        { 5, 1, 0x4000'0000 },
    };

    noc::NocAccess noc("/dev/tenstorrent/0");
    std::vector<std::unique_ptr<noc::TlbWindow>> windows;
    std::vector<std::vector<uint8_t>> patterns;
    std::vector<uint8_t*> buffers;
    std::vector<uint64_t> iovas;
    size_t buffer_size = 1 << 24;

    for (auto endpoint : endpoints) {
        auto window = noc.map_tlb_16M(endpoint.x, endpoint.y, endpoint.addr);
        windows.push_back(std::move(window));
        printf("window id: %d\n", windows.back()->handle_ref().get_id());

        auto pattern = random_buffer(buffer_size);
        patterns.push_back(std::move(pattern));

        void* buffer = std::aligned_alloc(0x1000, buffer_size);
        uint64_t iova = noc.map_for_dma(buffer, buffer_size);
        buffers.push_back(static_cast<uint8_t*>(buffer));
        iovas.push_back(iova);
    }

    // DMA to each endpoint
    Timestamp ts;
    double megabytes = 0;
    for (size_t i = 0; i < endpoints.size(); ++i) {
        auto& endpoint = endpoints[i];
        auto& window = windows[i];
        auto& buffer = patterns[i];
        auto iova = iovas[i];

        tenstorrent_dma dma{
            .in = {
                .tlb_id = (uint32_t)window->handle_ref().get_id(),
                .flags = TENSTORRENT_DMA_H2D,
                .offset = 0,
                .iova = iova,
                .size = buffer_size
            }
        };

        int fd = noc.get_fd();
        int ret = ioctl(fd, TENSTORRENT_IOCTL_DMA, &dma);
        if (ret != 0) {
            printf("DMA request failed: %d\n", ret);
            break;
        } else {
            megabytes += buffer_size / (1024. * 1024.);
        }
    }
    float elapsed = ts.nanoseconds();
    float megabytes_per_second = (megabytes * 1e9) / elapsed;
    printf("DMA took %f ns (%f MB/s)\n", elapsed, megabytes_per_second);
}

void kmd_dma_1(int x, int y, int addr)
{
    noc::NocAccess noc("/dev/tenstorrent/0");
    auto window = noc.map_tlb_16M(x, y, addr);
    printf("window id: %d\n", window->handle_ref().get_id());
    size_t buffer_size = 1 << 24;
    void* buffer = std::aligned_alloc(0x1000, buffer_size);
    uint64_t iova = noc.map_for_dma(buffer, buffer_size);
    int fd = noc.get_fd();

    auto pattern = random_buffer(buffer_size);

    window->write_block(0, &pattern[0], pattern.size());
    window->read32(0);
    window->read32(buffer_size - 4);

    printf("Mapped buffer to IOVA: 0x%lx\n", iova);
    tenstorrent_dma dma{
        .in = {
            .tlb_id = (uint32_t)window->handle_ref().get_id(),
            .flags = TENSTORRENT_DMA_D2H,
            .offset = 0,
            .iova = iova,
            .size = buffer_size
        }
    };
    Timestamp ts;
    int ret = ioctl(fd, TENSTORRENT_IOCTL_DMA, &dma);
    float elapsed = ts.nanoseconds();
    auto megabytes = buffer_size / (1024 * 1024);
    float megabytes_per_second = (megabytes * 1e9) / elapsed;
    printf("DMA took %f ns (%f MB/s)\n", elapsed, megabytes_per_second);
    printf("DMA request returned %d\n", ret);

    uint8_t* data = (uint8_t*)buffer;
    bool ok = true;
    for (size_t i = 0; i < buffer_size; ++i) {
        if (data[i] != pattern[i]) {
            printf("Mismatch at index %zu: expected %02x, got %02x\n", i, pattern[i], data[i]);
            ok = false;
            break;
        } else {
        }
    }
    if (ok) {
        printf("Data matches!\n");
    } else {
        printf("Data does not match!\n");
    }
}

void kmd_dma_2(int x, int y, int addr)
{
    noc::NocAccess noc("/dev/tenstorrent/0");
    auto window = noc.map_tlb_16M(x, y, addr);
    printf("window id: %d\n", window->handle_ref().get_id());
    size_t buffer_size = 1 << 24;
    void* buffer = std::aligned_alloc(0x1000, buffer_size);
    uint64_t iova = noc.map_for_dma(buffer, buffer_size);
    int fd = noc.get_fd();

    auto pattern = random_buffer(buffer_size);
    std::memcpy(buffer, &pattern[0], pattern.size());

    printf("Mapped buffer to IOVA: 0x%lx\n", iova);
    tenstorrent_dma dma{
        .in = {
            .tlb_id = (uint32_t)window->handle_ref().get_id(),
            .flags = TENSTORRENT_DMA_H2D,
            .offset = 0,
            .iova = iova,
            .size = buffer_size
        }
    };
    Timestamp ts;
    int ret = ioctl(fd, TENSTORRENT_IOCTL_DMA, &dma);
    float elapsed = ts.nanoseconds();
    auto megabytes = buffer_size / (1024 * 1024);
    float megabytes_per_second = (megabytes * 1e9) / elapsed;
    printf("DMA took %f ns (%f MB/s)\n", elapsed, megabytes_per_second);
    printf("DMA request returned %d\n", ret);
    return;
    // Read back the data
    window->read_block(0, buffer, buffer_size);

    uint8_t* data = (uint8_t*)buffer;
    bool ok = true;
    for (size_t i = 0; i < buffer_size; ++i) {
        if (data[i] != pattern[i]) {
            printf("Mismatch at index %zu: expected %02x, got %02x\n", i, pattern[i], data[i]);
            ok = false;
            break;
        } else {
        }
    }
    if (ok) {
        printf("Data matches!\n");
    } else {
        printf("Data does not match!\n");
    }
}

int main(int argc, char** argv)
{
    dma_test3();
    // kmd_dma_2(0, 0, 0);
    return 0;

    int fd = open("/dev/tenstorrent/0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "Failed to open device");
    }
    uint8_t* bar0 = pci::map_bar0(fd, 1 << 29);
    uint8_t* bar2 = pci::map_bar2(fd, 1 << 20);
    WormholeDMA dma(fd, bar0, bar2);

    noc::NocAccess noc("/dev/tenstorrent/0");
    std::vector<uint32_t> data((1 << 20) / sizeof(uint32_t));
    fill_with_random(data);

    noc.write_block(0, 0, 0x0, data.data(), data.size() * sizeof(uint32_t));

    std::vector<uint32_t> result(data.size());

    uint32_t src = 0x0;
    auto tlb = noc.map_tlb_1M(0, 0, 0x0);
    int id = tlb->handle_ref().get_id();
    if (id == 0) {
        // dma.read_chunk(src, result.data(), result.size() * sizeof(uint32_t));
        Timestamp ts;
        dma.do_dma(src, result.data(), result.size() * sizeof(uint32_t));
        float elapsed = ts.nanoseconds();
        float megabytes = result.size() / (1024. * 1024.);
        float megabytes_per_second = (megabytes * 1e9) / elapsed;
        printf("DMA took %f ns (%f MB/s)\n", elapsed, megabytes_per_second);
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
