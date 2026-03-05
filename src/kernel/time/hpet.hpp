#pragma once

#include <stdint.h>

namespace kernel::time::hpet
{
	/* HPET provides a monotonic hardware counter; used for calibration and timekeeping. */
	bool available() noexcept;

	/* Maps HPET registers, computes fs->ns conversion, and enables the main counter. */
	bool init(uint64_t hpet_phys) noexcept;

	uint64_t counter() noexcept;
	uint64_t ns_since_boot() noexcept;

	void busy_wait_ns(uint64_t ns) noexcept;
}
