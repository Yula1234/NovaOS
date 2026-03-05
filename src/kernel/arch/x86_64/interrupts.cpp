#include "kernel/arch/x86_64/interrupts.hpp"

#include "kernel/arch/x86_64/apic/lapic.hpp"
#include "kernel/arch/x86_64/pic.hpp"

namespace
{
	bool apic_active = false;
}

namespace kernel::arch::x86_64::interrupts
{
	void use_pic() noexcept
	{
		apic_active = false;
	}

	void use_apic() noexcept
	{
		apic_active = true;
	}

	void eoi(uint8_t irq) noexcept
	{
		/* In APIC mode EOI is per-LAPIC and independent of legacy IRQ number. */
		if (apic_active)
		{
			kernel::arch::x86_64::apic::lapic::eoi();
			return;
		}

		kernel::arch::x86_64::pic::eoi(irq);
	}
}
