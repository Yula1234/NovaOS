#include "kernel/mm/pmm.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/arch/x86_64/cpu.hpp"
#include "kernel/arch/x86_64/cpu_local.hpp"
#include "kernel/arch/x86_64/apic/lapic.hpp"
#include "kernel/log/log.hpp"
#include "kernel/mm/physmap.hpp"
#include "lib/align.hpp"
#include "lib/bit.hpp"
#include "lib/lock.hpp"

extern "C" uint8_t __kernel_phys_start;
extern "C" uint8_t __kernel_phys_end;

namespace
{
	constexpr uint64_t page_size = 4096;
	constexpr uint64_t early_mapped_limit = 0x100000000ull;
	constexpr uint32_t max_apic_id = 256;
	constexpr uint32_t pcp_capacity = 64;

	uint8_t* bitmap = nullptr;
	uint8_t* free_block_order = nullptr;
	uint64_t bitmap_bytes = 0;
	uint64_t free_block_order_bytes = 0;
	uint64_t page_count = 0;
	uint32_t max_order = 0;

	uint64_t free_pages = 0;
	uint64_t alloc_limit_bytes = early_mapped_limit;
	kernel::lib::SpinLock pmm_lock;

	struct FreeNode
	{
		uint64_t next_page;
		uint64_t prev_page;
	};

	constexpr uint64_t null_page = ~0ull;
	uint64_t free_list_head[64]{};
	kernel::lib::SpinLock buddy_lock[64]{};

	struct PerCpuCache
	{
		kernel::lib::SpinLock lock;
		uint32_t count;
		uint64_t pages[pcp_capacity];
	};

	PerCpuCache pcp[max_apic_id]{};

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

	uint32_t current_cpu_index() noexcept
	{
		return kernel::arch::x86_64::cpu_local::cpu_id();
	}

	FreeNode* node(uint64_t page_index) noexcept
	{
		const uint64_t phys = page_index * page_size;
		return static_cast<FreeNode*>(kernel::mm::physmap::to_virt(phys));
	}

	const FreeNode* node_const(uint64_t page_index) noexcept
	{
		const uint64_t phys = page_index * page_size;
		return static_cast<const FreeNode*>(kernel::mm::physmap::to_virt(phys));
	}

	void list_init() noexcept
	{
		for (uint32_t i = 0; i < 64; ++i)
		{
			free_list_head[i] = null_page;
		}
	}

	void list_push(uint32_t order, uint64_t page_index) noexcept
	{
		auto* n = node(page_index);
		n->prev_page = null_page;
		n->next_page = free_list_head[order];
		if (free_list_head[order] != null_page)
		{
			node(free_list_head[order])->prev_page = page_index;
		}
		free_list_head[order] = page_index;
	}

	void list_remove(uint32_t order, uint64_t page_index) noexcept
	{
		auto* n = node(page_index);
		const uint64_t prev = n->prev_page;
		const uint64_t next = n->next_page;

		if (prev != null_page)
		{
			node(prev)->next_page = next;
		}
		else
		{
			free_list_head[order] = next;
		}

		if (next != null_page)
		{
			node(next)->prev_page = prev;
		}

		n->prev_page = null_page;
		n->next_page = null_page;
	}

	uint64_t list_pop(uint32_t order) noexcept
	{
		const uint64_t head = free_list_head[order];
		if (head == null_page)
		{
			return null_page;
		}

		list_remove(order, head);
		return head;
	}

	uint32_t compute_max_order(uint64_t pages) noexcept
	{
		uint32_t order = 0;
		while (order + 1 < 64)
		{
			const uint64_t next = 1ull << (order + 1);
			if (next == 0 || next > pages)
			{
				break;
			}
			++order;
		}
		return order;
	}

	uint32_t block_order_at(uint64_t page_index) noexcept
	{
		return free_block_order[page_index];
	}

	void set_block_order(uint64_t page_index, uint32_t order) noexcept
	{
		free_block_order[page_index] = static_cast<uint8_t>(order);
	}

