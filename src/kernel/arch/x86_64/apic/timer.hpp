#pragma once

#include <stdint.h>

namespace kernel::arch::x86_64::apic::timer
{
	/* Calibrates LAPIC timer using HPET; must be called after HPET and LAPIC are up. */
	bool init_calibrated(uint32_t hz) noexcept;
	void init_cpu() noexcept;
	uint32_t init_count() noexcept;

	uint64_t ticks() noexcept;
	/* Per-CPU tick counter indexed by LAPIC ID (xAPIC ID). */
	uint64_t ticks_cpu(uint32_t apic_id) noexcept;
	uint32_t frequency_hz() noexcept;
}
