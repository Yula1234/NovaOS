#include "kernel/arch/x86_64/interrupt_frame.hpp"

#include "kernel/arch/x86_64/apic/lapic.hpp"
#include "kernel/arch/x86_64/cpu.hpp"
#include "kernel/arch/x86_64/irq.hpp"
#include "kernel/arch/x86_64/tlb.hpp"
#include "kernel/log/log.hpp"

namespace
{
	[[noreturn]] void hang() noexcept
	{
		kernel::arch::x86_64::halt_forever();
	}

	void write_frame(const char* prefix, const kernel::arch::x86_64::InterruptFrame& frame) noexcept
	{
		kernel::log::write(prefix);
		kernel::log::write(" rip=");
		kernel::log::write_u64_hex(frame.rip);
		kernel::log::write(" cs=");
		kernel::log::write_u64_hex(frame.cs);
		kernel::log::write(" rflags=");
		kernel::log::write_u64_hex(frame.rflags);
		kernel::log::write("\n", 1);
	}

	void write_error_code(uint64_t error_code) noexcept
	{
		kernel::log::write(" error=");
		kernel::log::write_u64_hex(error_code);
		kernel::log::write("\n", 1);
	}

	kernel::arch::x86_64::InterruptFrame to_frame(const kernel::arch::x86_64::InterruptContext* ctx) noexcept
	{
		kernel::arch::x86_64::InterruptFrame f{};
		f.rip = ctx ? ctx->rip : 0;
		f.cs = ctx ? ctx->cs : 0;
		f.rflags = ctx ? ctx->rflags : 0;
		f.rsp = 0;
		f.ss = 0;

		return f;
	}
}

extern "C" void isr_dispatch(kernel::arch::x86_64::InterruptContext* ctx) noexcept
{
	asm volatile("cld" ::: "cc");

	const uint64_t vector = ctx ? ctx->vector : 0;
	const uint64_t error_code = ctx ? ctx->error_code : 0;

	if (vector >= 0x20 && vector <= 0x2F)
	{
		auto frame = to_frame(ctx);
		kernel::arch::x86_64::irq::dispatch(static_cast<uint8_t>(vector - 0x20), &frame);
		return;
	}

	if (vector == 0x30)
	{
		kernel::arch::x86_64::apic::lapic::handle_timer_vector();
		return;
	}

	if (vector == 0x31)
	{
		kernel::arch::x86_64::apic::lapic::handle_ipi_vector();
		return;
	}

	if (vector == 2)
	{
		kernel::arch::x86_64::tlb::on_nmi();
		return;
	}

	if (vector == 13)
	{
		kernel::log::write_line("#GP");
		write_frame("frame", to_frame(ctx));
		write_error_code(error_code);
		hang();
	}

	if (vector == 14)
	{
		kernel::log::write_line("#PF");
		write_frame("frame", to_frame(ctx));
		kernel::log::write(" cr2=");
		kernel::log::write_u64_hex(kernel::arch::x86_64::read_cr2());
		kernel::log::write("\n", 1);
		write_error_code(error_code);
		hang();
	}

	kernel::log::write_line("exception");
	write_frame("frame", to_frame(ctx));
	write_error_code(error_code);
	kernel::log::write(" vector=");
	kernel::log::write_u64_hex(vector);
	kernel::log::write("\n", 1);
	
	hang();
}
