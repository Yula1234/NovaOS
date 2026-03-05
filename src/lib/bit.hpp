#pragma once

#include <stdint.h>

namespace kernel::lib
{
	constexpr uint8_t popcount_u8(uint8_t v) noexcept
	{
		/* SWAR popcount for 8-bit values; constant-time and branch-free. */
		v = static_cast<uint8_t>(v - ((v >> 1) & 0x55u));
		v = static_cast<uint8_t>((v & 0x33u) + ((v >> 2) & 0x33u));
		return static_cast<uint8_t>(((v + (v >> 4)) & 0x0Fu));
	}
}
