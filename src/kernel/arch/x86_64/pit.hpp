#pragma once

#include <stdint.h>

namespace kernel::arch::x86_64::pit
{
	void init(uint32_t frequency_hz) noexcept;
	uint64_t ticks() noexcept;
	uint32_t frequency_hz() noexcept;
}
