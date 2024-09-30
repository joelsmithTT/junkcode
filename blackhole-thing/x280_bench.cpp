#include "blackhole_pcie.hpp"
#include "l2cpu_core.hpp"

#include "fmt/core.h"

using namespace tt;
static constexpr size_t L2CPU_X = 8;
static constexpr size_t L2CPU_Y = 3;

enum TileKind { DRAM, L2_CPU, TENSIX, ETHERNET, PCIE };
struct Tile
{
    const TileKind kind;
    const uint32_t x;
    const uint32_t y;
    const size_t size;

    Tile(TileKind kind, uint32_t x, uint32_t y, size_t size)
        : kind(kind)
        , x(x)
        , y(y)
        , size(size)
    {
    }
};

// Gonna run some benchmarks on the X280 to see how fast it can access its own
// DRAM, but also other places on the NOC.
//
// This code configures X280 -> NOC address mappings (128 GiB windows in X280's
// address space).
int main(int argc, char** argv)
{
    // Instantiate a Blackhole
    BlackholePciDevice device("/dev/tenstorrent/0");
    L2CPU x280(device, L2CPU_X, L2CPU_Y);

    // Some tiles to use
    std::vector<Tile> tiles = {
        Tile{DRAM, 0, 0, (4ULL * 1024 * 1024 * 1024)}, // 4 GiB
        Tile{DRAM, 0, 5, (4ULL * 1024 * 1024 * 1024)}, // 4 GiB
        Tile{DRAM, 9, 5, (4ULL * 1024 * 1024 * 1024)}, // 4 GiB
        Tile{TENSIX, 2, 9, (1ULL * 1024 * 1024)}, // 1 MiB
    };

    // Map TLB windows 0..n for each tile.
    uint64_t addr = 0x0;
    for (size_t i = 0; i < tiles.size(); ++i) {
        const auto& tile = tiles[i];

        size_t tlb_index = i;
        uint64_t addr = 0x0;
        uint32_t x = tile.x;
        uint32_t y = tile.y;
        device.map_tlb_2M_UC(x, y, addr)->write32(0x0, 0xBEEF0000 | i);
        
        uint64_t x280_addr = x280.configure_noc_tlb_128G(tlb_index, x, y, addr);
        fmt::print("Tile {} at {}, {} has address {:#x}\n", i, x, y, x280_addr);

        auto test1 = device.map_tlb_2M_UC(L2CPU_X, L2CPU_Y, x280_addr)->read32(0x0);
        auto test2 = device.map_tlb_2M_UC(x, y, addr)->read32(0x0);

        fmt::print("Test1: {:#x} Test2: {:#x}\n", test1, test2);
    }


    return 0;
}