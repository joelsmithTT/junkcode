#include "uapi.hpp"
#include <iostream>

#include <array>
#include <utility>



#include <queue>
#include <memory>
#include <mutex>


int main()
{
    NOC noc;
    std::vector<ManagedNocWindow> windows;


    for (const auto& tensix : Blackhole::TENSIX_LOCATIONS) {
        uint32_t value = 0xbeef;
        noc.write(tensix.x, tensix.y, 0x1000, &value, sizeof(value));
    }

    for (const auto& tensix : Blackhole::TENSIX_LOCATIONS) {
        uint32_t value = 0;
        noc.read(tensix.x, tensix.y, 0x1000, &value, sizeof(value));
        if (value != 0xbeef) {
            std::cerr << "Failed to read back value from tensix at " << tensix.x << ", " << tensix.y << std::endl;
            return 1;
        }
    }

    return 0;
}
