#pragma once

#include <stdint.h>

namespace kernel::arch::x86_64::pic
{
	/* Remap IRQ0..15 to IDT vectors starting at master_offset and slave_offset. */
	void remap(uint8_t master_offset, uint8_t slave_offset) noexcept;
	void set_mask(uint16_t mask) noexcept;
	uint16_t get_mask() noexcept;
	void eoi(uint8_t irq) noexcept;
}
