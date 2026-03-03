#include "kernel/arch/x86_64/apic/ioapic.hpp"

#include "kernel/log/log.hpp"
#include "kernel/mm/ioremap.hpp"

namespace
{
	constexpr uint32_t reg_sel = 0x00;
	constexpr uint32_t reg_win = 0x10;

	constexpr uint32_t ioreg_id = 0x00;
	constexpr uint32_t ioreg_ver = 0x01;

	constexpr uint32_t redtbl_base = 0x10;

	volatile uint32_t* io = nullptr;
	uint32_t base_gsi = 0;

	uint32_t read_ioreg(uint8_t reg) noexcept
	{
		io[reg_sel / 4] = reg;
		return io[reg_win / 4];
	}

	void write_ioreg(uint8_t reg, uint32_t value) noexcept
	{
		io[reg_sel / 4] = reg;
		io[reg_win / 4] = value;
	}

	uint32_t max_redir() noexcept
	{
		const uint32_t ver = read_ioreg(static_cast<uint8_t>(ioreg_ver));
		return (ver >> 16) & 0xFFu;
	}

	uint64_t make_redir(uint8_t vector, uint32_t dest_apic_id, uint16_t flags) noexcept
	{
		uint64_t value = vector;

		const bool active_low = (flags & (1u << 1)) != 0;
		const bool level = (flags & (1u << 3)) != 0;

		if (active_low)
		{
			value |= 1ull << 13;
		}

		if (level)
		{
			value |= 1ull << 15;
		}

		value |= static_cast<uint64_t>(dest_apic_id) << 56;
		return value;
	}

	void write_redir(uint32_t index, uint64_t value) noexcept
	{
		const uint8_t low = static_cast<uint8_t>(redtbl_base + index * 2);
		const uint8_t high = static_cast<uint8_t>(redtbl_base + index * 2 + 1);

		write_ioreg(high, static_cast<uint32_t>(value >> 32));
		write_ioreg(low, static_cast<uint32_t>(value));
	}
}

namespace kernel::arch::x86_64::apic::ioapic
{
	void init(uint64_t ioapic_phys, uint32_t gsi_base) noexcept
	{
		io = static_cast<volatile uint32_t*>(kernel::mm::ioremap::map(ioapic_phys, 0x20));
		base_gsi = gsi_base;

		const uint32_t id = read_ioreg(static_cast<uint8_t>(ioreg_id)) >> 24;
		kernel::log::write("ioapic id=");
		kernel::log::write_u64_dec(id);
		kernel::log::write(" max=");
		kernel::log::write_u64_dec(max_redir());
		kernel::log::write("\n", 1);
	}

	void route_irq(uint32_t gsi, uint8_t vector, uint32_t dest_apic_id, uint16_t flags) noexcept
	{
		if (gsi < base_gsi)
		{
			return;
		}

		const uint32_t index = gsi - base_gsi;
		if (index > max_redir())
		{
			return;
		}

		const uint64_t redir = make_redir(vector, dest_apic_id, flags);
		write_redir(index, redir);
	}
}
