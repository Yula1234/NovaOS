#pragma once

#include <stdint.h>

namespace kernel::arch::x86_64::apic::timer
{
	bool init_calibrated(uint32_t hz) noexcept;

	uint64_t ticks() noexcept;
	uint64_t ticks_cpu(uint32_t apic_id) noexcept;
	uint32_t frequency_hz() noexcept;
}
