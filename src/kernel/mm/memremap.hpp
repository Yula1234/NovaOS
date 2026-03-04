#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kernel::mm::memremap
{
	const void* map(uint64_t phys, size_t size) noexcept;
}
