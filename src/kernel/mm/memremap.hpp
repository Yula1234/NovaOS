#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kernel::mm::memremap
{
	/* Lightweight read-only view of physical memory via physmap; intended for firmware tables. */
	const void* map(uint64_t phys, size_t size) noexcept;
}
