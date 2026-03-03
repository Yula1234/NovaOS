#include "kernel/panic.hpp"

#include "kernel/log/log.hpp"

namespace
{
	[[noreturn]] void halt() noexcept
	{
		for (;;)
		{
			asm volatile("cli");
			asm volatile("hlt");
		}
	}
}

namespace kernel
{
	[[noreturn]] void panic(const char* message) noexcept
	{
		kernel::log::write("panic: ");
		kernel::log::write_line(message);

		halt();
	}

	[[noreturn]] void panic(const char* message, uint64_t value) noexcept
	{
		kernel::log::write("panic: ");
		kernel::log::write(message);
		kernel::log::write(" ");
		kernel::log::write_u64_hex(value);
		kernel::log::write("\n", 1);

		halt();
	}
}
