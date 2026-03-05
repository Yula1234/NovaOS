#include "kernel/arch/x86_64/apic/timer.hpp"

#include <atomic>

#include "kernel/arch/x86_64/apic/lapic.hpp"
#include "kernel/log/log.hpp"
#include "kernel/time/hpet.hpp"

namespace
{
	constexpr uint32_t reg_lvt_timer = 0x320;
	constexpr uint32_t reg_timer_init = 0x380;
	constexpr uint32_t reg_timer_cur = 0x390;
	constexpr uint32_t reg_timer_div = 0x3E0;

	constexpr uint32_t lvt_periodic = 1u << 17;
	constexpr uint32_t lvt_masked = 1u << 16;

	constexpr uint8_t vector_timer = 0x30;

	/* We key counters by APIC ID; it fits in 8 bits in xAPIC mode. */
	std::atomic<uint64_t> tick_counter_per_cpu[256]{};
	uint8_t bsp_apic_id = 0;
	uint32_t active_hz = 0;
	uint32_t ticks_per_ms = 0;
	uint32_t calibrated_init_count = 0;
	uint32_t calibrated_div = 0;

	void on_tick() noexcept
	{
		const uint32_t apic_id = kernel::arch::x86_64::apic::lapic::id() & 0xFFu;
		tick_counter_per_cpu[apic_id].fetch_add(1, std::memory_order_relaxed);
		kernel::arch::x86_64::apic::lapic::eoi();
	}
}

namespace kernel::arch::x86_64::apic::timer
{
	uint64_t ticks() noexcept
	{
		return ticks_cpu(bsp_apic_id);
	}

	uint64_t ticks_cpu(uint32_t apic_id) noexcept
	{
		return tick_counter_per_cpu[apic_id & 0xFFu].load(std::memory_order_relaxed);
	}

	uint32_t frequency_hz() noexcept
	{
		return active_hz;
	}

	uint32_t init_count() noexcept
	{
		return calibrated_init_count;
	}

	void init_cpu() noexcept
	{
		if (active_hz == 0 || calibrated_init_count == 0)
		{
			return;
		}

		if (!kernel::arch::x86_64::apic::lapic::available())
		{
			return;
		}

		kernel::arch::x86_64::apic::lapic::write_timer_div(calibrated_div);
		kernel::arch::x86_64::apic::lapic::set_timer_handler(on_tick);
		kernel::arch::x86_64::apic::lapic::write_lvt_timer(static_cast<uint32_t>(vector_timer) | lvt_periodic);
		kernel::arch::x86_64::apic::lapic::write_timer_init(calibrated_init_count);
	}

	bool init_calibrated(uint32_t hz) noexcept
	{
		if (hz == 0)
		{
			return false;
		}

		if (!kernel::arch::x86_64::apic::lapic::available())
		{
			return false;
		}

		if (!kernel::time::hpet::available())
		{
			kernel::log::write_line("apic timer needs hpet");
			return false;
		}

		bsp_apic_id = static_cast<uint8_t>(kernel::arch::x86_64::apic::lapic::id() & 0xFFu);

		constexpr uint32_t calib_ms = 100;
		const uint64_t calib_ns = static_cast<uint64_t>(calib_ms) * 1000ull * 1000ull;

		/* Start masked to avoid stray interrupts during calibration. */
		calibrated_div = 0x3;
		kernel::arch::x86_64::apic::lapic::write_timer_div(calibrated_div);
		kernel::arch::x86_64::apic::lapic::write_lvt_timer(static_cast<uint32_t>(vector_timer) | lvt_masked);

		/* Use HPET as a wall clock and count down from a known init value to derive LAPIC ticks. */
		kernel::arch::x86_64::apic::lapic::write_timer_init(0xFFFFFFFFu);
		const uint64_t hpet_start = kernel::time::hpet::ns_since_boot();
		kernel::time::hpet::busy_wait_ns(calib_ns);
		const uint64_t hpet_end = kernel::time::hpet::ns_since_boot();
		const uint32_t cur = kernel::arch::x86_64::apic::lapic::read_timer_cur();
		const uint32_t elapsed = 0xFFFFFFFFu - cur;
		const uint64_t hpet_delta = hpet_end - hpet_start;

		if (elapsed == 0)
		{
			kernel::log::write_line("apic timer calibration failed");
			return false;
		}

		active_hz = hz;

		const uint64_t denom = static_cast<uint64_t>(calib_ms) * static_cast<uint64_t>(hz);
		const uint64_t numer = static_cast<uint64_t>(elapsed) * 1000ull;
		uint64_t init64 = (numer + (denom / 2)) / denom;
		if (init64 == 0)
		{
			init64 = 1;
		}

		const uint32_t init = static_cast<uint32_t>(init64 > 0xFFFFFFFFull ? 0xFFFFFFFFull : init64);
		calibrated_init_count = init;
		const uint64_t ticks_per_ms64 = (static_cast<uint64_t>(elapsed) + (static_cast<uint64_t>(calib_ms) / 2)) / static_cast<uint64_t>(calib_ms);
		ticks_per_ms = static_cast<uint32_t>(ticks_per_ms64 == 0 ? 1 : ticks_per_ms64);

		init_cpu();

		kernel::log::write("apic calib ns=");
		kernel::log::write_u64_dec(hpet_delta);
		kernel::log::write(" target=");
		kernel::log::write_u64_dec(calib_ns);
		kernel::log::write(" elapsed=");
		kernel::log::write_u64_dec(elapsed);
		kernel::log::write(" init=");
		kernel::log::write_u64_dec(init);
		kernel::log::write("\n", 1);

		kernel::log::write("apic timer ticks_per_ms=");
		kernel::log::write_u64_dec(ticks_per_ms);
		kernel::log::write(" hz=");
		kernel::log::write_u64_dec(active_hz);
		kernel::log::write("\n", 1);

		return true;
	}
}