	void clear_block_order(uint64_t page_index) noexcept
	{
		free_block_order[page_index] = 0xFFu;
	}

	void add_free_block(uint64_t page_index, uint32_t order) noexcept
	{
		set_block_order(page_index, order);
		list_push(order, page_index);
	}

	uint64_t alloc_block(uint32_t order) noexcept
	{
		for (uint32_t o = order; o <= max_order; ++o)
		{
			kernel::lib::IrqLockGuard<kernel::lib::SpinLock> guard(buddy_lock[o]);
			const uint64_t block = list_pop(o);
			if (block == null_page)
			{
				continue;
			}

			clear_block_order(block);

			uint64_t current = block;
			uint32_t current_order = o;
			while (current_order > order)
			{
				--current_order;
				const uint64_t split = current + (1ull << current_order);
				{
					kernel::lib::IrqLockGuard<kernel::lib::SpinLock> split_guard(buddy_lock[current_order]);
					add_free_block(split, current_order);
				}
			}

			return current;
		}

		return null_page;
	}

	bool alloc_block_at(uint64_t target_page) noexcept
	{
		uint32_t found_order = 0;
		bool found = false;
		uint64_t block_start = 0;

		for (uint32_t order = 0; order <= max_order; ++order)
		{
			const uint64_t mask = (1ull << order) - 1;
			const uint64_t start = target_page & ~mask;
			if (start >= page_count)
			{
				continue;
			}

			{
				kernel::lib::IrqLockGuard<kernel::lib::SpinLock> guard(buddy_lock[order]);
				if (block_order_at(start) == order)
				{
					found = true;
					found_order = order;
					block_start = start;
				}
			}
		}

		if (!found)
		{
			return false;
		}

		{
			kernel::lib::IrqLockGuard<kernel::lib::SpinLock> guard(buddy_lock[found_order]);
			if (block_order_at(block_start) != found_order)
			{
				return false;
			}
			list_remove(found_order, block_start);
			clear_block_order(block_start);
		}

		uint64_t current = block_start;
		uint32_t current_order = found_order;
		while (current_order > 0)
		{
			--current_order;
			const uint64_t half = 1ull << current_order;
			const uint64_t right = current + half;
			const bool target_in_right = target_page >= right;
			const uint64_t free_half = target_in_right ? current : right;
			const uint64_t keep_half = target_in_right ? right : current;

			{
				kernel::lib::IrqLockGuard<kernel::lib::SpinLock> guard(buddy_lock[current_order]);
				add_free_block(free_half, current_order);
			}

			current = keep_half;
		}

		return current == target_page;
	}

	void free_block(uint64_t page_index, uint32_t order) noexcept
	{
		uint64_t current = page_index;
		uint32_t current_order = order;

		while (current_order < max_order)
		{
			const uint64_t buddy = current ^ (1ull << current_order);
			if (buddy >= page_count)
			{
				break;
			}

			{
				kernel::lib::IrqLockGuard<kernel::lib::SpinLock> guard(buddy_lock[current_order]);
				if (block_order_at(buddy) != current_order)
				{
					break;
				}
				list_remove(current_order, buddy);
				clear_block_order(buddy);
			}

			current = buddy < current ? buddy : current;
			++current_order;
		}

		{
			kernel::lib::IrqLockGuard<kernel::lib::SpinLock> guard(buddy_lock[current_order]);
			add_free_block(current, current_order);
		}
	}

	void add_free_range(uint64_t start_page, uint64_t len_pages) noexcept
	{
		uint64_t page = start_page;
		uint64_t remaining = len_pages;

		while (remaining > 0)
		{
			uint32_t order = 0;
			while (order < max_order)
			{
				const uint64_t size = 1ull << (order + 1);
				if ((page & (size - 1)) != 0)
				{
					break;
				}
				if (size > remaining)
				{
					break;
				}
				++order;
			}

			{
				kernel::lib::IrqLockGuard<kernel::lib::SpinLock> guard(buddy_lock[order]);
				add_free_block(page, order);
			}

			const uint64_t step = 1ull << order;
			page += step;
			remaining -= step;
		}
	}

