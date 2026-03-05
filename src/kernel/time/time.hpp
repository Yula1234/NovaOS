#pragma once

#include <stdint.h>

namespace kernel::time
{
	/* Initializes time sources. Prefers HPET+calibrated LAPIC timer, falls back to PIT. */
	void init(uint32_t tick_hz) noexcept;

	namespace monotonic
	{
		/* True if we can provide a monotonic nanosecond clock (TSC calibrated or HPET). */
		bool available() noexcept;
		uint64_t now_ns() noexcept;
		uint64_t now_ms() noexcept;
	}

	namespace tick
	{
		uint64_t count() noexcept;
		uint32_t hz() noexcept;
		uint64_t ms_since_init() noexcept;
	}

	void sleep_until_ns(uint64_t deadline_ns) noexcept;
	void sleep_ns(uint64_t duration_ns) noexcept;
	void sleep_until_ms(uint64_t deadline_ms) noexcept;
	void sleep_ms(uint64_t duration_ms) noexcept;

	uint64_t ticks() noexcept;
	uint32_t frequency_hz() noexcept;
	uint64_t ms_since_boot() noexcept;
	uint64_t ms_since_time_init() noexcept;
}
