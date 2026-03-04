#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/boot/multiboot2.hpp"

namespace kernel::mm::pmm
{
	struct Stats
	{
		uint64_t total_pages;
		uint64_t free_pages;
		uint64_t alloc_limit_bytes;
	};

	void init(const kernel::boot::multiboot2::Reader& multiboot) noexcept;

	Stats stats() noexcept;

	uint64_t alloc_limit() noexcept;
	void set_alloc_limit(uint64_t limit_bytes) noexcept;

	uint64_t alloc_page() noexcept;
	uint64_t alloc_page_at(uint64_t phys_addr) noexcept;
	void free_page(uint64_t phys_addr) noexcept;
}
