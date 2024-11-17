#include "blackhole_pcie.hpp"
#include "atomic.hpp"
#include "utility.hpp"
#include "fmt/core.h"

using namespace tt;
static constexpr size_t DRAM_X = 9;
static constexpr size_t DRAM_Y = 6;

int main(int argc, char** argv)
{
    BlackholePciDevice device("/dev/tenstorrent/0");

    // 2 MiB of junk
    const auto above = random_vec<uint8_t>(2 * 1024 * 1024);
    const auto below = random_vec<uint8_t>(2 * 1024 * 1024);

    device.map_tlb_2M_UC(DRAM_X, DRAM_Y, 510 * 1024 * 1024)->write_block(0, &below[0], below.size());
    device.map_tlb_2M_UC(DRAM_X, DRAM_Y, 512 * 1024 * 1024)->write_block(0, &above[0], above.size());

    std::vector<uint8_t> buffer(2 * 1024 * 1024, 0x00);

    device.map_tlb_4G(DRAM_X, DRAM_Y, 0)->read_block(510 * 1024 * 1024, &buffer[0], buffer.size());
    if (buffer != below) {
        fmt::print("Below mismatch\n");
        return 1;
    }
    device.map_tlb_4G(DRAM_X, DRAM_Y, 512 * 1024 * 1024)->read_block(0, &buffer[0], buffer.size());
    if (buffer != above) {
        fmt::print("Above mismatch\n");
        return 1;
    }
    device.map_tlb_4G(DRAM_X, DRAM_Y, 0)->read_block(512 * 1024 * 1024, &buffer[0], buffer.size());
    if (buffer != above) {
        fmt::print("Above mismatch\n");
        return 1;
    }

    return 0;
}