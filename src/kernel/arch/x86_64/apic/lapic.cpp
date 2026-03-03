#include "kernel/arch/x86_64/apic/lapic.hpp"

#include "kernel/arch/x86_64/cpu.hpp"
#include "kernel/arch/x86_64/interrupt_frame.hpp"
#include "kernel/log/log.hpp"
#include "kernel/mm/ioremap.hpp"

namespace
{
	constexpr uint32_t reg_id = 0x20;
	constexpr uint32_t reg_eoi = 0xB0;
	constexpr uint32_t reg_svr = 0xF0;
	constexpr uint32_t reg_lvt_timer = 0x320;
	constexpr uint32_t reg_timer_init = 0x380;
	constexpr uint32_t reg_timer_cur = 0x390;
	constexpr uint32_t reg_timer_div = 0x3E0;

	constexpr uint32_t svr_enable = 1u << 8;

	volatile uint32_t* lapic_regs = nullptr;
	void (*timer_handler)() noexcept = nullptr;
	bool lapic_ready = false;

	inline uint32_t read_reg(uint32_t off) noexcept
	{
		return lapic_regs[off / 4];
	}

	inline void write_reg(uint32_t off, uint32_t value) noexcept
	{
		lapic_regs[off / 4] = value;
	}

	[[gnu::interrupt]] void isr_timer(kernel::arch::x86_64::InterruptFrame*) noexcept
	{
		if (timer_handler)
		{
			timer_handler();
			return;
		}

		kernel::arch::x86_64::apic::lapic::eoi();
	}
}

namespace kernel::arch::x86_64::apic::lapic
{
	bool available() noexcept
	{
		return lapic_ready && lapic_regs != nullptr;
	}

	void init(uint64_t lapic_phys) noexcept
	{
		constexpr uint32_t ia32_apic_base = 0x1B;
		constexpr uint64_t apic_enable = 1ull << 11;

		const uint64_t base = kernel::arch::x86_64::rdmsr(ia32_apic_base);
		kernel::arch::x86_64::wrmsr(ia32_apic_base, base | apic_enable);

		lapic_regs = static_cast<volatile uint32_t*>(kernel::mm::ioremap::map(lapic_phys, 0x1000));
		lapic_ready = lapic_regs != nullptr;

		const uint32_t svr = read_reg(reg_svr);
		write_reg(reg_svr, (svr & 0xFFFFFF00u) | svr_enable | 0xFFu);

		kernel::log::write("lapic id=");
		kernel::log::write_u64_hex(id());
		kernel::log::write("\n", 1);
	}

	void eoi() noexcept
	{
		write_reg(reg_eoi, 0);
	}

	uint32_t id() noexcept
	{
		return read_reg(reg_id) >> 24;
	}

	void set_timer_handler(void (*handler)() noexcept) noexcept
	{
		timer_handler = handler;
	}

	void write_timer_div(uint32_t value) noexcept
	{
		write_reg(reg_timer_div, value);
	}

	void write_lvt_timer(uint32_t value) noexcept
	{
		write_reg(reg_lvt_timer, value);
	}

	void write_timer_init(uint32_t value) noexcept
	{
		write_reg(reg_timer_init, value);
	}

	uint32_t read_timer_cur() noexcept
	{
		return read_reg(reg_timer_cur);
	}

	void* timer_isr() noexcept
	{
		return reinterpret_cast<void*>(&isr_timer);
	}
}
