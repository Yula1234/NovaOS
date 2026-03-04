#include "kernel/arch/x86_64/apic/lapic.hpp"

#include "kernel/arch/x86_64/cpu.hpp"
#include "kernel/log/log.hpp"
#include "kernel/mm/ioremap.hpp"

namespace
{
	constexpr uint32_t reg_id = 0x20;
	constexpr uint32_t reg_eoi = 0xB0;
	constexpr uint32_t reg_svr = 0xF0;
	constexpr uint32_t reg_esr = 0x280;
	constexpr uint32_t reg_icr_low = 0x300;
	constexpr uint32_t reg_icr_high = 0x310;
	constexpr uint32_t reg_lvt_timer = 0x320;
	constexpr uint32_t reg_timer_init = 0x380;
	constexpr uint32_t reg_timer_cur = 0x390;
	constexpr uint32_t reg_timer_div = 0x3E0;

	constexpr uint32_t svr_enable = 1u << 8;

	constexpr uint32_t icr_delivery_init = 0b101u << 8;
	constexpr uint32_t icr_delivery_startup = 0b110u << 8;
	constexpr uint32_t icr_delivery_nmi = 0b100u << 8;
	constexpr uint32_t icr_level_assert = 1u << 14;
	constexpr uint32_t icr_level_deassert = 0u << 14;
	constexpr uint32_t icr_trigger_level = 1u << 15;
	constexpr uint32_t icr_trigger_edge = 0u << 15;

	volatile uint32_t* lapic_regs = nullptr;
	void (*timer_handler)() noexcept = nullptr;
	void (*ipi_handler)() noexcept = nullptr;
	bool lapic_ready = false;

	inline uint32_t read_reg(uint32_t off) noexcept
	{
		return lapic_regs[off / 4];
	}

	inline void write_reg(uint32_t off, uint32_t value) noexcept
	{
		lapic_regs[off / 4] = value;
	}

	inline void clear_esr() noexcept
	{
		write_reg(reg_esr, 0);
		write_reg(reg_esr, 0);
		(void)read_reg(reg_esr);
	}

	void handle_timer_vector() noexcept
	{
		if (timer_handler)
		{
			timer_handler();
			return;
		}

		kernel::arch::x86_64::apic::lapic::eoi();
	}

	void handle_ipi_vector() noexcept
	{
		if (ipi_handler)
		{
			ipi_handler();
			kernel::arch::x86_64::apic::lapic::eoi();
			return;
		}

		kernel::arch::x86_64::apic::lapic::eoi();
	}

	inline void write_icr(uint32_t high, uint32_t low) noexcept
	{
		write_reg(reg_icr_high, high);
		write_reg(reg_icr_low, low);

		while ((read_reg(reg_icr_low) & (1u << 12)) != 0)
		{
			asm volatile("pause");
		}
	}

	inline void enable_apic_msr() noexcept
	{
		constexpr uint32_t ia32_apic_base = 0x1B;
		constexpr uint64_t apic_enable = 1ull << 11;
		constexpr uint64_t x2apic_enable = 1ull << 10;

		const uint64_t base = kernel::arch::x86_64::rdmsr(ia32_apic_base);
		kernel::arch::x86_64::wrmsr(
			ia32_apic_base,
			(base | apic_enable) & ~x2apic_enable
		);
	}

	inline void enable_spurious_vector() noexcept
	{
		const uint32_t svr = read_reg(reg_svr);
		write_reg(reg_svr, (svr & 0xFFFFFF00u) | svr_enable | 0xFFu);
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
		if (lapic_regs == nullptr)
		{
			lapic_regs = static_cast<volatile uint32_t*>(kernel::mm::ioremap::map(lapic_phys, 0x1000));
			lapic_ready = lapic_regs != nullptr;
		}

		if (lapic_ready)
		{
			init_cpu();
		}

		constexpr uint32_t ia32_apic_base = 0x1B;
		const uint64_t apic_base = kernel::arch::x86_64::rdmsr(ia32_apic_base);
		kernel::log::write("apic_base_msr=");
		kernel::log::write_u64_hex(apic_base);
		kernel::log::write(" x2apic=");
		kernel::log::write_u64_dec((apic_base & (1ull << 10)) != 0);
		kernel::log::write("\n", 1);

		kernel::log::write("lapic id=");
		kernel::log::write_u64_hex(id());
		kernel::log::write("\n", 1);
	}

