#pragma once

#include <stddef.h>

namespace kernel::mm::heap
{
	void init() noexcept;

	/* General-purpose kernel heap; small allocations come from slabs, large ones may use vmalloc. */
	void* alloc(size_t size) noexcept;
	void free(void* ptr) noexcept;
}
