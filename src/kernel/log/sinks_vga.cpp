#include "kernel/log/sinks_vga.hpp"

namespace kernel::log
{
	void VgaSink::write(const char* s, size_t len) noexcept
	{
		/* vga::write is internally synchronized. */
		kernel::console::vga::write(s, len);
	}
}
