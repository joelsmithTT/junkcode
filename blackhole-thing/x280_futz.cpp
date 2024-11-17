#include "blackhole_pcie.hpp"
#include "l2cpu_core.hpp"
#include "atomic.hpp"
#include "utility.hpp"
#include "fmt/core.h"
#include <chrono>
#include <string>
#include <iostream>

using namespace tt;
static constexpr size_t L2CPU_X = 8;
static constexpr size_t L2CPU_Y = 3;
static constexpr size_t ARC_X = 8;
static constexpr size_t ARC_Y = 0;

const std::vector<uint32_t> noc_x = { 0, 0, 0, 0 };
const std::vector<uint32_t> noc_y = { 0, 2, 9, 5 };

void slurp_the_arc(BlackholePciDevice& device)
{
    auto window = device.map_tlb_2M_UC(ARC_X, ARC_Y, 0x8003'0434);
    auto telemetry_struct_addr = window->read32(0);
    fmt::println("ARC: {:#08x}", telemetry_struct_addr);
    window = device.map_tlb_2M_UC(ARC_X, ARC_Y, telemetry_struct_addr);
    auto ver = window->read32(0);
    auto entry_count = window->read32(4);
    fmt::println("ARC: ver {} {}", ver, entry_count);

    std::vector<uint32_t> tags((entry_count + 1), 0);
    std::vector<uint32_t> data((entry_count + 1), 0);

    uint64_t telemetry_tags_addr = telemetry_struct_addr + 8;
    uint64_t telemetry_data_addr = telemetry_struct_addr + 8 + (entry_count * 4);

    for (size_t i = 0; i < entry_count + 1; ++i) {
        window = device.map_tlb_2M_UC(ARC_X, ARC_Y, telemetry_tags_addr + (i * 4));
        tags[i] = window->read32(0);
    }
    for (size_t i = 0; i < entry_count + 1; ++i) {
        window = device.map_tlb_2M_UC(ARC_X, ARC_Y, telemetry_data_addr + (i * 4));
        data[i] = window->read32(0);
    }

    auto asic_temp_tag = 11;
    auto fan_speed = 31;

    for (size_t i = 0; i < entry_count; ++i)
    {
        auto entry = tags[i];
        auto tag = entry & 0xFF;
        auto offset = (entry >> 16) & 0xFF;
        auto d = data[offset];
        fmt::println("Entry: {:#08x} tag: {:#02x} offset: {:#02x} data: {:#08x}", entry, tag, offset, d);

        if (tag == asic_temp_tag)
        {
            auto temp = (d >> 16) + (d & 0xFFFF) / (65536.0);
            fmt::println("ASIC Temp: {}", temp);
            fmt::println("Actual data: {}", d);
        }
    }
}

int main(int argc, char** argv)
{
    BlackholePciDevice device("/dev/tenstorrent/0");
    // slurp_the_arc(device);
    // return 0;
    L2CPU l2cpu(device, L2CPU_X, L2CPU_Y);

    uint64_t a = 0 * (1 << 21);
    uint64_t b = 1 * (1 << 21);
    uint64_t c = 2 * (1 << 21);
    uint64_t d = 3 * (1 << 21);

    for (size_t i = 0; i < 224; ++i)
        l2cpu.print_noc_tlb_2M(i);
    for (size_t i = 0; i < 32; ++i)
        l2cpu.print_noc_tlb_128G(i);
    return 0;

    // device.map_tlb_2M_UC(0, 0, 0)->write32(0, 0xbeefcafe);
    // device.map_tlb_2M_UC(0, 0, 1<<21)->write32(0, 0xaa55aa55);

    std::cout << std::hex;
    std::cout << device.map_tlb_2M_UC(0, 0, a)->read32(0) << std::endl;
    std::cout << device.map_tlb_2M_UC(0, 0, b)->read32(0) << std::endl;
    std::cout << device.map_tlb_2M_UC(0, 0, c)->read32(0) << std::endl;
    std::cout << device.map_tlb_2M_UC(0, 0, d)->read32(0) << std::endl;

    std::cout << device.map_tlb_2M_UC(8, 3, 0x2003000000 | a)->read32(0) << std::endl;
    std::cout << device.map_tlb_2M_UC(8, 3, 0x2003000000 | b)->read32(0) << std::endl;
    std::cout << device.map_tlb_2M_UC(8, 3, 0x2003000000 | c)->read32(0) << std::endl;
    std::cout << device.map_tlb_2M_UC(8, 3, 0x2003000000 | d)->read32(0) << std::endl;


    return 0;
}