#include "kernel/time/time.hpp"

#include "kernel/acpi/acpi.hpp"
#include "kernel/arch/x86_64/apic/timer.hpp"
#include "kernel/arch/x86_64/cpu.hpp"
#include "kernel/arch/x86_64/pit.hpp"
#include "kernel/time/hpet.hpp"

namespace
{
	uint64_t time_init_base_ms = 0;
	uint64_t tsc_hz = 0;
	uint64_t tsc_base = 0;
	uint64_t tsc_base_ns = 0;
	uint64_t tsc_ns_mul_q32 = 0;

	constexpr uint64_t ns_per_s = 1000000000ull;
	constexpr uint64_t q32_shift = 32;

	void mul_u64_u64_128(uint64_t a, uint64_t b, uint64_t& lo, uint64_t& hi) noexcept
	{
		asm volatile(
			"mulq %[b]"
			: "=a"(lo), "=d"(hi)
			: "a"(a), [b] "r"(b)
			: "cc"
		);
	}

	uint64_t mul_shift_right_32(uint64_t a, uint64_t b) noexcept
	{
		uint64_t lo = 0;
		uint64_t hi = 0;
		mul_u64_u64_128(a, b, lo, hi);
		return (hi << (64 - q32_shift)) | (lo >> q32_shift);
	}

	uint64_t u64_mul_div_u64(uint64_t value, uint64_t mul, uint64_t div) noexcept
	{
		if (div == 0)
		{
			return 0;
		}

		const uint64_t q = value / div;
		const uint64_t r = value % div;
		return q * mul + (r * mul) / div;
	}

	uint64_t tsc_to_ns(uint64_t tsc_delta) noexcept
	{
		if (tsc_hz == 0 || tsc_ns_mul_q32 == 0)
		{
			return 0;
		}

		return mul_shift_right_32(tsc_delta, tsc_ns_mul_q32);
	}

	bool calibrate_tsc_with_hpet() noexcept
	{
		if (!kernel::time::hpet::available())
		{
			return false;
		}

		constexpr uint64_t calib_ns = 200ull * 1000ull * 1000ull;

		const uint64_t start_ns = kernel::time::hpet::ns_since_boot();
		const uint64_t start_tsc = kernel::arch::x86_64::rdtsc();
		kernel::time::hpet::busy_wait_ns(calib_ns);
		const uint64_t end_tsc = kernel::arch::x86_64::rdtsc();
		const uint64_t end_ns = kernel::time::hpet::ns_since_boot();

		const uint64_t delta_tsc = end_tsc - start_tsc;
		const uint64_t delta_ns = end_ns - start_ns;
		if (delta_tsc == 0 || delta_ns == 0)
		{
			return false;
		}

		tsc_hz = u64_mul_div_u64(delta_tsc, ns_per_s, delta_ns);
		if (tsc_hz == 0)
		{
			return false;
		}

		const uint64_t numer_q32 = ns_per_s << q32_shift;
		tsc_ns_mul_q32 = (numer_q32 + (tsc_hz / 2)) / tsc_hz;
		if (tsc_ns_mul_q32 == 0)
		{
			return false;
		}

		tsc_base = kernel::arch::x86_64::rdtsc();
		tsc_base_ns = kernel::time::hpet::ns_since_boot();
		return true;
	}
}

namespace kernel::time
{
	void init(uint32_t tick_hz) noexcept
	{
		time_init_base_ms = 0;
		tsc_hz = 0;
		tsc_base = 0;
		tsc_base_ns = 0;
		tsc_ns_mul_q32 = 0;

		if (const auto* h = kernel::acpi::hpet())
		{
			kernel::time::hpet::init(h->hpet_phys);
		}

		if (kernel::time::hpet::available())
		{
			calibrate_tsc_with_hpet();

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
			return tsc_hz != 0 || kernel::time::hpet::available();
		}

		uint64_t now_ns() noexcept
		{
			if (tsc_hz != 0)
			{
				const uint64_t now_tsc = kernel::arch::x86_64::rdtsc();
				const uint64_t delta = now_tsc - tsc_base;
				return tsc_base_ns + tsc_to_ns(delta);
			}

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
		if (kernel::time::monotonic::available())
		{
			return kernel::time::monotonic::now_ns() / 1000000ull;
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
