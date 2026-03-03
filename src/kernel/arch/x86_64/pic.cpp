#include "kernel/arch/x86_64/pic.hpp"

#include "kernel/arch/x86_64/port_io.hpp"

namespace
{
	constexpr uint16_t pic1_cmd = 0x20;
	constexpr uint16_t pic1_data = 0x21;
	constexpr uint16_t pic2_cmd = 0xA0;
	constexpr uint16_t pic2_data = 0xA1;

	constexpr uint8_t icw1_init = 0x10;
	constexpr uint8_t icw1_icw4 = 0x01;
	constexpr uint8_t icw4_8086 = 0x01;

	constexpr uint8_t pic_eoi = 0x20;
}

namespace kernel::arch::x86_64::pic
{
	void remap(uint8_t master_offset, uint8_t slave_offset) noexcept
	{
		const uint8_t a1 = kernel::arch::x86_64::inb(pic1_data);
		const uint8_t a2 = kernel::arch::x86_64::inb(pic2_data);

		kernel::arch::x86_64::outb(pic1_cmd, icw1_init | icw1_icw4);
		kernel::arch::x86_64::io_wait();
		kernel::arch::x86_64::outb(pic2_cmd, icw1_init | icw1_icw4);
		kernel::arch::x86_64::io_wait();

		kernel::arch::x86_64::outb(pic1_data, master_offset);
		kernel::arch::x86_64::io_wait();
		kernel::arch::x86_64::outb(pic2_data, slave_offset);
		kernel::arch::x86_64::io_wait();

		kernel::arch::x86_64::outb(pic1_data, 4);
		kernel::arch::x86_64::io_wait();
		kernel::arch::x86_64::outb(pic2_data, 2);
		kernel::arch::x86_64::io_wait();

		kernel::arch::x86_64::outb(pic1_data, icw4_8086);
		kernel::arch::x86_64::io_wait();
		kernel::arch::x86_64::outb(pic2_data, icw4_8086);
		kernel::arch::x86_64::io_wait();

		kernel::arch::x86_64::outb(pic1_data, a1);
		kernel::arch::x86_64::outb(pic2_data, a2);
	}

	void set_mask(uint16_t mask) noexcept
	{
		kernel::arch::x86_64::outb(pic1_data, static_cast<uint8_t>(mask & 0xFF));
		kernel::arch::x86_64::outb(pic2_data, static_cast<uint8_t>((mask >> 8) & 0xFF));
	}

	uint16_t get_mask() noexcept
	{
		const uint8_t a1 = kernel::arch::x86_64::inb(pic1_data);
		const uint8_t a2 = kernel::arch::x86_64::inb(pic2_data);

		return static_cast<uint16_t>(a1) | (static_cast<uint16_t>(a2) << 8);
	}

	void eoi(uint8_t irq) noexcept
	{
		if (irq >= 8)
		{
			kernel::arch::x86_64::outb(pic2_cmd, pic_eoi);
		}

		kernel::arch::x86_64::outb(pic1_cmd, pic_eoi);
	}
}
