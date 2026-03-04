#include "kernel/mm/pmm.hpp"

#include <atomic>
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
	constexpr uint32_t null_page_index = ~0u;
	constexpr uint64_t dma_limit_bytes = 16ull * 1024 * 1024;
	constexpr uint32_t normal_zone_shards = 4;
	constexpr uint32_t zone_count = 1 + normal_zone_shards;

	kernel::mm::pmm::PageMetadata* page_meta = nullptr;
	uint64_t page_count = 0;
	uint32_t max_order = 0;

	std::atomic<uint64_t> alloc_limit_bytes{early_mapped_limit};
	kernel::lib::McsLock pmm_lock;

	struct Zone
	{
		uint32_t start_page = 0;
		uint32_t end_page = 0;

		uint32_t free_list_head[64]{};
		kernel::lib::McsLock buddy_lock[64]{};
		std::atomic<uint64_t> free_pages{0};
	};

	Zone zones[zone_count]{};

	struct alignas(64) PerCpuCache
	{
		std::atomic<uint32_t> count{0};
		uint64_t pages[pcp_capacity];
	};

	PerCpuCache pcp[max_apic_id]{};

	uint32_t current_cpu_index() noexcept
	{
		return kernel::arch::x86_64::cpu_local::cpu_id();
	}

	void zone_list_init(Zone& z) noexcept
	{
		for (uint32_t i = 0; i < 64; ++i)
		{
			z.free_list_head[i] = null_page_index;
		}
	}

	void list_push_meta(Zone& z, uint32_t order, uint32_t page_index) noexcept
	{
		auto* meta = &page_meta[page_index];
		meta->prev_page = null_page_index;
		meta->next_page = z.free_list_head[order];

		if (z.free_list_head[order] != null_page_index)
		{
			page_meta[z.free_list_head[order]].prev_page = page_index;
		}

		z.free_list_head[order] = page_index;
	}

	void list_remove_meta(Zone& z, uint32_t order, uint32_t page_index) noexcept
	{
		auto* meta = &page_meta[page_index];
		const uint32_t prev = meta->prev_page;
		const uint32_t next = meta->next_page;

		if (prev != null_page_index)
		{
			page_meta[prev].next_page = next;
		}
		else
		{
			z.free_list_head[order] = next;
		}

		if (next != null_page_index)
		{
			page_meta[next].prev_page = prev;
		}

		meta->prev_page = null_page_index;
		meta->next_page = null_page_index;
	}

	uint32_t list_pop_meta(Zone& z, uint32_t order) noexcept
	{
		const uint32_t head = z.free_list_head[order];
		if (head == null_page_index)
		{
			return null_page_index;
		}

		list_remove_meta(z, order, head);
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

	bool is_allocated(uint32_t page_index) noexcept
	{
		const uint8_t v = page_meta[page_index].flags.load(std::memory_order_relaxed);
		return (v & static_cast<uint8_t>(kernel::mm::pmm::PageFlags::Allocated)) != 0;
	}

	void set_allocated(uint32_t page_index) noexcept
	{
		page_meta[page_index].flags.fetch_or(static_cast<uint8_t>(kernel::mm::pmm::PageFlags::Allocated), std::memory_order_relaxed);
	}

	void clear_allocated(uint32_t page_index) noexcept
	{
		page_meta[page_index].flags.fetch_and(static_cast<uint8_t>(~kernel::mm::pmm::PageFlags::Allocated), std::memory_order_relaxed);
	}

	bool is_reserved(uint32_t page_index) noexcept
	{
		const uint8_t v = page_meta[page_index].flags.load(std::memory_order_relaxed);
		return (v & static_cast<uint8_t>(kernel::mm::pmm::PageFlags::Reserved)) != 0;
	}

	void set_reserved(uint32_t page_index) noexcept
	{
		page_meta[page_index].flags.fetch_or(static_cast<uint8_t>(kernel::mm::pmm::PageFlags::Reserved), std::memory_order_relaxed);
	}

	void clear_reserved(uint32_t page_index) noexcept
	{
		page_meta[page_index].flags.fetch_and(static_cast<uint8_t>(~kernel::mm::pmm::PageFlags::Reserved), std::memory_order_relaxed);
	}

	bool try_clear_allocated(uint32_t page_index) noexcept
	{
		uint8_t expected = page_meta[page_index].flags.load(std::memory_order_relaxed);
		for (;;)
		{
			if ((expected & static_cast<uint8_t>(kernel::mm::pmm::PageFlags::Allocated)) == 0)
			{
				return false;
			}

			const uint8_t desired = static_cast<uint8_t>(expected & static_cast<uint8_t>(~kernel::mm::pmm::PageFlags::Allocated));
			if (page_meta[page_index].flags.compare_exchange_weak(expected, desired, std::memory_order_relaxed, std::memory_order_relaxed))
			{
				return true;
			}
		}
	}

	uint8_t get_order_meta(uint32_t page_index) noexcept
	{
		return page_meta[page_index].order;
	}

	void set_order_meta(uint32_t page_index, uint8_t order) noexcept
	{
		page_meta[page_index].order = order;
	}

	void clear_order_meta(uint32_t page_index) noexcept
	{
		page_meta[page_index].order = 0xFF;
	}

	void add_free_block(Zone& z, uint32_t page_index, uint32_t order) noexcept
	{
		set_order_meta(page_index, static_cast<uint8_t>(order));
		list_push_meta(z, order, page_index);
		z.free_pages.fetch_add(1ull << order, std::memory_order_relaxed);
	}

	uint32_t alloc_block(Zone& z, uint32_t order) noexcept
	{
		for (uint32_t o = order; o <= max_order; ++o)
		{
			kernel::lib::IrqMcsLockGuard guard(z.buddy_lock[o]);
			const uint32_t block = list_pop_meta(z, o);
			if (block == null_page_index)
			{
				continue;
			}

			z.free_pages.fetch_sub(1ull << o, std::memory_order_relaxed);

			clear_order_meta(block);

			uint32_t current = block;
			uint32_t current_order = o;
			while (current_order > order)
			{
				--current_order;
				const uint32_t split = current + (1u << current_order);
				{
					kernel::lib::IrqMcsLockGuard split_guard(z.buddy_lock[current_order]);
					add_free_block(z, split, current_order);
				}
			}

			return current;
		}

		return null_page_index;
	}

	bool alloc_block_at(Zone& z, uint32_t target_page) noexcept
	{
		uint32_t found_order = 0;
		bool found = false;
		uint32_t block_start = 0;

		for (uint32_t order = 0; order <= max_order; ++order)
		{
			const uint32_t mask = (1u << order) - 1;
			const uint32_t start = target_page & ~mask;
			if (start >= page_count)
			{
				continue;
			}

			{
				kernel::lib::IrqMcsLockGuard guard(z.buddy_lock[order]);
				if (get_order_meta(start) == order)
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
			kernel::lib::IrqMcsLockGuard guard(z.buddy_lock[found_order]);
			if (get_order_meta(block_start) != found_order)
			{
				return false;
			}
			list_remove_meta(z, found_order, block_start);
			z.free_pages.fetch_sub(1ull << found_order, std::memory_order_relaxed);
			clear_order_meta(block_start);
		}

		uint32_t current = block_start;
		uint32_t current_order = found_order;
		while (current_order > 0)
		{
			--current_order;
			const uint32_t half = 1u << current_order;
			const uint32_t right = current + half;
			const bool target_in_right = target_page >= right;
			const uint32_t free_half = target_in_right ? current : right;
			const uint32_t keep_half = target_in_right ? right : current;

			{
				kernel::lib::IrqMcsLockGuard guard(z.buddy_lock[current_order]);
				add_free_block(z, free_half, current_order);
			}

			current = keep_half;
		}

		return current == target_page;
	}

	void free_block(Zone& z, uint32_t page_index, uint32_t order) noexcept
	{
		uint32_t current = page_index;
		uint32_t current_order = order;

		while (current_order < max_order)
		{
			const uint32_t buddy = current ^ (1u << current_order);
			if (buddy >= page_count)
			{
				break;
			}

			{
				kernel::lib::IrqMcsLockGuard guard(z.buddy_lock[current_order]);
				if (get_order_meta(buddy) != current_order)
				{
					break;
				}
				list_remove_meta(z, current_order, buddy);
				z.free_pages.fetch_sub(1ull << current_order, std::memory_order_relaxed);
				clear_order_meta(buddy);
			}

			current = buddy < current ? buddy : current;
			++current_order;
		}

		{
			kernel::lib::IrqMcsLockGuard guard(z.buddy_lock[current_order]);
			add_free_block(z, current, current_order);
		}
	}

	void add_free_range(Zone& z, uint32_t start_page, uint32_t len_pages) noexcept
	{
		uint32_t page = start_page;
		uint32_t remaining = len_pages;

		while (remaining > 0)
		{
			uint32_t order = 0;
			while (order < max_order)
			{
				const uint32_t size = 1u << (order + 1);
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
				kernel::lib::IrqMcsLockGuard guard(z.buddy_lock[order]);
				add_free_block(z, page, order);
			}

			const uint32_t step = 1u << order;
			page += step;
			remaining -= step;
		}
	}

	bool zone_contains(const Zone& z, uint32_t page) noexcept
	{
		return page >= z.start_page && page < z.end_page;
	}

	Zone& pick_zone_for_alloc(bool allow_dma) noexcept
	{
		const uint32_t cpu = current_cpu_index();
		const uint32_t shard = normal_zone_shards != 0 ? (cpu % normal_zone_shards) : 0;
		const uint32_t normal_index = 1 + shard;

		if (normal_index < zone_count && zones[normal_index].start_page < zones[normal_index].end_page)
		{
			return zones[normal_index];
		}

		if (allow_dma)
		{
			return zones[0];
		}

		return zones[0];
	}

	void build_zone_buddy_from_metadata(Zone& z) noexcept
	{
		zone_list_init(z);
		z.free_pages.store(0, std::memory_order_relaxed);

		uint32_t run_start = 0;
		uint32_t run_len = 0;

		for (uint32_t i = z.start_page; i < z.end_page; ++i)
		{
			const bool is_free = !is_reserved(i) && !is_allocated(i);
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
				add_free_range(z, run_start, run_len);
				run_len = 0;
			}
		}

		if (run_len != 0)
		{
			add_free_range(z, run_start, run_len);
		}
	}

	Zone& pick_zone_for_page(uint32_t page) noexcept
	{
		for (uint32_t i = 0; i < zone_count; ++i)
		{
			if (zone_contains(zones[i], page))
			{
				return zones[i];
			}
		}

		return zones[0];
	}

	uint64_t total_free_pages() noexcept
	{
		uint64_t total = 0;
		for (uint32_t i = 0; i < zone_count; ++i)
		{
			total += zones[i].free_pages.load(std::memory_order_relaxed);
		}

		for (uint32_t cpu = 0; cpu < max_apic_id; ++cpu)
		{
			total += static_cast<uint64_t>(pcp[cpu].count.load(std::memory_order_relaxed));
		}

		return total;
	}

	bool pcp_pop(uint64_t& out_phys) noexcept
	{
		const uint32_t cpu = current_cpu_index();
		auto& cache = pcp[cpu];

		const uint64_t rflags = kernel::lib::irq_save_disable();

		uint32_t count = cache.count.load(std::memory_order_relaxed);
		if (count == 0)
		{
			kernel::lib::irq_restore(rflags);
			return false;
		}

		--count;
		out_phys = cache.pages[count];
		cache.count.store(count, std::memory_order_relaxed);

		kernel::lib::irq_restore(rflags);
		return true;
	}

	bool pcp_push(uint64_t phys) noexcept
	{
		const uint32_t cpu = current_cpu_index();
		auto& cache = pcp[cpu];

		const uint64_t rflags = kernel::lib::irq_save_disable();

		uint32_t count = cache.count.load(std::memory_order_relaxed);
		if (count >= pcp_capacity)
		{
			kernel::lib::irq_restore(rflags);
			return false;
		}

		cache.pages[count] = phys;
		++count;
		cache.count.store(count, std::memory_order_relaxed);

		kernel::lib::irq_restore(rflags);
		return true;
	}

	void mark_available_range(uint32_t start_page, uint32_t len_pages) noexcept
	{
		for (uint32_t i = start_page; i < start_page + len_pages && i < page_count; ++i)
		{
			clear_reserved(i);
			clear_allocated(i);
			clear_order_meta(i);
		}
	}

	void mark_reserved_range(uint64_t start, uint64_t end) noexcept
	{
		const uint64_t s = kernel::lib::align_down(start, page_size);
		const uint64_t e = kernel::lib::align_up(end, page_size);

		for (uint64_t addr = s; addr < e; addr += page_size)
		{
			const uint32_t index = static_cast<uint32_t>(addr / page_size);
			if (index >= page_count)
			{
				break;
			}

			if (!is_reserved(index) && !is_allocated(index))
			{
				set_reserved(index);
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

			const uint32_t start_page = static_cast<uint32_t>(start / page_size);
			uint32_t end_page = static_cast<uint32_t>(end / page_size);
			if (end_page > page_count)
			{
				end_page = static_cast<uint32_t>(page_count);
			}

			if (end_page <= start_page)
			{
				continue;
			}

			mark_available_range(start_page, end_page - start_page);
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
		return reinterpret_cast<uint64_t>(&::__kernel_phys_start);
	}

	uint64_t kernel_phys_end() noexcept
	{
		return reinterpret_cast<uint64_t>(&::__kernel_phys_end);
	}

	uint64_t place_metadata(const kernel::boot::multiboot2::Reader& multiboot, uint64_t bytes) noexcept
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
	PageMetadata* page_metadata_base() noexcept
	{
		return page_meta;
	}

	PageMetadata* page_metadata(uint64_t phys_addr) noexcept
	{
		const uint32_t index = static_cast<uint32_t>(phys_addr / page_size);
		if (index >= page_count)
		{
			return nullptr;
		}
		return &page_meta[index];
	}

	const PageMetadata* page_metadata_const(uint64_t phys_addr) noexcept
	{
		const uint32_t index = static_cast<uint32_t>(phys_addr / page_size);
		if (index >= page_count)
		{
			return nullptr;
		}
		return &page_meta[index];
	}

	void init(const kernel::boot::multiboot2::Reader& multiboot) noexcept
	{
		kernel::lib::IrqMcsLockGuard guard(pmm_lock);

		const uint64_t max_addr = find_max_address(multiboot);
		page_count = kernel::lib::align_up(max_addr, page_size) / page_size;
		max_order = compute_max_order(page_count);
		if (max_order >= 63)
		{
			max_order = 63;
		}

		const uint64_t metadata_bytes = page_count * sizeof(PageMetadata);
		const uint64_t metadata_phys = place_metadata(multiboot, metadata_bytes);

		if (metadata_phys == 0)
		{
			kernel::log::write("pmm kernel_phys_start=");
			kernel::log::write_u64_hex(kernel_phys_start());
			kernel::log::write(" kernel_phys_end=");
			kernel::log::write_u64_hex(kernel_phys_end());
			kernel::log::write("\n", 1);

			kernel::log::write("pmm start_hint=");
			kernel::log::write_u64_hex(kernel::lib::align_up(kernel_phys_end(), page_size));
			kernel::log::write(" metadata_bytes=");
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

		page_meta = static_cast<PageMetadata*>(kernel::mm::physmap::to_virt(metadata_phys));

		for (uint64_t i = 0; i < page_count; ++i)
		{
			new (&page_meta[i]) PageMetadata();
			set_reserved(static_cast<uint32_t>(i));
		}

		alloc_limit_bytes.store(early_mapped_limit, std::memory_order_relaxed);
		unreserve_available(multiboot);

		mark_reserved_range(0, page_size);
		mark_reserved_range(kernel_phys_start(), kernel_phys_end());
		mark_reserved_range(metadata_phys, metadata_phys + metadata_bytes);

		const uint64_t limit_bytes = alloc_limit_bytes.load(std::memory_order_relaxed);
		const uint64_t limit_pages64 = limit_bytes == ~0ull ? page_count : (limit_bytes / page_size);
		const uint32_t limit_pages = static_cast<uint32_t>(limit_pages64 > page_count ? page_count : limit_pages64);
		const uint32_t dma_pages = static_cast<uint32_t>(dma_limit_bytes / page_size);
		const uint32_t dma_end = dma_pages < limit_pages ? dma_pages : limit_pages;

		zones[0].start_page = 0;
		zones[0].end_page = dma_end;

		const uint32_t normal_start = dma_end;
		const uint32_t normal_end = limit_pages;
		const uint32_t normal_len = normal_end > normal_start ? (normal_end - normal_start) : 0;
		const uint32_t shard_len = normal_zone_shards != 0 ? ((normal_len + normal_zone_shards - 1) / normal_zone_shards) : 0;

		for (uint32_t i = 0; i < normal_zone_shards; ++i)
		{
			const uint32_t z = 1 + i;
			zones[z].start_page = normal_start + i * shard_len;
			zones[z].end_page = zones[z].start_page + shard_len;
			if (zones[z].start_page > normal_end)
			{
				zones[z].start_page = normal_end;
			}
			if (zones[z].end_page > normal_end)
			{
				zones[z].end_page = normal_end;
			}
		}

		for (uint32_t i = 0; i < zone_count; ++i)
		{
			build_zone_buddy_from_metadata(zones[i]);
		}

		kernel::log::write("pmm pages total=");
		kernel::log::write_u64_dec(page_count);
		kernel::log::write(" free=");
		kernel::log::write_u64_dec(total_free_pages());
		kernel::log::write(" limit=");
		kernel::log::write_u64_hex(alloc_limit_bytes.load(std::memory_order_relaxed));
		kernel::log::write(" metadata=");
		kernel::log::write_u64_hex(metadata_phys);
		kernel::log::write(" metadata_bytes=");
		kernel::log::write_u64_dec(metadata_bytes);
		kernel::log::write("\n", 1);
	}

	Stats stats() noexcept
	{
		return Stats{
			.total_pages = page_count,
			.free_pages = total_free_pages(),
			.alloc_limit_bytes = alloc_limit_bytes.load(std::memory_order_relaxed),
		};
	}

	uint64_t alloc_limit() noexcept
	{
		return alloc_limit_bytes.load(std::memory_order_relaxed);
	}

	void set_alloc_limit(uint64_t limit_bytes) noexcept
	{
		alloc_limit_bytes.store(limit_bytes, std::memory_order_relaxed);
	}

	uint64_t alloc_page() noexcept
	{
		uint64_t cached = 0;
		if (pcp_pop(cached))
		{
			const uint32_t index = static_cast<uint32_t>(cached / page_size);
			const uint64_t limit = alloc_limit_bytes.load(std::memory_order_relaxed);
			if (index < page_count && cached < limit && !is_reserved(index) && !is_allocated(index))
			{
				set_allocated(index);
				return cached;
			}
		}

		Zone& z = pick_zone_for_alloc(false);
		uint32_t block = alloc_block(z, 0);
		if (block == null_page_index)
		{
			Zone& dma = zones[0];
			block = alloc_block(dma, 0);
			if (block == null_page_index)
			{
				return 0;
			}
		}

		set_allocated(block);
		return static_cast<uint64_t>(block) * page_size;
	}

	uint64_t alloc_page_at(uint64_t phys_addr) noexcept
	{
		if ((phys_addr % page_size) != 0)
		{
			return 0;
		}

		if (phys_addr >= alloc_limit_bytes.load(std::memory_order_relaxed))
		{
			return 0;
		}

		const uint32_t index = static_cast<uint32_t>(phys_addr / page_size);
		if (index >= page_count)
		{
			return 0;
		}

		if (is_allocated(index) || is_reserved(index))
		{
			return 0;
		}

		Zone& z = pick_zone_for_page(index);
		if (!zone_contains(z, index))
		{
			return 0;
		}

		if (!alloc_block_at(z, index))
		{
			return 0;
		}

		set_allocated(index);

		return phys_addr;
	}

	void free_page(uint64_t phys_addr) noexcept
	{
		if ((phys_addr % page_size) != 0)
		{
			return;
		}

		const uint32_t index = static_cast<uint32_t>(phys_addr / page_size);
		if (index >= page_count)
		{
			return;
		}

		if (!try_clear_allocated(index))
		{
			return;
		}

		if (pcp_push(phys_addr))
		{
			return;
		}

		Zone& z = pick_zone_for_page(index);
		free_block(z, index, 0);
	}
}
