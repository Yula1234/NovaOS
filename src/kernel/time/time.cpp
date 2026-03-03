#include "kernel/time/time.hpp"

#include "kernel/acpi/acpi.hpp"
#include "kernel/arch/x86_64/apic/timer.hpp"
#include "kernel/arch/x86_64/cpu.hpp"
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

	namespace monotonic
	{
		bool available() noexcept
		{
			return kernel::time::hpet::available();
		}

		uint64_t now_ns() noexcept
		{
			return kernel::time::hpet::available() ? kernel::time::hpet::ns_since_boot() : 0;
		}

		uint64_t now_ms() noexcept
		{
			return now_ns() / 1000000ull;
		}
	}

	namespace tick
	{
		uint64_t count() noexcept
		{
			return kernel::time::ticks();
		}

		uint32_t hz() noexcept
		{
			return kernel::time::frequency_hz();
		}

		uint64_t ms_since_init() noexcept
		{
			return kernel::time::ms_since_time_init();
		}
	}

	void sleep_until_ns(uint64_t deadline_ns) noexcept
	{
		if (!kernel::time::monotonic::available())
		{
			return;
		}

		for (;;)
		{
			const uint64_t now = kernel::time::monotonic::now_ns();
			if (static_cast<int64_t>(now - deadline_ns) >= 0)
			{
				return;
			}

			if (kernel::arch::x86_64::interrupts_enabled())
			{
				asm volatile("hlt");
			}
			else
			{
				asm volatile("pause");
			}
		}
	}

	void sleep_ns(uint64_t duration_ns) noexcept
	{
		if (!kernel::time::monotonic::available() || duration_ns == 0)
		{
			return;
		}

		const uint64_t start = kernel::time::monotonic::now_ns();
		sleep_until_ns(start + duration_ns);
	}

	void sleep_until_ms(uint64_t deadline_ms) noexcept
	{
		sleep_until_ns(deadline_ms * 1000000ull);
	}

	void sleep_ms(uint64_t duration_ms) noexcept
	{
		sleep_ns(duration_ms * 1000000ull);
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
