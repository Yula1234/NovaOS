#include "kernel/arch/x86_64/pit.hpp"

#include <atomic>

#include "kernel/arch/x86_64/irq.hpp"
#include "kernel/arch/x86_64/port_io.hpp"

namespace
{
	constexpr uint32_t pit_base_hz = 1193182;

	constexpr uint16_t pit_ch0 = 0x40;
	constexpr uint16_t pit_cmd = 0x43;

	constexpr uint8_t cmd_ch0 = 0b00'000'000;
	constexpr uint8_t cmd_access_lohi = 0b00'110'000;
	constexpr uint8_t cmd_mode3 = 0b00'000'110;
	constexpr uint8_t cmd_binary = 0;

	std::atomic<uint64_t> tick_counter{0};
	uint32_t active_frequency_hz = 0;

	void on_irq0(kernel::arch::x86_64::InterruptFrame*) noexcept
	{
		tick_counter.fetch_add(1, std::memory_order_relaxed);
	}

	uint16_t compute_divisor(uint32_t frequency_hz) noexcept
	{
		if (frequency_hz == 0)
		{
			return 0;
		}

		uint32_t divisor = pit_base_hz / frequency_hz;
		if (divisor < 1)
		{
			divisor = 1;
		}

		if (divisor > 0xFFFF)
		{
			divisor = 0xFFFF;
		}

		return static_cast<uint16_t>(divisor);
	}
}

namespace kernel::arch::x86_64::pit
{
	void init(uint32_t frequency_hz) noexcept
	{
		const uint16_t divisor = compute_divisor(frequency_hz);
		active_frequency_hz = divisor != 0 ? (pit_base_hz / divisor) : 0;

		kernel::arch::x86_64::irq::set_handler(0, on_irq0);

		const uint8_t command = static_cast<uint8_t>(cmd_ch0 | cmd_access_lohi | cmd_mode3 | cmd_binary);
		kernel::arch::x86_64::outb(pit_cmd, command);
		kernel::arch::x86_64::outb(pit_ch0, static_cast<uint8_t>(divisor & 0xFF));
		kernel::arch::x86_64::outb(pit_ch0, static_cast<uint8_t>((divisor >> 8) & 0xFF));
	}

	uint64_t ticks() noexcept
	{
		return tick_counter.load(std::memory_order_relaxed);
	}

	uint32_t frequency_hz() noexcept
	{
		return active_frequency_hz;
	}
}
