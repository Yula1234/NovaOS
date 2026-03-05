#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kernel::serial
{
	class Com1
	{
	public:
		/* Standard legacy COM1 base I/O port. */
		static constexpr uint16_t base_port = 0x3F8;

		/* Programs the UART for basic polled output; returns false if loopback test fails. */
		bool init() noexcept;
		void write_byte(uint8_t value) noexcept;
		/* Writes a NUL-terminated string; '\n' is translated to CRLF for terminals. */
		void write(const char* s) noexcept;
		void write(const char* s, size_t len) noexcept;

	private:
		bool is_transmit_empty() const noexcept;
	};
}
