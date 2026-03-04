#include "kernel/arch/x86_64/idt.hpp"

#include <stddef.h>

#include "kernel/arch/x86_64/cpu.hpp"
#include "kernel/arch/x86_64/gdt.hpp"

extern "C" void* isr_stub_table[256];

namespace
{
	struct [[gnu::packed]] IdtEntry
	{
		uint16_t offset_0_15;
		uint16_t selector;
		uint8_t ist;
		uint8_t type_attr;
		uint16_t offset_16_31;
		uint32_t offset_32_63;
		uint32_t zero;
	};

	static_assert(sizeof(IdtEntry) == 16);

	IdtEntry idt_table[256];
	kernel::arch::x86_64::Idtr active_idtr{};

	IdtEntry make_entry(void (*handler)(), uint8_t type_attr, uint8_t ist) noexcept
	{
		const uint64_t addr = reinterpret_cast<uint64_t>(handler);

		IdtEntry e{};
		e.offset_0_15 = static_cast<uint16_t>(addr);
		e.selector = kernel::arch::x86_64::gdt::selectors::kernel_code;
		e.ist = static_cast<uint8_t>(ist & 0x7u);
		e.type_attr = type_attr;
		e.offset_16_31 = static_cast<uint16_t>(addr >> 16);
		e.offset_32_63 = static_cast<uint32_t>(addr >> 32);
		e.zero = 0;

		return e;
	}

	constexpr uint8_t interrupt_gate = 0x8E;

	void set_isr_ist(uint8_t vector, uint8_t ist) noexcept
	{
		idt_table[vector] = make_entry(reinterpret_cast<void (*)()>(isr_stub_table[vector]), interrupt_gate, ist);
	}

	void set_isr(uint8_t vector) noexcept
	{
		set_isr_ist(vector, 0);
	}
}

namespace kernel::arch::x86_64::idt
{
	void init() noexcept
	{
		for (size_t i = 0; i < 256; ++i)
		{
			set_isr(static_cast<uint8_t>(i));
		}

		set_isr_ist(14, 3);
		set_isr_ist(8, 1);
		set_isr_ist(2, 2);

		active_idtr.limit = static_cast<uint16_t>(sizeof(idt_table) - 1);
		active_idtr.base = reinterpret_cast<uint64_t>(&idt_table[0]);

		kernel::arch::x86_64::lidt(active_idtr);
	}

	void reload() noexcept
	{
		if (active_idtr.limit == 0 || active_idtr.base == 0)
		{
			return;
		}

		kernel::arch::x86_64::lidt(active_idtr);
	}
}
