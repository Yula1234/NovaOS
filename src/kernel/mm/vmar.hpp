#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kernel::mm::vmar
{
	void init() noexcept;

	enum class Arena : uint8_t
	{
		Mmio,
		Vmalloc,
	};

	void* alloc(Arena arena, uint64_t size, uint64_t align) noexcept;
	/* free requires the original base+size returned to alloc/reserve_fixed. */
	bool free(Arena arena, void* addr, uint64_t size) noexcept;

	bool reserve_fixed(Arena arena, void* addr, uint64_t size) noexcept;
	/* lookup succeeds if addr lies within a used range and returns the recorded base+size. */
	bool lookup(Arena arena, void* addr, void*& out_base, uint64_t& out_size) noexcept;

	void* ioremap_alloc(uint64_t size, uint64_t align) noexcept;
	void ioremap_free(void* addr, uint64_t size) noexcept;
}
