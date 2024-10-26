
#include <cstdint>

struct GS_TLB_1M_REG {
	union {
		struct {
			uint32_t low32;
			uint32_t high32;
		};
		struct {
			uint64_t local_offset: 12;
			uint64_t x_end: 6;
			uint64_t y_end: 6;
			uint64_t x_start: 6;
			uint64_t y_start: 6;
			uint64_t noc_sel : 1;
			uint64_t mcast: 1;
			uint64_t ordering: 2;
			uint64_t linked: 1;
		};
	};
};

struct WH_TLB_1M_REG {
	union {
		struct {
			uint32_t low32;
			uint32_t high32;
		};
		struct {
			uint64_t local_offset: 16;
			uint64_t x_end: 6;
			uint64_t y_end: 6;
			uint64_t x_start: 6;
			uint64_t y_start: 6;
			uint64_t noc_sel : 1;
			uint64_t mcast: 1;
			uint64_t ordering: 2;
			uint64_t linked: 1;
		};
	};
};

struct WH_TLB_16M_REG {
	union {
		struct {
			uint32_t low32;
			uint32_t high32;
		};
		struct {
			uint64_t local_offset: 12;
			uint64_t x_end: 6;
			uint64_t y_end: 6;
			uint64_t x_start: 6;
			uint64_t y_start: 6;
			uint64_t noc_sel : 1;
			uint64_t mcast: 1;
			uint64_t ordering: 2;
			uint64_t linked: 1;
		};
	};
};

struct GS_TLB_16M_REG {
	union {
		struct {
			uint32_t low32;
			uint32_t high32;
		};
		struct {
			uint64_t local_offset: 8;
			uint64_t x_end: 6;
			uint64_t y_end: 6;
			uint64_t x_start: 6;
			uint64_t y_start: 6;
			uint64_t noc_sel : 1;
			uint64_t mcast: 1;
			uint64_t ordering: 2;
			uint64_t linked: 1;
		};
	};
};

#if 0
uint64_t program_gs_kernel_tlb(TlbWindow &gs_bar0, GS_TLB_16M_REG &reg)
{
    // two uint32_t regs per TLB; 156 1M, 10 2M, and 20 16M (we'll use the last 16M TLB).
    uint64_t reg_offset = 0x1FC00000 + ((2 * (156 + 10 + 19)) * sizeof(uint32_t));

    mfence();
    gs_bar0.write32(reg_offset + 0, reg.low32);
    gs_bar0.write32(reg_offset + 4, reg.high32);
    mfence();

    // Offset from base of GS BAR0 to access this particular window.
    return (1 << 20) * 156 + (1 << 21) * 10 + (1 << 24) * 19;
}

uint64_t program_wh_kernel_tlb(TlbWindow &wh_bar0, WH_TLB_16M_REG &reg)
{
    // two uint32_t regs per TLB; 156 1M, 10 2M, and 20 16M (we'll use the last 16M TLB).
    uint64_t reg_offset = 0x1FC00000 + ((2 * (156 + 10 + 19)) * sizeof(uint32_t));

    mfence();
    wh_bar0.write32(reg_offset + 0, reg.low32);
    wh_bar0.write32(reg_offset + 4, reg.high32);
    mfence();

    // Offset from base of GS BAR0 to access this particular window.
    return (1 << 20) * 156 + (1 << 21) * 10 + (1 << 24) * 19;
}

#endif