#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kernel::serial
{
	class Com1
	{
	public:
		static constexpr uint16_t base_port = 0x3F8;

		bool init() noexcept;
		void write_byte(uint8_t value) noexcept;
		void write(const char* s) noexcept;
		void write(const char* s, size_t len) noexcept;

	private:
		bool is_transmit_empty() const noexcept;
	};
}
