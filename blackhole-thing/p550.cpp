
#include "noc.hpp"

static constexpr uint32_t WH_PCIE_X = 0;
static constexpr uint32_t WH_PCIE_Y = 3;

// This is here to make sure we don't waste any time if the chip is screwed up.
static void wormhole_sanity_test(noc::NocAccess& access)
{
    {
        constexpr uint32_t ARC_X = 0;
        constexpr uint32_t ARC_Y = 10;
        constexpr uint64_t ARC_NOC_NODE_ID = 0xFFFB2002CULL;

        auto node_id = access.read_register(ARC_X, ARC_Y, ARC_NOC_NODE_ID);
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

        auto node_id = access.read_register(DDR_X, DDR_Y, DDR_NOC_NODE_ID);
        auto x = (node_id >> 0x0) & 0x3f;
        auto y = (node_id >> 0x6) & 0x3f;
        if (x != DDR_X || y != DDR_Y)
            throw std::runtime_error("Something is screwed up with the chip");
        else
            printf("DDR node_id: %08x\n", node_id);
    }
}

static inline uint32_t random32()
{
    static uint32_t seed = 0x12345678;
    seed = seed * 1664525 + 1013904223;
    return seed;
}

static void wormhole_touch_dram(noc::NocAccess& access)
{
    constexpr uint32_t DDR_X = 0;
    constexpr uint32_t DDR_Y = 11;

    size_t n = 0x1000;

    std::vector<uint32_t> values;
    for (size_t i = 0; i < n; i += 4) {
        auto value = random32();
        access.write_register(DDR_X, DDR_Y, i, value);
        values.push_back(value);
    }

    for (size_t i = 0; i < n; i += 4) {
        auto value_1 = access.read_register(DDR_X, DDR_Y, i);
        auto value_2 = values[i / 4];
        if (value_1 != value_2) {
            throw std::runtime_error("DRAM test failed");
        }
    }

    printf("Passed DRAM test\n");
}


static void p550(int argc, char** argv)
{
    noc::NocAccess noc("/dev/tenstorrent/0");
    wormhole_sanity_test(noc);
    wormhole_touch_dram(noc);

    pci::Device device("/dev/tenstorrent/0");
    pci::DmaBuffer buffer(device, 0x100000, 0x0);

    uint32_t* data = reinterpret_cast<uint32_t*>(buffer.data());
    size_t n = buffer.length() / sizeof(uint32_t);

    for (size_t i = 0; i < n; i++) {
        data[i] = random32();
    }

    for (size_t i = 0; i < n; i++) {
        uint64_t wormhole = 0x8'0000'0000ULL;
        uint32_t value_1 = noc.read_register(WH_PCIE_X, WH_PCIE_Y, wormhole + i);
        uint32_t value_2 = data[i];
        if (value_1 != value_2) {
            printf("Mismatch at %08zx: %08x != %08x\n", i, value_1, value_2);
            break;
        }
    }

}

int main(int argc, char** argv) {

    try {
        p550(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        std::cerr << "\nYou should reset the chip." << std::endl;
        return 1;
    }

    return 0;
}
