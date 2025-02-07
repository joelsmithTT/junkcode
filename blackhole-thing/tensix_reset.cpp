#include "blackhole_pcie.hpp"

#include <iostream>
#include <array>
#include "fmt/format.h"

using namespace tt;

uint32_t TRISC_SOFT_RESETS = (1 << 12) | (1 << 13) | (1 << 14);
uint32_t NCRISC_SOFT_RESET = 1 << 18;
uint32_t BRISC_SOFT_RESET = 1 << 11;
uint32_t STAGGERED_START_ENABLE = (1 << 31);


struct xy_t {
    uint32_t x, y;
    constexpr xy_t(uint32_t x_, uint32_t y_) : x(x_), y(y_) {}
};

template<std::size_t... Is>
constexpr auto make_tensix_locations(std::index_sequence<Is...>) {
    return std::array<xy_t, sizeof...(Is)>{
        xy_t{
            ((Is % 14) < 7) ? (1 + (Is % 7)) : (10 + (Is % 7)), // X
            2 + (Is / 14)                                       // Y
        }...
    };
}

auto tensix_locations = make_tensix_locations(std::make_index_sequence<140>{});

class Tensix
{
    BlackholePciDevice& device;
    uint32_t x, y;
public:
    Tensix(BlackholePciDevice& device, uint32_t x, uint32_t y)
        : device(device)
        , x(x)
        , y(y)
    {
    }

    void assert_soft_reset()
    {
        auto window = device.map_tlb_2M_UC(x, y, 0xFFB121B0);
        uint32_t reset_value = BRISC_SOFT_RESET | TRISC_SOFT_RESETS | NCRISC_SOFT_RESET;

        fmt::println("Asserting reset for tensix ({}, {}): {:#x}", x, y, reset_value);

        window->write32(0, reset_value);
    }

    void deassert_reset()
    {
        auto window = device.map_tlb_2M_UC(x, y, 0xFFB121B0);
        uint32_t reset_value = NCRISC_SOFT_RESET | STAGGERED_START_ENABLE;
        fmt::println("Deasserting reset for tensix ({}, {}): {:#x}", x, y, reset_value);
        window->write32(0, reset_value);
    }
};

int main(int argc, char** argv)
{
    BlackholePciDevice device("/dev/tenstorrent/0");

    for (const auto& loc : tensix_locations) {
        std::cout << "Trying location (" << loc.x << ", " << loc.y << ")" << std::endl;
        Tensix tensix(device, loc.x, loc.y);
        tensix.assert_soft_reset();
        tensix.deassert_reset();
        tensix.assert_soft_reset();
    }


    return 0;
}
