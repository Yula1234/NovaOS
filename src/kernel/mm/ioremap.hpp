#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kernel::mm::ioremap
{
	void init() noexcept;

	void* map(uint64_t phys, size_t size) noexcept;
	void unmap(void* addr, size_t size) noexcept;
}
