#include "kernel/arch/x86_64/irq.hpp"

#include "kernel/arch/x86_64/interrupts.hpp"

namespace
{
	/* 8259 PIC exposes 16 IRQ lines. We keep the same indexing even when IOAPIC is active. */
	kernel::arch::x86_64::irq::Handler handlers[16]{};
}

namespace kernel::arch::x86_64::irq
{
	void set_handler(uint8_t irq, Handler handler) noexcept
	{
		if (irq >= 16)
		{
			return;
		}

		handlers[irq] = handler;
	}

	void dispatch(uint8_t irq, kernel::arch::x86_64::InterruptFrameView* frame) noexcept
	{
		/* Handler is optional; we still must ack the interrupt controller to avoid wedging the line. */
		if (irq < 16)
		{
			if (const auto handler = handlers[irq])
			{
				handler(frame);
			}
		}

		kernel::arch::x86_64::interrupts::eoi(irq);
	}
}
