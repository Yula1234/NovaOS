#pragma once

#include <stdint.h>

namespace kernel
{
	[[noreturn]] void panic(const char* message) noexcept;
	[[noreturn]] void panic(const char* message, uint64_t value) noexcept;
}
