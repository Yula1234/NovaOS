#pragma once

#include <stdint.h>

namespace kernel::arch::x86_64::idt
{
	/* init builds the table and loads IDTR; reload only re-loads the cached IDTR. */
	void init() noexcept;
	void reload() noexcept;
}
