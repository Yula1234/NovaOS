#include "kernel/arch/x86_64/idt.hpp"

#include <stddef.h>

#include "kernel/arch/x86_64/cpu.hpp"
#include "kernel/arch/x86_64/interrupt_frame.hpp"
#include "kernel/arch/x86_64/apic/lapic.hpp"
#include "kernel/arch/x86_64/irq.hpp"
#include "kernel/log/log.hpp"

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

	IdtEntry make_entry(void (*handler)(), uint8_t type_attr) noexcept
	{
		const uint64_t addr = reinterpret_cast<uint64_t>(handler);

		IdtEntry e{};
		e.offset_0_15 = static_cast<uint16_t>(addr);
		e.selector = 0x08;
		e.ist = 0;
		e.type_attr = type_attr;
		e.offset_16_31 = static_cast<uint16_t>(addr >> 16);
		e.offset_32_63 = static_cast<uint32_t>(addr >> 32);
		e.zero = 0;

		return e;
	}

	constexpr uint8_t interrupt_gate = 0x8E;

	[[noreturn]] void hang() noexcept
	{
		kernel::arch::x86_64::halt_forever();
	}

	void write_frame(const char* prefix, kernel::arch::x86_64::InterruptFrame* frame) noexcept
	{
		kernel::log::write(prefix);
		kernel::log::write(" rip=");
		kernel::log::write_u64_hex(frame ? frame->rip : 0);
		kernel::log::write(" cs=");
		kernel::log::write_u64_hex(frame ? frame->cs : 0);
		kernel::log::write(" rflags=");
		kernel::log::write_u64_hex(frame ? frame->rflags : 0);
		kernel::log::write("\n", 1);
	}

	void write_error_code(uint64_t error_code) noexcept
	{
		kernel::log::write(" error=");
		kernel::log::write_u64_hex(error_code);
		kernel::log::write("\n", 1);
	}

	[[gnu::interrupt]] void isr_default(kernel::arch::x86_64::InterruptFrame* frame) noexcept
	{
		kernel::log::write_line("exception");
		write_frame("frame", frame);
		hang();
	}

	[[gnu::interrupt]] void isr_gpf(kernel::arch::x86_64::InterruptFrame* frame, uint64_t error_code) noexcept
	{
		kernel::log::write_line("#GP");
		write_frame("frame", frame);
		write_error_code(error_code);
		hang();
	}

	[[gnu::interrupt]] void isr_page_fault(kernel::arch::x86_64::InterruptFrame* frame, uint64_t error_code) noexcept
	{
		kernel::log::write_line("#PF");
		write_frame("frame", frame);
		kernel::log::write(" cr2=");
		kernel::log::write_u64_hex(kernel::arch::x86_64::read_cr2());
		kernel::log::write("\n", 1);
		write_error_code(error_code);
		hang();
	}

	[[gnu::interrupt]] void isr_irq0(kernel::arch::x86_64::InterruptFrame* frame) noexcept { kernel::arch::x86_64::irq::dispatch(0, frame); }
	[[gnu::interrupt]] void isr_irq1(kernel::arch::x86_64::InterruptFrame* frame) noexcept { kernel::arch::x86_64::irq::dispatch(1, frame); }
	[[gnu::interrupt]] void isr_irq2(kernel::arch::x86_64::InterruptFrame* frame) noexcept { kernel::arch::x86_64::irq::dispatch(2, frame); }
	[[gnu::interrupt]] void isr_irq3(kernel::arch::x86_64::InterruptFrame* frame) noexcept { kernel::arch::x86_64::irq::dispatch(3, frame); }
	[[gnu::interrupt]] void isr_irq4(kernel::arch::x86_64::InterruptFrame* frame) noexcept { kernel::arch::x86_64::irq::dispatch(4, frame); }
	[[gnu::interrupt]] void isr_irq5(kernel::arch::x86_64::InterruptFrame* frame) noexcept { kernel::arch::x86_64::irq::dispatch(5, frame); }
	[[gnu::interrupt]] void isr_irq6(kernel::arch::x86_64::InterruptFrame* frame) noexcept { kernel::arch::x86_64::irq::dispatch(6, frame); }
	[[gnu::interrupt]] void isr_irq7(kernel::arch::x86_64::InterruptFrame* frame) noexcept { kernel::arch::x86_64::irq::dispatch(7, frame); }
	[[gnu::interrupt]] void isr_irq8(kernel::arch::x86_64::InterruptFrame* frame) noexcept { kernel::arch::x86_64::irq::dispatch(8, frame); }
	[[gnu::interrupt]] void isr_irq9(kernel::arch::x86_64::InterruptFrame* frame) noexcept { kernel::arch::x86_64::irq::dispatch(9, frame); }
	[[gnu::interrupt]] void isr_irq10(kernel::arch::x86_64::InterruptFrame* frame) noexcept { kernel::arch::x86_64::irq::dispatch(10, frame); }
	[[gnu::interrupt]] void isr_irq11(kernel::arch::x86_64::InterruptFrame* frame) noexcept { kernel::arch::x86_64::irq::dispatch(11, frame); }
	[[gnu::interrupt]] void isr_irq12(kernel::arch::x86_64::InterruptFrame* frame) noexcept { kernel::arch::x86_64::irq::dispatch(12, frame); }
	[[gnu::interrupt]] void isr_irq13(kernel::arch::x86_64::InterruptFrame* frame) noexcept { kernel::arch::x86_64::irq::dispatch(13, frame); }
	[[gnu::interrupt]] void isr_irq14(kernel::arch::x86_64::InterruptFrame* frame) noexcept { kernel::arch::x86_64::irq::dispatch(14, frame); }
	[[gnu::interrupt]] void isr_irq15(kernel::arch::x86_64::InterruptFrame* frame) noexcept { kernel::arch::x86_64::irq::dispatch(15, frame); }

	void set_isr(uint8_t vector, void (*handler)()) noexcept
	{
		idt_table[vector] = make_entry(handler, interrupt_gate);
	}
}

