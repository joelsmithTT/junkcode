#pragma once

#include "atomic.hpp"
#include "blackhole_pcie.hpp"
#include "tlb_window.hpp"

namespace tt {

class BlackholePciDevice;

// L2CPU has TLB windows for NOC access in two flavors: 2 MiB and 128 GiB.
// The 128 GiB windows are weirdly broken when attempting to access PCIe core's
// address space corresponding to the MMIO (i.e. address space in which BARs are
// assigned) region of the device when the PCIe core is in in root port mode.
// The 2 MiB windows work fine.  One day I should figure out the story here.
namespace l2cpu {
struct Tlb2M
{
    union
    {
        struct __attribute__((packed))
        {
            uint64_t address : 43;
            uint64_t reserved0 : 21;
            uint64_t x_end : 6;
            uint64_t y_end : 6;
            uint64_t x_start : 6;
            uint64_t y_start : 6;
            uint64_t multicast_en : 1;
            uint64_t strict_order : 1;
            uint64_t posted : 1;
            uint64_t linked : 1;
            uint64_t static_en : 1;
            uint64_t stream_header : 1;
            uint64_t reserved1 : 1;
            uint64_t noc_selector : 1;
            uint64_t static_vc : 3;
            uint64_t strided : 8;
            uint64_t exclude_coord_x : 5;
            uint64_t exclude_coord_y : 4;
            uint64_t exclude_dir_x : 1;
            uint64_t exclude_dir_y : 1;
            uint64_t exclude_enable : 1;
            uint64_t exclude_routing_option : 1;
            uint64_t num_destinations : 8;
        };
        uint32_t data[4];
    };
};

struct Tlb128G
{
    union
    {
        struct __attribute__((packed))
        {
            uint64_t address : 27;
            uint64_t reserved0 : 5;
            uint64_t x_end : 6;
            uint64_t y_end : 6;
            uint64_t x_start : 6;
            uint64_t y_start : 6;
            uint64_t multicast_en : 1;
            uint64_t strict_order : 1;
            uint64_t posted : 1;
            uint64_t linked : 1;
            uint64_t static_en : 1;
            uint64_t stream_header : 1;
            uint64_t reserved1 : 1;
            uint64_t noc_selector : 1;
            uint64_t static_vc : 3;
            uint64_t strided : 8;
            uint64_t exclude_coord_x : 5;
            uint64_t exclude_coord_y : 4;
            uint64_t exclude_dir_x : 1;
            uint64_t exclude_dir_y : 1;
            uint64_t exclude_enable : 1;
            uint64_t exclude_routing_option : 1;
            uint64_t num_destinations : 8;
        };
        uint32_t data[3];
    };
};

} // namespace l2cpu

/**
 * @brief One L2CPU core in Blackhole.
 *
 * L2CPU cores are on the NOC.  There are four in Blackhole.  Each contains four
 * X280 cores from SiFive.  L2CPU refers to the X280s plus the surrounding
 * "uncore" logic.
 *
 * The only L2CPU I've bothered with is the one at NOC0 (x=8, y=3).
 *
 * System port and Memory port are the same with one key difference: NOC access
 * to the memory port is coherent with X280 cache.  System port is not.
 *
 * TODO: I've just been changing the access address calculation back and forth
 * in the NOC TLB configuration functions.  If this were real code...
 */
class L2CPU
{
    // clang-format off
    static constexpr uint64_t PERIPHERAL_PORT   = 0x0000'0000'2000'0000ULL; // 256 MiB
    static constexpr uint64_t L3_ZERO_START     = 0x0000'0000'0A00'0000ULL; //   2 MiB
    static constexpr uint64_t L3_ZERO_END       = 0x0000'0000'0A20'0000ULL; //   
    static constexpr uint64_t SYSTEM_PORT       = 0x0000'0000'3000'0000ULL; //  64 TiB
    static constexpr uint64_t MEMORY_PORT       = 0x0000'4000'3000'0000ULL; //  64 TiB
    static constexpr uint64_t L2CPU_REGISTERS   = 0xFFFF'F7FE'FFF0'0000ULL; // 512 KiB
    static constexpr uint64_t L2CPU_DMAC        = 0xFFFF'F7FE'FFF8'0000ULL;
    // clang-format on

