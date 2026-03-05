#pragma once

#include <stdint.h>

namespace kernel::arch::x86_64::pit
{
	/* Programs PIT channel 0 and installs an IRQ0 handler that increments a tick counter. */
	void init(uint32_t frequency_hz) noexcept;
	uint64_t ticks() noexcept;
	uint32_t frequency_hz() noexcept;
}
