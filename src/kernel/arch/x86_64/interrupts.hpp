#pragma once

#include <stdint.h>

namespace kernel::arch::x86_64::interrupts
{
	/* Controls which controller receives EOI (legacy PIC vs LAPIC). */
	void use_pic() noexcept;
	void use_apic() noexcept;

	void eoi(uint8_t irq) noexcept;
}