    BlackholePciDevice& device;
    const uint32_t our_noc0_x;
    const uint32_t our_noc0_y;
    std::unique_ptr<TlbWindow> peripheral_port;

public:
    L2CPU(BlackholePciDevice& device, uint32_t noc0_x, uint32_t noc0_y)
        : device(device)
        , our_noc0_x(noc0_x)
        , our_noc0_y(noc0_y)
        , peripheral_port(device.map_tlb_4G(our_noc0_x, our_noc0_y, PERIPHERAL_PORT)) // Ugh
    {
    }

    // TODO: would be ideal to manage tlb_index internally instead of making the
    // caller have to pick one.
    uint64_t configure_noc_tlb_2M(size_t tlb_index, uint32_t noc_x, uint32_t noc_y, uint64_t address)
    {
        auto registers = device.map_tlb_2M_UC(our_noc0_x, our_noc0_y, L2CPU_REGISTERS);
        size_t tlb_config_offset = tlb_index * 0x10;

        const size_t tlb_size = 1ULL << 21; // 2 MiB
        const size_t tlb_mask = tlb_size - 1;
        const uint64_t local_offset = address & tlb_mask;
        const size_t apparent_size = tlb_size - local_offset;

        l2cpu::Tlb2M tlb{};
        tlb.address = address >> 21;
        tlb.x_end = noc_x;
        tlb.y_end = noc_y;
        tlb.strict_order = 1;

        mfence();
        registers->write32(tlb_config_offset + 0x0, tlb.data[0]);
        registers->write32(tlb_config_offset + 0x4, tlb.data[1]);
        registers->write32(tlb_config_offset + 0x8, tlb.data[2]);
        registers->write32(tlb_config_offset + 0xC, tlb.data[3]);
        mfence();

        // Where in X280 address space the window begins:
        // return 0x0000'0020'3000'0000ULL + (0x200000 * tlb_index);
        auto access_address = ((1ULL << 37) | (0x200000 * tlb_index) | SYSTEM_PORT) + local_offset;
        return access_address;
    }

    uint64_t configure_noc_tlb_128G(size_t tlb_index, uint32_t noc_x, uint32_t noc_y, uint64_t address)
    {
        auto registers = device.map_tlb_2M_UC(our_noc0_x, our_noc0_y, L2CPU_REGISTERS);
        size_t tlb_config_offset = 0xE00 + (tlb_index * 0xC);

        const size_t tlb_size = 1ULL << 37; // 128 GiB
        const size_t tlb_mask = tlb_size - 1;
        const uint64_t local_offset = address & tlb_mask;
        const size_t apparent_size = tlb_size - local_offset;

        l2cpu::Tlb128G tlb{};
        tlb.address = address >> 37;
        tlb.x_end = noc_x;
        tlb.y_end = noc_y;
        tlb.strict_order = 1;

        mfence();
        registers->write32(tlb_config_offset + 0x0, tlb.data[0]);
        registers->write32(tlb_config_offset + 0x4, tlb.data[1]);
        registers->write32(tlb_config_offset + 0x8, tlb.data[2]);
        mfence();

        // Where in X280 space does the window begin?  L2CPU Spec.docx gave me
        // numbers that did not work.
        //
        // Andrew says,
        //  RTL uses bit 43 to determine whether to use 2MB TLBs (bit 43=0) or 128GB TLBs (bit 43=1)
        //  the address is evaluated after passing ddr_noc_xbar which sends 0x2000000000+ to NOC
        //  So I think the first 128GB TLB is at system_port_address + noc_address + bit 43 = 0x82030000000
        //

        auto access_address = ((1ULL << 43) | ((1ULL << 37) * (1 + tlb_index)) | SYSTEM_PORT) + local_offset;
        return access_address;
        // return ((1ULL << 43) | ((1ULL << 37) * (1 + tlb_index)) | MEMORY_PORT) + local_offset;
    }
};

} // namespace tt