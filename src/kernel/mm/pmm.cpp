#include "kernel/mm/pmm.hpp"

#include <stddef.h>
#include <stdint.h>

#include "lib/align.hpp"
#include "lib/bit.hpp"
#include "kernel/log/log.hpp"
#include "kernel/mm/physmap.hpp"

extern "C" uint8_t __kernel_phys_start;
extern "C" uint8_t __kernel_phys_end;

namespace
{
	constexpr uint64_t page_size = 4096;
	constexpr uint64_t early_mapped_limit = 0x100000000ull;

	uint8_t* bitmap = nullptr;
	uint64_t bitmap_bytes = 0;
	uint64_t page_count = 0;

	uint64_t free_pages = 0;
	uint64_t alloc_limit_bytes = early_mapped_limit;

	void bitmap_set(uint64_t page_index) noexcept
	{
		bitmap[page_index / 8] |= static_cast<uint8_t>(1u << (page_index % 8));
	}

	void bitmap_clear(uint64_t page_index) noexcept
	{
		bitmap[page_index / 8] &= static_cast<uint8_t>(~(1u << (page_index % 8)));
	}

	bool bitmap_test(uint64_t page_index) noexcept
	{
		return (bitmap[page_index / 8] & static_cast<uint8_t>(1u << (page_index % 8))) != 0;
	}

	void bitmap_clear_range(uint64_t start_page, uint64_t page_len) noexcept
	{
		if (page_len == 0)
		{
			return;
		}

		const uint64_t end_page = start_page + page_len;
		uint64_t page = start_page;

		while (page < end_page && (page % 8) != 0)
		{
			if (bitmap_test(page))
			{
				bitmap_clear(page);
				++free_pages;
			}
			++page;
		}

		while (page + 8 <= end_page)
		{
			const uint64_t byte_index = page / 8;
			if (bitmap[byte_index] != 0)
			{
				free_pages += static_cast<uint64_t>(kernel::lib::popcount_u8(bitmap[byte_index]));
				bitmap[byte_index] = 0;
			}
			page += 8;
		}

		while (page < end_page)
		{
			if (bitmap_test(page))
			{
				bitmap_clear(page);
				++free_pages;
			}
			++page;
		}
	}

	void reserve_range(uint64_t start, uint64_t end) noexcept
	{
		const uint64_t s = kernel::lib::align_down(start, page_size);
		const uint64_t e = kernel::lib::align_up(end, page_size);

		for (uint64_t addr = s; addr < e; addr += page_size)
		{
			const uint64_t index = addr / page_size;
			if (index >= page_count)
			{
				break;
			}

			if (!bitmap_test(index))
			{
				bitmap_set(index);
				if (free_pages > 0)
				{
					--free_pages;
				}
			}
		}
	}

	void unreserve_available(const kernel::boot::multiboot2::Reader& multiboot) noexcept
	{
		const auto count = multiboot.memory_map_entry_count();
		const auto* entries = multiboot.memory_map_entries();
		const auto entry_size = multiboot.memory_map_entry_size();

		for (size_t i = 0; i < count; ++i)
		{
			const auto* e = reinterpret_cast<const kernel::boot::multiboot2::MemoryMapEntry*>(
				reinterpret_cast<const uint8_t*>(entries) + i * entry_size
			);

			if (e->type != static_cast<uint32_t>(kernel::boot::multiboot2::MemoryType::Available))
			{
				continue;
			}

			const uint64_t start = kernel::lib::align_up(e->addr, page_size);
			const uint64_t end = kernel::lib::align_down(e->addr + e->len, page_size);
			if (end <= start)
			{
				continue;
			}

			const uint64_t start_page = start / page_size;
			uint64_t end_page = end / page_size;
			if (end_page > page_count)
			{
				end_page = page_count;
			}

			if (end_page <= start_page)
			{
				continue;
			}

			bitmap_clear_range(start_page, end_page - start_page);
		}
	}

	uint64_t find_max_address(const kernel::boot::multiboot2::Reader& multiboot) noexcept
	{
		uint64_t max = 0;

		const auto count = multiboot.memory_map_entry_count();
		const auto* entries = multiboot.memory_map_entries();
		const auto entry_size = multiboot.memory_map_entry_size();

		for (size_t i = 0; i < count; ++i)
		{
			const auto* e = reinterpret_cast<const kernel::boot::multiboot2::MemoryMapEntry*>(
				reinterpret_cast<const uint8_t*>(entries) + i * entry_size
			);

			const uint64_t end = e->addr + e->len;
			if (end > max)
			{
				max = end;
			}
		}

		return max;
	}

	uint64_t kernel_phys_start() noexcept
	{
		return reinterpret_cast<uint64_t>(&:: __kernel_phys_start);
	}

	uint64_t kernel_phys_end() noexcept
	{
		return reinterpret_cast<uint64_t>(&:: __kernel_phys_end);
	}

