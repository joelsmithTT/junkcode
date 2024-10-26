#include "blackhole_pcie.hpp"
#include "l2cpu_core.hpp"

#include "fmt/core.h"

using namespace tt;
static constexpr size_t L2CPU_X = 8;
static constexpr size_t L2CPU_Y = 3;

int main(int argc, char** argv)
{
    // Instantiate a Blackhole
    BlackholePciDevice device("/dev/tenstorrent/0");
    L2CPU x280(device, L2CPU_X, L2CPU_Y);

    // x280.configure_prefetcher_default();
    x280.configure_prefetcher_recommended();

    return 0;
}