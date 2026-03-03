#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kernel::console::vga
{
	void clear() noexcept;
	void write(const char* s) noexcept;
	void write(const char* s, size_t len) noexcept;
}
