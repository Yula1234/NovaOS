#include "kernel/time/time.hpp"

#include "kernel/arch/x86_64/pit.hpp"

namespace kernel::time
{
	void init(uint32_t pit_frequency_hz) noexcept
	{
		kernel::arch::x86_64::pit::init(pit_frequency_hz);
	}

	uint64_t ticks() noexcept
	{
		return kernel::arch::x86_64::pit::ticks();
	}

	uint32_t frequency_hz() noexcept
	{
		return kernel::arch::x86_64::pit::frequency_hz();
	}

	uint64_t ms_since_boot() noexcept
	{
		const uint32_t hz = frequency_hz();
		if (hz == 0)
		{
			return 0;
		}

		return (ticks() * 1000) / hz;
	}
}
