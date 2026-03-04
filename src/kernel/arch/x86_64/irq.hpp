#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/interrupt_frame.hpp"

namespace kernel::arch::x86_64::irq
{
	using Handler = void (*)(kernel::arch::x86_64::InterruptFrameView*) noexcept;

	void set_handler(uint8_t irq, Handler handler) noexcept;
	void dispatch(uint8_t irq, kernel::arch::x86_64::InterruptFrameView* frame) noexcept;
}