	uint64_t place_bitmap(const kernel::boot::multiboot2::Reader& multiboot, uint64_t bytes) noexcept
	{
		const uint64_t start_hint = kernel::lib::align_up(kernel_phys_end(), page_size);
		const auto count = multiboot.memory_map_entry_count();
		const auto* entries = multiboot.memory_map_entries();
		const auto entry_size = multiboot.memory_map_entry_size();
		const uint64_t size = kernel::lib::align_up(bytes, page_size);

		if (start_hint + size <= early_mapped_limit)
		{
			for (size_t i = 0; i < count; ++i)
			{
				const auto* e = reinterpret_cast<const kernel::boot::multiboot2::MemoryMapEntry*>(
					reinterpret_cast<const uint8_t*>(entries) + i * entry_size
				);

				if (e->type != static_cast<uint32_t>(kernel::boot::multiboot2::MemoryType::Available))
				{
					continue;
				}

				const uint64_t region_start = kernel::lib::align_up(e->addr, page_size);
				const uint64_t region_end = e->addr + e->len;
				const uint64_t capped_region_end = region_end > early_mapped_limit ? early_mapped_limit : region_end;

				if (start_hint >= region_start && start_hint + size <= capped_region_end)
				{
					return start_hint;
				}
			}
		}

		for (size_t i = 0; i < count; ++i)
		{
			const auto* e = reinterpret_cast<const kernel::boot::multiboot2::MemoryMapEntry*>(
				reinterpret_cast<const uint8_t*>(entries) + i * entry_size
			);

			if (e->type != static_cast<uint32_t>(kernel::boot::multiboot2::MemoryType::Available))
			{
				continue;
			}

			uint64_t candidate = kernel::lib::align_up(e->addr, page_size);
			const uint64_t region_end = e->addr + e->len;

			if (candidate >= early_mapped_limit)
			{
				continue;
			}

			const uint64_t capped_region_end = region_end > early_mapped_limit ? early_mapped_limit : region_end;

			if (candidate < start_hint && start_hint < capped_region_end)
			{
				candidate = start_hint;
			}

			candidate = kernel::lib::align_up(candidate, page_size);
			const uint64_t needed_end = candidate + size;

			if (needed_end <= capped_region_end)
			{
				return candidate;
			}
		}

		return 0;
	}
}

namespace kernel::mm::pmm
{
	void init(const kernel::boot::multiboot2::Reader& multiboot) noexcept
	{
		const uint64_t max_addr = find_max_address(multiboot);
		page_count = kernel::lib::align_up(max_addr, page_size) / page_size;

		bitmap_bytes = kernel::lib::align_up((page_count + 7) / 8, page_size);
		const uint64_t bitmap_phys = place_bitmap(multiboot, bitmap_bytes);
		if (bitmap_phys == 0)
		{
			kernel::log::write("pmm kernel_phys_start=");
			kernel::log::write_u64_hex(kernel_phys_start());
			kernel::log::write(" kernel_phys_end=");
			kernel::log::write_u64_hex(kernel_phys_end());
			kernel::log::write("\n", 1);

			kernel::log::write("pmm start_hint=");
			kernel::log::write_u64_hex(kernel::lib::align_up(kernel_phys_end(), page_size));
			kernel::log::write(" bitmap_bytes=");
			kernel::log::write_u64_hex(bitmap_bytes);
			kernel::log::write("\n", 1);

			const auto count = multiboot.memory_map_entry_count();
			const auto* entries = multiboot.memory_map_entries();
			const auto entry_size = multiboot.memory_map_entry_size();

			for (size_t i = 0; i < count; ++i)
			{
				const auto* e = reinterpret_cast<const kernel::boot::multiboot2::MemoryMapEntry*>(
					reinterpret_cast<const uint8_t*>(entries) + i * entry_size
				);

				if (e->type != static_cast<uint32_t>(kernel::boot::multiboot2::MemoryType::Available))
				{
					continue;
				}

				kernel::log::write("pmm avail addr=");
				kernel::log::write_u64_hex(e->addr);
				kernel::log::write(" end=");
				kernel::log::write_u64_hex(e->addr + e->len);
				kernel::log::write("\n", 1);
			}

			kernel::log::write_line("pmm bitmap placement failed");
			for (;;)
			{
				asm volatile("cli");
				asm volatile("hlt");
			}
		}

		bitmap = static_cast<uint8_t*>(kernel::mm::physmap::to_virt(bitmap_phys));
		for (uint64_t i = 0; i < bitmap_bytes; ++i)
		{
			bitmap[i] = 0xFF;
		}

		free_pages = 0;
		alloc_limit_bytes = early_mapped_limit;
		unreserve_available(multiboot);

		reserve_range(0, page_size);
		reserve_range(kernel_phys_start(), kernel_phys_end());
		reserve_range(bitmap_phys, bitmap_phys + bitmap_bytes);

		kernel::log::write("pmm pages total=");
		kernel::log::write_u64_dec(page_count);
		kernel::log::write(" free=");
		kernel::log::write_u64_dec(free_pages);
		kernel::log::write(" limit=");
		kernel::log::write_u64_hex(alloc_limit_bytes);
		kernel::log::write(" bitmap=");
		kernel::log::write_u64_hex(bitmap_phys);
		kernel::log::write(" bytes=");
		kernel::log::write_u64_dec(bitmap_bytes);
		kernel::log::write("\n", 1);
	}

	Stats stats() noexcept
	{
		return Stats{
			.total_pages = page_count,
			.free_pages = free_pages,
			.alloc_limit_bytes = alloc_limit_bytes,
		};
	}

	uint64_t alloc_limit() noexcept
	{
		return alloc_limit_bytes;
	}

	void set_alloc_limit(uint64_t limit_bytes) noexcept
	{
		alloc_limit_bytes = limit_bytes;
	}

	uint64_t alloc_page() noexcept
	{
		const uint64_t limit_pages = alloc_limit_bytes / page_size;
		const uint64_t search_pages = limit_pages < page_count ? limit_pages : page_count;

		for (uint64_t i = 0; i < search_pages; ++i)
		{
			if (!bitmap_test(i))
			{
				bitmap_set(i);
				if (free_pages > 0)
				{
					--free_pages;
				}

				return i * page_size;
			}
		}

		return 0;
	}

	void free_page(uint64_t phys_addr) noexcept
	{
		if ((phys_addr % page_size) != 0)
		{
			return;
		}

		const uint64_t index = phys_addr / page_size;
		if (index >= page_count)
		{
			return;
		}

		if (bitmap_test(index))
		{
			bitmap_clear(index);
			++free_pages;
		}
	}
}
