#include "uapi.hpp"
#include <iostream>

int main()
{
    Blackhole device;
    for (const auto& tensix : Blackhole::TENSIX_LOCATIONS) {
        if (tensix.x == 1) continue;
        std::cout << "Reserving window for tensix at " << tensix.x << ", " << tensix.y << std::endl;
        device.reserve_window(tensix.x, tensix.y);
    }

    for (const auto& tensix : Blackhole::TENSIX_LOCATIONS) {
        uint32_t value = 0xbeef;
        device.write(tensix.x, tensix.y, 0x1000, &value, sizeof(value));
    }

    for (const auto& tensix : Blackhole::TENSIX_LOCATIONS) {
        uint32_t value = 0;
        device.read(tensix.x, tensix.y, 0x1000, &value, sizeof(value));
        if (value != 0xbeef) {
            std::cerr << "Failed to read back value from tensix at " << tensix.x << ", " << tensix.y << std::endl;
            return 1;
        }
    }

    return 0;
}
