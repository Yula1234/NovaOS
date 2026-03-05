#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/interrupt_frame.hpp"

namespace kernel::arch::x86_64::irq
{
	/* Legacy PIC-style IRQ numbers (0..15). In APIC mode these come from vectors 0x20..0x2F by convention. */
	using Handler = void (*)(kernel::arch::x86_64::InterruptFrameView*) noexcept;

	void set_handler(uint8_t irq, Handler handler) noexcept;
	void dispatch(uint8_t irq, kernel::arch::x86_64::InterruptFrameView* frame) noexcept;
}
