#pragma once

#include <stdint.h>

namespace kernel::arch::x86_64::idt
{
	void init() noexcept;
	void reload() noexcept;
}
