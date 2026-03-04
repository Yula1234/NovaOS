#include "kernel/arch/x86_64/interrupt_frame.hpp"

#include "kernel/arch/x86_64/apic/lapic.hpp"
#include "kernel/arch/x86_64/cpu.hpp"
#include "kernel/arch/x86_64/irq.hpp"
#include "kernel/arch/x86_64/tlb.hpp"
#include "kernel/log/log.hpp"

namespace
{
	using Handler = void (*)(kernel::arch::x86_64::InterruptContext*) noexcept;

	Handler isr_handlers[256]{};
	volatile uint8_t handlers_ready = 0;

	[[noreturn]] void hang() noexcept
	{
		kernel::arch::x86_64::halt_forever();
	}

	void write_frame(const char* prefix, const kernel::arch::x86_64::InterruptFrameView& frame) noexcept
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

	kernel::arch::x86_64::InterruptFrameView frame_view(const kernel::arch::x86_64::InterruptContext* ctx) noexcept
	{
		kernel::arch::x86_64::InterruptFrameView f{};
		f.rip = ctx ? ctx->rip : 0;
		f.cs = ctx ? ctx->cs : 0;
		f.rflags = ctx ? ctx->rflags : 0;

		return f;
	}

	void dispatch_irq(kernel::arch::x86_64::InterruptContext* ctx) noexcept
	{
		const uint8_t irq = static_cast<uint8_t>(ctx->vector - 0x20);
		auto f = frame_view(ctx);
		kernel::arch::x86_64::irq::dispatch(irq, &f);
	}

	void dispatch_timer(kernel::arch::x86_64::InterruptContext*) noexcept
	{
		kernel::arch::x86_64::apic::lapic::handle_timer_vector();
	}

	void dispatch_ipi(kernel::arch::x86_64::InterruptContext*) noexcept
	{
		kernel::arch::x86_64::apic::lapic::handle_ipi_vector();
	}

	void dispatch_nmi(kernel::arch::x86_64::InterruptContext*) noexcept
	{
		kernel::arch::x86_64::tlb::on_nmi();
	}

	void dispatch_gp(kernel::arch::x86_64::InterruptContext* ctx) noexcept
	{
		kernel::log::write_line("#GP");
		write_frame("frame", frame_view(ctx));
		write_error_code(ctx ? ctx->error_code : 0);
		hang();
	}

	void dispatch_pf(kernel::arch::x86_64::InterruptContext* ctx) noexcept
	{
		kernel::log::write_line("#PF");
		write_frame("frame", frame_view(ctx));
		kernel::log::write(" cr2=");
		kernel::log::write_u64_hex(kernel::arch::x86_64::read_cr2());
		kernel::log::write("\n", 1);
		write_error_code(ctx ? ctx->error_code : 0);
		hang();
	}

	void dispatch_unhandled(kernel::arch::x86_64::InterruptContext* ctx) noexcept
	{
		const uint64_t vector = ctx ? ctx->vector : 0;
		const uint64_t error_code = ctx ? ctx->error_code : 0;

		kernel::log::write_line("exception");
		write_frame("frame", frame_view(ctx));
		write_error_code(error_code);
		kernel::log::write(" vector=");
		kernel::log::write_u64_hex(vector);
		kernel::log::write("\n", 1);

		hang();
	}

	void init_handlers() noexcept
	{
		if (__atomic_load_n(&handlers_ready, __ATOMIC_ACQUIRE) != 0)
		{
			return;
		}

		for (auto& h : isr_handlers)
		{
			h = dispatch_unhandled;
		}

		for (uint32_t v = 0x20; v <= 0x2F; ++v)
		{
			isr_handlers[v] = dispatch_irq;
		}

		isr_handlers[0x30] = dispatch_timer;
		isr_handlers[0x31] = dispatch_ipi;
		isr_handlers[2] = dispatch_nmi;
		isr_handlers[13] = dispatch_gp;
		isr_handlers[14] = dispatch_pf;

		__atomic_store_n(&handlers_ready, static_cast<uint8_t>(1), __ATOMIC_RELEASE);
	}
}

extern "C" void isr_dispatch(kernel::arch::x86_64::InterruptContext* ctx) noexcept
{
	asm volatile("cld" ::: "cc");
	init_handlers();

	const uint64_t vector = ctx ? ctx->vector : 0;
	if (const auto h = isr_handlers[vector & 0xFFu])
	{
		h(ctx);
		return;
	}

	dispatch_unhandled(ctx);
}