	void build_buddy_from_bitmap() noexcept
	{
		for (uint64_t i = 0; i < page_count; ++i)
		{
			free_block_order[i] = 0xFFu;
		}

		list_init();
		max_order = compute_max_order(page_count);
		if (max_order >= 63)
		{
			max_order = 63;
		}

		uint64_t run_start = 0;
		uint64_t run_len = 0;

		for (uint64_t i = 0; i < page_count; ++i)
		{
			const bool is_free = !bitmap_test(i);
			if (is_free)
			{
				if (run_len == 0)
				{
					run_start = i;
				}
				++run_len;
				continue;
			}

			if (run_len != 0)
			{
				add_free_range(run_start, run_len);
				run_len = 0;
			}
		}

		if (run_len != 0)
		{
			add_free_range(run_start, run_len);
		}
	}

	bool pcp_pop(uint64_t& out_phys) noexcept
	{
		const uint32_t cpu = current_cpu_index();
		auto& cache = pcp[cpu];
		kernel::lib::IrqLockGuard<kernel::lib::SpinLock> guard(cache.lock);
		if (cache.count == 0)
		{
			return false;
		}

		out_phys = cache.pages[cache.count - 1];
		--cache.count;
		return true;
	}

	bool pcp_push(uint64_t phys) noexcept
	{
		const uint32_t cpu = current_cpu_index();
		auto& cache = pcp[cpu];
		kernel::lib::IrqLockGuard<kernel::lib::SpinLock> guard(cache.lock);
		if (cache.count >= pcp_capacity)
		{
			return false;
		}
		cache.pages[cache.count] = phys;
		++cache.count;
		return true;
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

	uint64_t place_metadata(const kernel::boot::multiboot2::Reader& multiboot, uint64_t bytes) noexcept
	{
		return place_bitmap(multiboot, bytes);
	}
}

namespace kernel::mm::pmm
{
	void init(const kernel::boot::multiboot2::Reader& multiboot) noexcept
	{
		kernel::lib::IrqLockGuard<kernel::lib::SpinLock> guard(pmm_lock);

		const uint64_t max_addr = find_max_address(multiboot);
		page_count = kernel::lib::align_up(max_addr, page_size) / page_size;

		bitmap_bytes = kernel::lib::align_up((page_count + 7) / 8, page_size);
		free_block_order_bytes = kernel::lib::align_up(page_count, page_size);
		const uint64_t metadata_bytes = bitmap_bytes + free_block_order_bytes;
		const uint64_t metadata_phys = place_metadata(multiboot, metadata_bytes);
		const uint64_t bitmap_phys = metadata_phys;
		const uint64_t free_block_order_phys = metadata_phys + bitmap_bytes;

		if (metadata_phys == 0)
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
			kernel::log::write(" meta_bytes=");
			kernel::log::write_u64_hex(metadata_bytes);
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

			kernel::log::write_line("pmm metadata placement failed");
			for (;;)
			{
				asm volatile("cli");
				asm volatile("hlt");
			}
		}

		bitmap = static_cast<uint8_t*>(kernel::mm::physmap::to_virt(bitmap_phys));
		free_block_order = static_cast<uint8_t*>(kernel::mm::physmap::to_virt(free_block_order_phys));
		for (uint64_t i = 0; i < bitmap_bytes; ++i)
		{
			bitmap[i] = 0xFF;
		}

		for (uint64_t i = 0; i < free_block_order_bytes; ++i)
		{
			free_block_order[i] = 0xFFu;
		}

		free_pages = 0;
		alloc_limit_bytes = early_mapped_limit;
		unreserve_available(multiboot);

		reserve_range(0, page_size);
		reserve_range(kernel_phys_start(), kernel_phys_end());
		reserve_range(metadata_phys, metadata_phys + metadata_bytes);

		build_buddy_from_bitmap();

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
		kernel::log::write(" ordermap=");
		kernel::log::write_u64_hex(free_block_order_phys);
		kernel::log::write(" ordermap_bytes=");
		kernel::log::write_u64_dec(free_block_order_bytes);
		kernel::log::write("\n", 1);
	}

