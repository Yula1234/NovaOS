#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kernel::console::vga
{
	/* VGA text-mode console (0xB8000). Intended mainly for early boot and low-level diagnostics. */
	void clear() noexcept;
	void write(const char* s) noexcept;
	void write(const char* s, size_t len) noexcept;
}
