#pragma once

#include <stdint.h>

namespace kernel::arch::x86_64::tlb
{
	void init() noexcept;
	void on_nmi() noexcept;
	void on_ipi() noexcept;

	constexpr uint8_t shootdown_vector = 0xF1;

	void shootdown_page(uint64_t target_cr3_phys, uint64_t virt) noexcept;
}
