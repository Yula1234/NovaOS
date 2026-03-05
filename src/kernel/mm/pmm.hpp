#pragma once

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include "kernel/boot/multiboot2.hpp"

namespace kernel::mm::pmm
{
	enum class PageFlags : uint8_t
	{
		None = 0,
		/* Reserved pages are never handed out (kernel image, firmware regions, MMIO holes). */
		Reserved = 1 << 0,
		Allocated = 1 << 1,
		/* Head of a buddy block; order is stored in metadata::order. */
		BuddyHead = 1 << 2,
		PcpCached = 1 << 3,
	};

	inline PageFlags operator|(PageFlags a, PageFlags b) noexcept
	{
		return static_cast<PageFlags>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
	}

	inline PageFlags operator&(PageFlags a, PageFlags b) noexcept
	{
		return static_cast<PageFlags>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
	}

	inline PageFlags& operator|=(PageFlags& a, PageFlags b) noexcept
	{
		a = a | b;
		return a;
	}

	inline PageFlags& operator&=(PageFlags& a, PageFlags b) noexcept
	{
		a = a & b;
		return a;
	}

	inline PageFlags operator~(PageFlags a) noexcept
	{
		return static_cast<PageFlags>(~static_cast<uint8_t>(a));
	}

	struct alignas(16) PageMetadata
	{
		/* Refcount is used by subsystems that pin pages (page tables, shared mappings, etc.). */
		std::atomic<uint32_t> refcount{0};
		uint8_t order{0};
		std::atomic<uint8_t> flags{static_cast<uint8_t>(PageFlags::None)};
		uint8_t reserved{0};
		uint32_t prev_page{0};
		uint32_t next_page{0};
	};

	static_assert(sizeof(PageMetadata) == 16, "PageMetadata must be exactly 16 bytes");

	PageMetadata* page_metadata_base() noexcept;

	PageMetadata* page_metadata(uint64_t phys_addr) noexcept;

	const PageMetadata* page_metadata_const(uint64_t phys_addr) noexcept;

	struct Stats
	{
		uint64_t total_pages;
		uint64_t free_pages;
		uint64_t alloc_limit_bytes;
	};

	void init(const kernel::boot::multiboot2::Reader& multiboot) noexcept;

	Stats stats() noexcept;

	uint64_t alloc_limit() noexcept;
	/* Hard cap for PMM allocations; used during early boot for low-memory constraints (SMP trampoline). */
	void set_alloc_limit(uint64_t limit_bytes) noexcept;

	uint64_t alloc_page() noexcept;
	uint64_t alloc_page_at(uint64_t phys_addr) noexcept;
	void free_page(uint64_t phys_addr) noexcept;
}
