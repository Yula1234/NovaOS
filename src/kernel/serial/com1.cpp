#include "kernel/serial/com1.hpp"

#include "kernel/arch/x86_64/port_io.hpp"

namespace kernel::serial
{
	using kernel::arch::x86_64::inb;
	using kernel::arch::x86_64::outb;

	bool Com1::init() noexcept
	{
		/* 16550 init: disable interrupts, set DLAB, program divisor, 8N1, enable FIFO, enable OUT2. */
		outb(base_port + 1, 0x00);
		outb(base_port + 3, 0x80);
		outb(base_port + 0, 0x03);
		outb(base_port + 1, 0x00);
		outb(base_port + 3, 0x03);
		outb(base_port + 2, 0xC7);
		outb(base_port + 4, 0x0B);

		/* Loopback sanity check; helps catch missing UART or misdecoded ports. */
		const uint8_t test_value = 0xAE;
		outb(base_port + 0, test_value);

		return inb(base_port + 0) == test_value;
	}

	bool Com1::is_transmit_empty() const noexcept
	{
		/* LSR bit 5 = THR empty. */
		return (inb(base_port + 5) & 0x20) != 0;
	}

	void Com1::write_byte(uint8_t value) noexcept
	{
		while (!is_transmit_empty())
		{
		}

		outb(base_port + 0, value);
	}

	void Com1::write(const char* s) noexcept
	{
		if (!s)
		{
			return;
		}

		for (const char* p = s; *p; ++p)
		{
			const char c = *p;
			if (c == '\n')
			{
				/* Many serial terminals expect CRLF. */
				write_byte('\r');
			}

			write_byte(static_cast<uint8_t>(c));
		}
	}

	void Com1::write(const char* s, size_t len) noexcept
	{
		if (!s)
		{
			return;
		}

		for (size_t i = 0; i < len; ++i)
		{
			const char c = s[i];
			if (c == '\n')
			{
				write_byte('\r');
			}

			write_byte(static_cast<uint8_t>(c));
		}
	}
}
