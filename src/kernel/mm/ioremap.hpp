#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kernel::mm::ioremap
{
	void init() noexcept;

	void* map(uint64_t phys, size_t size) noexcept;
}
