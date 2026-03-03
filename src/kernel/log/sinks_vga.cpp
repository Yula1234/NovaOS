#include "kernel/log/sinks_vga.hpp"

namespace kernel::log
{
	void VgaSink::write(const char* s, size_t len) noexcept
	{
		kernel::console::vga::write(s, len);
	}
}
