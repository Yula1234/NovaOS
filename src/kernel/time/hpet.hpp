#pragma once

#include <stdint.h>

namespace kernel::time::hpet
{
	bool available() noexcept;

	bool init(uint64_t hpet_phys) noexcept;

	uint64_t counter() noexcept;
	uint64_t ns_since_boot() noexcept;

	void busy_wait_ns(uint64_t ns) noexcept;
}
