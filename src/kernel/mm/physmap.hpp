#pragma once

#include <stdint.h>

namespace kernel::mm::physmap
{
	/*
	 * Kernel direct map of physical memory.
	 * All phys addresses that the PMM hands out must be reachable at (base + phys).
	 */
	constexpr uint64_t base = 0xFFFFFF8000000000ull;

	inline void* to_virt(uint64_t phys) noexcept
	{
		return reinterpret_cast<void*>(base + phys);
	}

	inline const void* to_virt_const(uint64_t phys) noexcept
	{
		return reinterpret_cast<const void*>(base + phys);
	}

	inline uint64_t to_phys(const void* virt) noexcept
	{
		/* Caller must pass a pointer that belongs to the physmap window. */
		return reinterpret_cast<uint64_t>(virt) - base;
	}
}
