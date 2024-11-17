#include "blackhole_pcie.hpp"
#include "atomic.hpp"
#include "utility.hpp"
#include "fmt/core.h"
#include <chrono>
#include <string>
#include <iostream>

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

int main(int argc, char** argv)
{
    BlackholePciDevice device("/dev/tenstorrent/0");
    const size_t one_gig = 1 << 30;
    std::vector<uint8_t> buffer(4 * one_gig);   // cover the full window
    auto tlb_window = device.map_tlb_4G(DRAM_X, DRAM_Y, 0);

    for (size_t size = 4; size <= 512 * 1024 * 1024; size *= 2) {
        Timestamp ts;
        tlb_window->read_block(0, buffer.data(), size);
        auto usec = ts.microseconds();

        double mib_per_sec = (size / (1024.0 * 1024.0)) / (usec / 1e6);
        fmt::print("Read {} bytes in {} us ({:.2f} MiB/s)\n", size, usec, mib_per_sec);
    }


    return 0;
}