#include "blackhole_pcie.hpp"
#include "pcie_core.hpp"
#include "utility.hpp"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>

using namespace tt;

// These numbers are for Blackhole PCIe core locations.
// yyz-syseng-06 and yyz-syseng-07 have host-connected PCIe at (2, 0).
// This may or may not be the case for you.
static constexpr size_t PCIE_RP_X = 11;
static constexpr size_t PCIE_RP_Y = 0;
static constexpr size_t PCIE_EP_X = 2;
static constexpr size_t PCIE_EP_Y = 0;

static bool is_iommu_enabled(const PciDeviceInfo& pci_info)
{
    std::ostringstream path_stream;
    path_stream << "/sys/bus/pci/devices/" << std::hex << std::setfill('0') << std::setw(4) << pci_info.pci_domain
                << ":" << std::setw(2) << pci_info.pci_bus << ":" << std::setw(2) << pci_info.pci_device << "."
                << pci_info.pci_function << "/iommu_group/type";

    std::string file_path = path_stream.str();
    std::ifstream file(file_path);

    if (!file.is_open()) {
        return false;
    }

    std::string content;
    std::getline(file, content);
    file.close();

    if (content.substr(0, 3) == "DMA") {
        return true;
    }

    return false;
}

int main(int argc, char** argv)
{
    // Instantiate a Blackhole
    BlackholePciDevice device("/dev/tenstorrent/0");

    // If IOMMU isn't enabled or if it is in passthrough mode, this won't work
    if (!is_iommu_enabled(device.get_info())) {
        std::cerr << "No IOMMU, quitting" << std::endl;
        return 1;
    } else {
        std::cout << "IOMMU is enabled" << std::endl;
    }

    // Make page-aligned buffer
    size_t size = 4 * 1024 * 1024;
    uint64_t* dma_buffer = reinterpret_cast<uint64_t*>(std::aligned_alloc(0x1000, size));
    if (!dma_buffer) {
        std::cerr << "Failed to allocate buffer" << std::endl;
        return 1;
    } else {
        std::cout << "Allocated buffer: 0x" << std::hex << size << " bytes" << std::endl;
    }

    // Fill the DMA buffer with a pattern
    std::iota(dma_buffer, dma_buffer + size / sizeof(uint64_t), 0);

    // Map the buffer for DMA, this will throw if it fails
    // iova can be used by the device to read/write the src_dma_buffer
    auto iova = device.map_for_dma(dma_buffer, size);
    std::cout << "IOVA is: 0x" << iova << std::endl;

    // Configure PCIe using NOC->PCIe TLB index 0 to disable hw addr translation
    PCIeCore pcie_noc_core(device, PCIE_EP_X, PCIE_EP_Y);
    auto pcie_addr = pcie_noc_core.configure_noc_tlb_data(0, NocTlbData{.atu_bypass = 1});
    std::cout << "Base of IOVA address space in PCIe core is 0x" << pcie_addr << std::endl;

    // The address needed by the NOC to access the buffer through the PCIe core
    auto noc_addr = pcie_addr + iova;
    std::cout << std::hex;
    std::cout << "Buffer is mapped to NOC" << "(x=" << PCIE_EP_X << ", y=" << PCIE_EP_Y << ", addr=0x" << std::hex
              << noc_addr << ")" << std::endl;

    // Map a 4 GiB inbound PCIe TLB window to the PCIe core itself, using the
    // NOC address that corresponds to the buffer
    auto window = device.map_tlb_4G(PCIE_EP_X, PCIE_EP_Y, noc_addr);

    // Read the pattern through the device; compare it
    std::cout << "Running test... wait a few seconds" << std::endl;
    Timer timer;
    for (size_t i = 0; i < size / sizeof(uint64_t); ++i) {
        auto expected = dma_buffer[i];
        auto actual = window->read64(i * sizeof(uint64_t));
        if (expected != actual) {
            std::cerr << "\nDMA readback test failed" << std::endl;
            std::cerr << "\t\tMismatch at index " << i << std::endl;
            std::cerr << std::hex;
            std::cerr << "\t\tExpected: 0x" << expected << "\n";
            std::cerr << "\t\tActual:   0x" << actual << std::endl;
            return 1;
        }
    }

    std::cout << "\nDMA readback 1 test passed in " << timer.elapsed_us() << " us" << std::endl;

    std::free(dma_buffer);
    return 0;
}

#if 0
// As above but with memcpy.  At least, read_block is just memcpy at the moment.
// This seems to work, but UMD has some hacks for using memcpy or memcpy-like
// functions with MMIO, at least for GS/WH.
void test2()
{
    // Do it again but with read_block
    std::vector<uint64_t> vec(size / sizeof(uint64_t));
    void* dst = vec.data();

    timer.reset();
    window->read_block(0, dst, size);

    // Compare the two buffers
    if (std::memcmp(src_dma_buffer, dst, size) != 0) {
        std::cerr << "\nDMA readback 2 test failed" << std::endl;
        return 1;
    }
    std::cout << "DMA readback 2 test passed in " << timer.elapsed_us() << " us" << std::endl;


    // Clean up after ourselves
    std::free(dst);
}
#endif