namespace kernel::arch::x86_64::idt
{
	void init() noexcept
	{
		for (size_t i = 0; i < 256; ++i)
		{
			idt_table[i] = make_entry(reinterpret_cast<void (*)()>(isr_default), interrupt_gate);
		}

		set_isr(13, reinterpret_cast<void (*)()>(isr_gpf));
		set_isr(14, reinterpret_cast<void (*)()>(isr_page_fault));

		set_isr(0x20, reinterpret_cast<void (*)()>(isr_irq0));
		set_isr(0x21, reinterpret_cast<void (*)()>(isr_irq1));
		set_isr(0x22, reinterpret_cast<void (*)()>(isr_irq2));
		set_isr(0x23, reinterpret_cast<void (*)()>(isr_irq3));
		set_isr(0x24, reinterpret_cast<void (*)()>(isr_irq4));
		set_isr(0x25, reinterpret_cast<void (*)()>(isr_irq5));
		set_isr(0x26, reinterpret_cast<void (*)()>(isr_irq6));
		set_isr(0x27, reinterpret_cast<void (*)()>(isr_irq7));
		set_isr(0x28, reinterpret_cast<void (*)()>(isr_irq8));
		set_isr(0x29, reinterpret_cast<void (*)()>(isr_irq9));
		set_isr(0x2A, reinterpret_cast<void (*)()>(isr_irq10));
		set_isr(0x2B, reinterpret_cast<void (*)()>(isr_irq11));
		set_isr(0x2C, reinterpret_cast<void (*)()>(isr_irq12));
		set_isr(0x2D, reinterpret_cast<void (*)()>(isr_irq13));
		set_isr(0x2E, reinterpret_cast<void (*)()>(isr_irq14));
		set_isr(0x2F, reinterpret_cast<void (*)()>(isr_irq15));

		set_isr(0x30, reinterpret_cast<void (*)()>(kernel::arch::x86_64::apic::lapic::timer_isr()));
		set_isr(0x31, reinterpret_cast<void (*)()>(kernel::arch::x86_64::apic::lapic::ipi_isr()));

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