	void init_cpu() noexcept
	{
		if (!lapic_ready || lapic_regs == nullptr)
		{
			return;
		}

		enable_apic_msr();
		enable_spurious_vector();
	}

	void eoi() noexcept
	{
		write_reg(reg_eoi, 0);
	}

	uint32_t id() noexcept
	{
		return read_reg(reg_id) >> 24;
	}

	void send_ipi(uint32_t dest_apic_id, uint8_t vector) noexcept
	{
		const uint32_t high = (dest_apic_id & 0xFFu) << 24;
		const uint32_t low = static_cast<uint32_t>(vector);
		write_icr(high, low);
	}

	void broadcast_ipi(uint8_t vector, bool include_self) noexcept
	{
		const uint32_t shorthand = include_self ? (2u << 18) : (3u << 18);
		const uint32_t low = static_cast<uint32_t>(vector) | shorthand;
		write_icr(0, low);
	}

	void broadcast_nmi(bool include_self) noexcept
	{
		const uint32_t shorthand = include_self ? (2u << 18) : (3u << 18);
		write_icr(0, icr_delivery_nmi | icr_level_assert | icr_trigger_edge | shorthand);
	}

	void broadcast_init_ipi_assert(bool include_self) noexcept
	{
		const uint32_t shorthand = include_self ? (2u << 18) : (3u << 18);
		write_icr(0, icr_delivery_init | icr_level_assert | icr_trigger_level | shorthand);
	}

	void broadcast_init_ipi_deassert(bool include_self) noexcept
	{
		const uint32_t shorthand = include_self ? (2u << 18) : (3u << 18);
		write_icr(0, icr_delivery_init | icr_level_deassert | icr_trigger_level | shorthand);
	}

	void broadcast_startup_ipi(uint8_t startup_vector, bool include_self) noexcept
	{
		const uint32_t shorthand = include_self ? (2u << 18) : (3u << 18);
		const uint32_t vec = static_cast<uint32_t>(startup_vector);
		write_icr(0, icr_delivery_startup | icr_level_assert | icr_trigger_edge | vec | shorthand);
	}

	void send_init_ipi_assert(uint32_t dest_apic_id) noexcept
	{
		const uint32_t high = (dest_apic_id & 0xFFu) << 24;
		clear_esr();
		write_icr(high, icr_delivery_init | icr_level_assert | icr_trigger_level);
	}

	void send_init_ipi_deassert(uint32_t dest_apic_id) noexcept
	{
		const uint32_t high = (dest_apic_id & 0xFFu) << 24;
		clear_esr();
		write_icr(high, icr_delivery_init | icr_level_deassert | icr_trigger_level);
	}

	void send_init_ipi_edge(uint32_t dest_apic_id) noexcept
	{
		const uint32_t high = (dest_apic_id & 0xFFu) << 24;
		clear_esr();
		write_icr(high, icr_delivery_init | icr_trigger_edge);
	}

	void send_startup_ipi(uint32_t dest_apic_id, uint8_t startup_vector) noexcept
	{
		const uint32_t high = (dest_apic_id & 0xFFu) << 24;
		const uint32_t vec = static_cast<uint32_t>(startup_vector);
		clear_esr();
		write_icr(high, icr_delivery_startup | icr_level_assert | icr_trigger_edge | vec);
	}

	uint32_t read_esr() noexcept
	{
		return read_reg(reg_esr);
	}

	uint32_t read_icr_low() noexcept
	{
		return read_reg(reg_icr_low);
	}

	uint32_t read_icr_high() noexcept
	{
		return read_reg(reg_icr_high);
	}

	void set_ipi_handler(void (*handler)() noexcept) noexcept
	{
		ipi_handler = handler;
	}

	void set_timer_handler(void (*handler)() noexcept) noexcept
	{
		timer_handler = handler;
	}

	void handle_timer_vector() noexcept
	{
		::handle_timer_vector();
	}

	void handle_ipi_vector() noexcept
	{
		::handle_ipi_vector();
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
		return nullptr;
	}

	void* ipi_isr() noexcept
	{
		return nullptr;
	}
}