	Stats stats() noexcept
	{
		kernel::lib::IrqLockGuard<kernel::lib::SpinLock> guard(pmm_lock);

		return Stats{
			.total_pages = page_count,
			.free_pages = free_pages,
			.alloc_limit_bytes = alloc_limit_bytes,
		};
	}

	uint64_t alloc_limit() noexcept
	{
		kernel::lib::IrqLockGuard<kernel::lib::SpinLock> guard(pmm_lock);
		return alloc_limit_bytes;
	}

	void set_alloc_limit(uint64_t limit_bytes) noexcept
	{
		kernel::lib::IrqLockGuard<kernel::lib::SpinLock> guard(pmm_lock);
		alloc_limit_bytes = limit_bytes;
	}

	uint64_t alloc_page() noexcept
	{
		uint64_t cached = 0;
		if (pcp_pop(cached))
		{
			kernel::lib::IrqLockGuard<kernel::lib::SpinLock> guard(pmm_lock);
			const uint64_t index = cached / page_size;
			if (index < page_count && !bitmap_test(index) && cached < alloc_limit_bytes)
			{
				bitmap_set(index);
				if (free_pages > 0)
				{
					--free_pages;
				}
				return cached;
			}
		}

		kernel::lib::IrqLockGuard<kernel::lib::SpinLock> guard(pmm_lock);
		const uint64_t limit_pages = alloc_limit_bytes / page_size;
		const uint64_t search_pages = limit_pages < page_count ? limit_pages : page_count;
		if (search_pages == 0)
		{
			return 0;
		}

		if (alloc_limit_bytes == ~0ull)
		{
			const uint64_t block = alloc_block(0);
			if (block == null_page)
			{
				return 0;
			}

			bitmap_set(block);
			if (free_pages > 0)
			{
				--free_pages;
			}
			return block * page_size;
		}

		for (uint32_t order = 0; order <= max_order; ++order)
		{
			kernel::lib::IrqLockGuard<kernel::lib::SpinLock> order_guard(buddy_lock[order]);
			uint64_t it = free_list_head[order];
			while (it != null_page)
			{
				const uint64_t block_pages = 1ull << order;
				if (it + block_pages <= search_pages)
				{
					list_remove(order, it);
					clear_block_order(it);

					uint64_t current = it;
					uint32_t current_order = order;
					while (current_order > 0)
					{
						--current_order;
						const uint64_t split = current + (1ull << current_order);
						{
							kernel::lib::IrqLockGuard<kernel::lib::SpinLock> split_guard(buddy_lock[current_order]);
							add_free_block(split, current_order);
						}
					}

					bitmap_set(current);
					if (free_pages > 0)
					{
						--free_pages;
					}
					return current * page_size;
				}

				it = node_const(it)->next_page;
			}
		}

		return 0;
	}

	uint64_t alloc_page_at(uint64_t phys_addr) noexcept
	{
		kernel::lib::IrqLockGuard<kernel::lib::SpinLock> guard(pmm_lock);

		if ((phys_addr % page_size) != 0)
		{
			return 0;
		}

		if (phys_addr >= alloc_limit_bytes)
		{
			return 0;
		}

		const uint64_t index = phys_addr / page_size;
		if (index >= page_count)
		{
			return 0;
		}

		if (bitmap_test(index))
		{
			return 0;
		}

		if (!alloc_block_at(index))
		{
			return 0;
		}

		bitmap_set(index);
		if (free_pages > 0)
		{
			--free_pages;
		}

		return phys_addr;
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

		{
			kernel::lib::IrqLockGuard<kernel::lib::SpinLock> guard(pmm_lock);
			if (!bitmap_test(index))
			{
				return;
			}

			bitmap_clear(index);
			++free_pages;
		}

		if (pcp_push(phys_addr))
		{
			return;
		}

		kernel::lib::IrqLockGuard<kernel::lib::SpinLock> guard(pmm_lock);
		free_block(index, 0);
	}
}
