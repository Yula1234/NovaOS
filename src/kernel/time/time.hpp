#pragma once

#include <stdint.h>

namespace kernel::time
{
	void init(uint32_t pit_frequency_hz) noexcept;

	uint64_t ticks() noexcept;
	uint32_t frequency_hz() noexcept;
	uint64_t ms_since_boot() noexcept;
}
