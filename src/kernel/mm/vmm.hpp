#pragma once

#include <stdint.h>

#include "kernel/boot/multiboot2.hpp"
#include "lib/lock.hpp"

namespace kernel::mm::vmm
{
	enum class PageFlags : uint64_t
	{
		Present = 1ull << 0,
		Writable = 1ull << 1,
		User = 1ull << 2,
		WriteThrough = 1ull << 3,
		CacheDisable = 1ull << 4,
		NoExecute = 1ull << 63,
	};

	inline constexpr PageFlags operator|(PageFlags a, PageFlags b) noexcept
	{
		return static_cast<PageFlags>(static_cast<uint64_t>(a) | static_cast<uint64_t>(b));
	}

	inline constexpr PageFlags operator&(PageFlags a, PageFlags b) noexcept
	{
		return static_cast<PageFlags>(static_cast<uint64_t>(a) & static_cast<uint64_t>(b));
	}

	class AddressSpace
	{
	public:
		AddressSpace() noexcept = default;

		/* pml4_phys must point to a 4KiB PML4 page in physical memory. */
		void reset(uint64_t pml4_phys) noexcept;

		uint64_t pml4_phys() const noexcept;

		bool map_page(uint64_t virt, uint64_t phys, PageFlags flags) noexcept;
		bool unmap_page(uint64_t virt) noexcept;

		uint64_t translate(uint64_t virt) const noexcept;

		void activate() const noexcept;

	private:
		/* Protected by lock_; map/unmap may be called concurrently on SMP. */
		uint64_t pml4_phys_ = 0;
		mutable kernel::lib::McsLock lock_;
	};

	void init() noexcept;
	AddressSpace& kernel_space() noexcept;

	/* Maps a contiguous phys range; on failure the function rolls back partial mappings. */
	bool map_range(uint64_t virt, uint64_t phys, uint64_t size, PageFlags flags) noexcept;
	void map_physmap_all(const kernel::boot::multiboot2::Reader& multiboot) noexcept;
}
