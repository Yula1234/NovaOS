#pragma once

#include <stdint.h>

#include <type_traits>

namespace kernel::lib
{
	template<typename T>
	constexpr T align_down(T value, T alignment) noexcept
	{
		static_assert(std::is_integral_v<T>);
		static_assert(std::is_unsigned_v<T>);

		return value & static_cast<T>(~static_cast<T>(alignment - 1));
	}

	template<typename T>
	constexpr T align_up(T value, T alignment) noexcept
	{
		static_assert(std::is_integral_v<T>);
		static_assert(std::is_unsigned_v<T>);

		return align_down(static_cast<T>(value + static_cast<T>(alignment - 1)), alignment);
	}
}
