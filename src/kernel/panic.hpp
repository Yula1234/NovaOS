#pragma once

#include <stdint.h>

namespace kernel
{
	/* Logs the message (best-effort) and halts the CPU forever. */
	[[noreturn]] void panic(const char* message) noexcept;
	[[noreturn]] void panic(const char* message, uint64_t value) noexcept;
}
