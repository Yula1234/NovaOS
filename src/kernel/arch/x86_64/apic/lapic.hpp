#pragma once

#include <stdint.h>

namespace kernel::arch::x86_64::apic::lapic
{
	bool available() noexcept;

	void init(uint64_t lapic_phys) noexcept;

	void eoi() noexcept;

	uint32_t id() noexcept;

	void set_timer_handler(void (*handler)() noexcept) noexcept;

	void write_timer_div(uint32_t value) noexcept;
	void write_lvt_timer(uint32_t value) noexcept;
	void write_timer_init(uint32_t value) noexcept;
	uint32_t read_timer_cur() noexcept;

	void* timer_isr() noexcept;
}
