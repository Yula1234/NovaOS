#pragma once

#include <stdint.h>

namespace kernel::mm::vmalloc
{
	void* vmalloc(uint64_t size, uint64_t align) noexcept;
	void vfree(void* addr, uint64_t size) noexcept;
	void vfree(void* addr) noexcept;
}
