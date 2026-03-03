#include "kernel/time/time.hpp"

#include "kernel/acpi/acpi.hpp"
#include "kernel/arch/x86_64/apic/timer.hpp"
#include "kernel/arch/x86_64/pit.hpp"
#include "kernel/time/hpet.hpp"

namespace
{
	uint64_t time_init_base_ms = 0;
}

namespace kernel::time
{
	void init(uint32_t tick_hz) noexcept
	{
		time_init_base_ms = 0;

		if (const auto* h = kernel::acpi::hpet())
		{
			kernel::time::hpet::init(h->hpet_phys);
		}

		if (kernel::time::hpet::available())
		{
			if (kernel::arch::x86_64::apic::timer::init_calibrated(tick_hz))
			{
				time_init_base_ms = ms_since_boot();
				return;
			}
		}

		kernel::arch::x86_64::pit::init(tick_hz);
		time_init_base_ms = ms_since_boot();
	}

	uint64_t ticks() noexcept
	{
		const uint32_t hz = kernel::arch::x86_64::apic::timer::frequency_hz();
		if (hz != 0)
		{
			return kernel::arch::x86_64::apic::timer::ticks();
		}

		return kernel::arch::x86_64::pit::ticks();
	}

	uint32_t frequency_hz() noexcept
	{
		const uint32_t apic_hz = kernel::arch::x86_64::apic::timer::frequency_hz();
		if (apic_hz != 0)
		{
			return apic_hz;
		}

		return kernel::arch::x86_64::pit::frequency_hz();
	}

	uint64_t ms_since_boot() noexcept
	{
		if (kernel::time::hpet::available())
		{
			return kernel::time::hpet::ns_since_boot() / 1000000ull;
		}

		const uint32_t hz = frequency_hz();
		if (hz == 0)
		{
			return 0;
		}

		return (ticks() * 1000) / hz;
	}

	uint64_t ms_since_time_init() noexcept
	{
		const uint64_t now = ms_since_boot();
		return now >= time_init_base_ms ? (now - time_init_base_ms) : 0;
	}
}
