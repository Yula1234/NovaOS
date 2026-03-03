#pragma once

#include <stddef.h>

namespace kernel::mm::heap
{
	void init() noexcept;

	void* alloc(size_t size) noexcept;
	void free(void* ptr) noexcept;
}
