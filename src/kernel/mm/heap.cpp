#include "kernel/mm/heap.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/arch/x86_64/cpu.hpp"
#include "kernel/arch/x86_64/cpu_local.hpp"
#include "kernel/arch/x86_64/apic/lapic.hpp"
#include "lib/align.hpp"
#include "lib/lock.hpp"
#include "kernel/log/log.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmalloc.hpp"
#include "kernel/mm/vmm.hpp"

namespace
{
	constexpr uint64_t page_size = 4096;

	constexpr uint64_t heap_base = 0xFFFFFE0000000000ull;
	constexpr uint64_t heap_limit = heap_base + (1ull << 30);
	constexpr uint64_t cacheline_size = 64;

	constexpr uint32_t slab_magic = 0x534C4142u;
	constexpr uint32_t large_magic = 0x4C415247u;

	constexpr size_t class_count = 8;
	constexpr size_t size_classes[class_count] = { 16, 32, 64, 128, 256, 512, 1024, 2048 };

	enum class SlabListKind : uint8_t
	{
		Empty,
		Partial,
		Full,
	};

	struct [[gnu::packed]] SlabHeader
	{
		uint32_t magic;
		uint16_t object_size;
		uint8_t list_kind;
		uint8_t reserved_0;
		uint64_t free_list;
		uint32_t free_count;
		uint32_t capacity;
		uint64_t next_slab;
		uint64_t prev_slab;
	};

	struct [[gnu::packed]] LargeHeader
	{
		uint32_t magic;
		uint32_t pages;
		uint64_t size_bytes;
	};

	struct FreeNode
	{
		FreeNode* next;
	};

	struct ClassList
	{
		uint64_t head_empty;
		uint64_t head_partial;
		uint64_t head_full;
		kernel::lib::McsLock lock;
	};

	constexpr uint32_t heap_pages = static_cast<uint32_t>((heap_limit - heap_base) / page_size);
	constexpr uint32_t heap_null_index = ~0u;

	uint8_t heap_free_order[heap_pages]{};
	uint32_t heap_prev[heap_pages]{};
	uint32_t heap_next[heap_pages]{};
	uint32_t heap_free_list_head[32]{};
	uint32_t heap_max_order = 0;

	uint32_t heap_compute_max_order(uint32_t pages) noexcept
	{
		uint32_t order = 0;
		while ((1u << (order + 1)) != 0 && (1u << (order + 1)) <= pages)
		{
			++order;
		}

		return order;
	}

	void heap_list_init() noexcept
	{
		for (uint32_t i = 0; i < 32; ++i)
		{
			heap_free_list_head[i] = heap_null_index;
		}
	}

	void heap_list_push(uint32_t order, uint32_t page_index) noexcept
	{
		heap_prev[page_index] = heap_null_index;
		heap_next[page_index] = heap_free_list_head[order];

		if (heap_free_list_head[order] != heap_null_index)
		{
			heap_prev[heap_free_list_head[order]] = page_index;
		}

		heap_free_list_head[order] = page_index;
	}

	void heap_list_remove(uint32_t order, uint32_t page_index) noexcept
	{
		const uint32_t prev = heap_prev[page_index];
		const uint32_t next = heap_next[page_index];

		if (prev != heap_null_index)
		{
			heap_next[prev] = next;
		}
		else
		{
			heap_free_list_head[order] = next;
		}

		if (next != heap_null_index)
		{
			heap_prev[next] = prev;
		}

		heap_prev[page_index] = heap_null_index;
		heap_next[page_index] = heap_null_index;
	}

	uint32_t heap_list_pop(uint32_t order) noexcept
	{
		const uint32_t head = heap_free_list_head[order];
		if (head == heap_null_index)
		{
			return heap_null_index;
		}

		heap_list_remove(order, head);
		return head;
	}

	void heap_add_free_block(uint32_t page_index, uint32_t order) noexcept
	{
		heap_free_order[page_index] = static_cast<uint8_t>(order);
		heap_list_push(order, page_index);
	}

	uint32_t heap_alloc_block(uint32_t order) noexcept
	{
		for (uint32_t o = order; o <= heap_max_order; ++o)
		{
			const uint32_t block = heap_list_pop(o);
			if (block == heap_null_index)
			{
				continue;
			}

			heap_free_order[block] = 0xFF;

			uint32_t current = block;
			uint32_t current_order = o;
			while (current_order > order)
			{
				--current_order;
				const uint32_t split = current + (1u << current_order);
				heap_add_free_block(split, current_order);
			}

			return current;
		}

		return heap_null_index;
	}

	void heap_free_block(uint32_t page_index, uint32_t order) noexcept
	{
		uint32_t current = page_index;
		uint32_t current_order = order;

		while (current_order < heap_max_order)
		{
			const uint32_t buddy = current ^ (1u << current_order);
			if (buddy >= heap_pages)
			{
				break;
			}

			if (heap_free_order[buddy] != current_order)
			{
				break;
			}

			heap_list_remove(current_order, buddy);
			heap_free_order[buddy] = 0xFF;

			current = buddy < current ? buddy : current;
			++current_order;
		}

		heap_add_free_block(current, current_order);
	}

	uint64_t heap_alloc_virt(uint32_t pages) noexcept
	{
		uint32_t order = 0;
		while ((1u << order) < pages)
		{
			++order;
		}

		if (order > heap_max_order)
		{
			return 0;
		}

		const uint32_t block = heap_alloc_block(order);
		if (block == heap_null_index)
		{
			return 0;
		}

		return heap_base + static_cast<uint64_t>(block) * page_size;
	}

	void heap_free_virt(uint64_t base, uint32_t pages) noexcept
	{
		if (base < heap_base || base >= heap_limit)
		{
			return;
		}

		const uint64_t offset = base - heap_base;
		if ((offset % page_size) != 0)
		{
			return;
		}

		uint32_t order = 0;
		while ((1u << order) < pages)
		{
			++order;
		}

		const uint32_t index = static_cast<uint32_t>(offset / page_size);
		if (index >= heap_pages)
		{
			return;
		}

		heap_free_block(index, order);
	}

	ClassList classes[class_count] = {};
	kernel::lib::McsLock heap_virt_lock;

	constexpr uint32_t max_apic_id = 256;
	constexpr uint32_t per_cpu_obj_capacity = 32;

	struct alignas(64) PerCpuClassCache
	{
		uint32_t count{0};
		void* objects[per_cpu_obj_capacity];
	};

	PerCpuClassCache per_cpu_cache[max_apic_id][class_count] = {};

	size_t class_index_for(size_t size) noexcept
	{
		for (size_t i = 0; i < class_count; ++i)
		{
			if (size <= size_classes[i])
			{
				return i;
			}
		}

		return class_count;
	}

	bool map_fresh_page(uint64_t virt) noexcept
	{
		const uint64_t phys = kernel::mm::pmm::alloc_page();
		if (phys == 0)
		{
			return false;
		}

		if (!kernel::mm::vmm::kernel_space().map_page(virt, phys, kernel::mm::vmm::PageFlags::Writable))
		{
			kernel::mm::pmm::free_page(phys);
			return false;
		}

		return true;
	}

	uint32_t cpu_index() noexcept;
	void* alloc_from_class_locked(ClassList& list, size_t class_i) noexcept;
	void free_to_class_locked(ClassList& list, void* ptr) noexcept;

	bool refill_cache(size_t class_i) noexcept
	{
		auto& list = classes[class_i];
		auto& c = per_cpu_cache[cpu_index()][class_i];

		kernel::lib::IrqMcsLockGuard class_guard(list.lock);

		while (c.count < per_cpu_obj_capacity)
		{
			void* obj = alloc_from_class_locked(list, class_i);
			if (!obj)
			{
				break;
			}

			c.objects[c.count] = obj;
			++c.count;
		}

		const bool result = c.count != 0;
		return result;
	}

	void drain_cache(size_t class_i, uint32_t drain_count) noexcept
	{
		auto& list = classes[class_i];
		auto& c = per_cpu_cache[cpu_index()][class_i];

		void* to_free[per_cpu_obj_capacity]{};
		uint32_t count = 0;

		{
			const uint64_t rflags = kernel::lib::irq_save_disable();

			const uint32_t cur = c.count;
			const uint32_t to_drain = drain_count < cur ? drain_count : cur;

			while (count < to_drain)
			{
				--c.count;
				to_free[count] = c.objects[c.count];
				++count;
			}

			kernel::lib::irq_restore(rflags);
		}

		if (count == 0)
		{
			return;
		}

		kernel::lib::IrqMcsLockGuard class_guard(list.lock);
		for (uint32_t i = 0; i < count; ++i)
		{
			free_to_class_locked(list, to_free[i]);
		}
	}

	uint64_t alloc_heap_pages(uint32_t pages) noexcept
	{
		kernel::lib::IrqMcsLockGuard guard(heap_virt_lock);

		const uint64_t base = heap_alloc_virt(pages);
		if (base == 0)
		{
			return 0;
		}

		for (uint32_t i = 0; i < pages; ++i)
		{
			const uint64_t v = base + static_cast<uint64_t>(i) * page_size;
			if (!map_fresh_page(v))
			{
				for (uint32_t j = 0; j < i; ++j)
				{
					const uint64_t rollback_v = base + static_cast<uint64_t>(j) * page_size;
					const uint64_t phys = kernel::mm::vmm::kernel_space().translate(rollback_v);
					kernel::mm::vmm::kernel_space().unmap_page(rollback_v);

					if (phys != 0)
					{
						kernel::mm::pmm::free_page(phys);
					}
				}

				heap_free_virt(base, pages);
				return 0;
			}
		}

		return base;
	}

	uint32_t cpu_index() noexcept
	{
		return kernel::arch::x86_64::cpu_local::cpu_id();
	}

	uint64_t slab_header_addr(uint64_t page_virt) noexcept
	{
		return page_virt;
	}

	SlabHeader* slab_from_page(uint64_t page_virt) noexcept
	{
		return reinterpret_cast<SlabHeader*>(slab_header_addr(page_virt));
	}

	LargeHeader* large_from_page(uint64_t page_virt) noexcept
	{
		return reinterpret_cast<LargeHeader*>(page_virt);
	}

	void list_remove(ClassList& list, uint64_t page) noexcept
	{
		auto* h = slab_from_page(page);
		const uint64_t prev = h->prev_slab;
		const uint64_t next = h->next_slab;

		if (prev != 0)
		{
			slab_from_page(prev)->next_slab = next;
		}
		else
		{
			switch (static_cast<SlabListKind>(h->list_kind))
			{
				case SlabListKind::Empty: list.head_empty = next; break;
				case SlabListKind::Partial: list.head_partial = next; break;
				case SlabListKind::Full: list.head_full = next; break;
			}
		}

		if (next != 0)
		{
			slab_from_page(next)->prev_slab = prev;
		}

		h->next_slab = 0;
		h->prev_slab = 0;
	}

	void list_push(ClassList& list, SlabListKind kind, uint64_t page) noexcept
	{
		auto* h = slab_from_page(page);
		h->list_kind = static_cast<uint8_t>(kind);
		h->prev_slab = 0;

		uint64_t* head = nullptr;
		switch (kind)
		{
			case SlabListKind::Empty: head = &list.head_empty; break;
			case SlabListKind::Partial: head = &list.head_partial; break;
			case SlabListKind::Full: head = &list.head_full; break;
		}

		h->next_slab = *head;
		if (*head != 0)
		{
			slab_from_page(*head)->prev_slab = page;
		}
		*head = page;
	}

	void* slab_pop_free(SlabHeader& h) noexcept
	{
		auto* node = reinterpret_cast<FreeNode*>(h.free_list);
		h.free_list = reinterpret_cast<uint64_t>(node->next);
		--h.free_count;
		return node;
	}

	void slab_push_free(SlabHeader& h, void* ptr) noexcept
	{
		auto* node = static_cast<FreeNode*>(ptr);
		node->next = reinterpret_cast<FreeNode*>(h.free_list);
		h.free_list = reinterpret_cast<uint64_t>(node);
		++h.free_count;
	}

	uint64_t create_slab_page(size_t class_i) noexcept
	{
		const uint64_t page = alloc_heap_pages(1);
		if (page == 0)
		{
			return 0;
		}

		auto* h = slab_from_page(page);
		h->magic = slab_magic;
		h->object_size = static_cast<uint16_t>(size_classes[class_i]);
		h->list_kind = static_cast<uint8_t>(SlabListKind::Empty);
		h->reserved_0 = 0;
		h->free_list = 0;
		h->free_count = 0;
		h->capacity = 0;
		h->next_slab = 0;
		h->prev_slab = 0;

		const uint64_t data_start = kernel::lib::align_up(page + sizeof(SlabHeader), cacheline_size);
		const uint64_t data_end = page + page_size;
		const uint64_t obj_size = h->object_size;

		uint64_t cursor = data_start;
		FreeNode* head = nullptr;

		while (cursor + obj_size <= data_end)
		{
			auto* n = reinterpret_cast<FreeNode*>(cursor);
			n->next = head;
			head = n;

			cursor += obj_size;
			++h->capacity;
			++h->free_count;
		}

		h->free_list = reinterpret_cast<uint64_t>(head);
		return page;
	}

	void* alloc_from_class(size_t class_i) noexcept
	{
		auto& list = classes[class_i];
		kernel::lib::IrqMcsLockGuard guard(list.lock);

		uint64_t page = list.head_partial;
		SlabListKind from_kind = SlabListKind::Partial;
		if (page == 0)
		{
			page = list.head_empty;
			from_kind = SlabListKind::Empty;
		}

		if (page == 0)
		{
			page = create_slab_page(class_i);
			if (page == 0)
			{
				return nullptr;
			}
			list_push(list, SlabListKind::Empty, page);
			from_kind = SlabListKind::Empty;
		}

		auto* h = slab_from_page(page);
		if (h->magic != slab_magic || h->free_count == 0)
		{
			kernel::log::write_line("heap slab corrupted");
			kernel::arch::x86_64::halt_forever();
		}

		list_remove(list, page);
		const uint32_t before_free = h->free_count;
		void* out = slab_pop_free(*h);
		const uint32_t after_free = h->free_count;

		if (after_free == 0)
		{
			list_push(list, SlabListKind::Full, page);
			return out;
		}

		if (before_free == h->capacity)
		{
			list_push(list, SlabListKind::Partial, page);
			return out;
		}

		if (from_kind == SlabListKind::Empty)
		{
			list_push(list, SlabListKind::Partial, page);
			return out;
		}

		list_push(list, SlabListKind::Partial, page);
		return out;
	}

	void free_to_class(size_t class_i, void* ptr) noexcept
	{
		const uint64_t addr = reinterpret_cast<uint64_t>(ptr);
		const uint64_t page = kernel::lib::align_down(addr, page_size);
		auto& list = classes[class_i];

		kernel::lib::IrqMcsLockGuard guard(list.lock);
		auto* h = slab_from_page(page);
		if (h->magic != slab_magic)
		{
			kernel::log::write_line("heap free slab corrupted header");
			kernel::arch::x86_64::halt_forever();
		}

		const uint32_t before_free = h->free_count;
		const auto kind = static_cast<SlabListKind>(h->list_kind);
		if (before_free > h->capacity)
		{
			kernel::log::write_line("heap free slab corrupted counters");
			kernel::arch::x86_64::halt_forever();
		}

		list_remove(list, page);
		slab_push_free(*h, ptr);

		if (h->free_count == h->capacity)
		{
			list_push(list, SlabListKind::Empty, page);
			return;
		}

		if (kind == SlabListKind::Full)
		{
			list_push(list, SlabListKind::Partial, page);
			return;
		}

		list_push(list, SlabListKind::Partial, page);
	}

	void free_to_class_locked(ClassList& list, void* ptr) noexcept
	{
		const uint64_t addr = reinterpret_cast<uint64_t>(ptr);
		const uint64_t page = kernel::lib::align_down(addr, page_size);
		auto* h = slab_from_page(page);
		if (h->magic != slab_magic)
		{
			kernel::log::write_line("heap free slab corrupted header");
			kernel::arch::x86_64::halt_forever();
		}

		const uint32_t before_free = h->free_count;
		const auto kind = static_cast<SlabListKind>(h->list_kind);
		if (before_free > h->capacity)
		{
			kernel::log::write_line("heap free slab corrupted counters");
			kernel::arch::x86_64::halt_forever();
		}

		list_remove(list, page);
		slab_push_free(*h, ptr);

		if (h->free_count == h->capacity)
		{
			list_push(list, SlabListKind::Empty, page);
			return;
		}

		if (kind == SlabListKind::Full)
		{
			list_push(list, SlabListKind::Partial, page);
			return;
		}

		list_push(list, SlabListKind::Partial, page);
	}

	void* alloc_from_class_locked(ClassList& list, size_t class_i) noexcept
	{
		uint64_t page = list.head_partial;
		SlabListKind from_kind = SlabListKind::Partial;
		if (page == 0)
		{
			page = list.head_empty;
			from_kind = SlabListKind::Empty;
		}

		if (page == 0)
		{
			page = create_slab_page(class_i);
			if (page == 0)
			{
				return nullptr;
			}
			list_push(list, SlabListKind::Empty, page);
			from_kind = SlabListKind::Empty;
		}

		auto* h = slab_from_page(page);
		if (h->magic != slab_magic || h->free_count == 0)
		{
			kernel::log::write_line("heap slab corrupted");
			kernel::arch::x86_64::halt_forever();
		}

		list_remove(list, page);
		const uint32_t before_free = h->free_count;
		void* out = slab_pop_free(*h);
		const uint32_t after_free = h->free_count;

		if (after_free == 0)
		{
			list_push(list, SlabListKind::Full, page);
			return out;
		}

		if (before_free == h->capacity)
		{
			list_push(list, SlabListKind::Partial, page);
			return out;
		}

		if (from_kind == SlabListKind::Empty)
		{
			list_push(list, SlabListKind::Partial, page);
			return out;
		}

		list_push(list, SlabListKind::Partial, page);
		return out;
	}

	bool cache_pop(size_t class_i, void*& out) noexcept
	{
		auto& c = per_cpu_cache[cpu_index()][class_i];

		const uint64_t rflags = kernel::lib::irq_save_disable();

		if (c.count == 0)
		{
			kernel::lib::irq_restore(rflags);
			return false;
		}

		--c.count;
		out = c.objects[c.count];

		kernel::lib::irq_restore(rflags);
		return true;
	}

	bool cache_push(size_t class_i, void* ptr) noexcept
	{
		auto& c = per_cpu_cache[cpu_index()][class_i];

		const uint64_t rflags = kernel::lib::irq_save_disable();

		if (c.count >= per_cpu_obj_capacity)
		{
			kernel::lib::irq_restore(rflags);
			return false;
		}

		c.objects[c.count] = ptr;
		++c.count;

		kernel::lib::irq_restore(rflags);
		return true;
	}

	void free_to_slab(void* ptr) noexcept
	{
		const uint64_t addr = reinterpret_cast<uint64_t>(ptr);
		const uint64_t page = kernel::lib::align_down(addr, page_size);
		auto* h = slab_from_page(page);
		if (h->magic != slab_magic)
		{
			kernel::log::write_line("heap free corrupted slab");
			kernel::arch::x86_64::halt_forever();
		}

		size_t class_i = class_index_for(h->object_size);
		if (class_i >= class_count)
		{
			kernel::log::write_line("heap free invalid slab class");
			kernel::arch::x86_64::halt_forever();
		}

		if (!cache_push(class_i, ptr))
		{
			drain_cache(class_i, per_cpu_obj_capacity / 2);
			if (!cache_push(class_i, ptr))
			{
				free_to_class(class_i, ptr);
			}
		}
	}

	void* alloc_large(size_t size) noexcept
	{
		const uint64_t total = kernel::lib::align_up(sizeof(LargeHeader) + size, page_size);
		const uint32_t pages = static_cast<uint32_t>(total / page_size);

		auto* base_ptr = kernel::mm::vmalloc::vmalloc(total, page_size);
		if (!base_ptr)
		{
			return nullptr;
		}
		const uint64_t base = reinterpret_cast<uint64_t>(base_ptr);

		auto* h = large_from_page(base);
		h->magic = large_magic;
		h->pages = pages;
		h->size_bytes = size;

		return reinterpret_cast<void*>(base + sizeof(LargeHeader));
	}

	void free_large(void* ptr) noexcept
	{
		const uint64_t addr = reinterpret_cast<uint64_t>(ptr);
		const uint64_t page = kernel::lib::align_down(addr, page_size);
		auto* h = large_from_page(page);
		const uint32_t pages = h->pages;
		const uint64_t total = static_cast<uint64_t>(pages) * page_size;

		if (h->magic != large_magic || pages == 0)
		{
			kernel::log::write_line("heap free large corrupted header");
			kernel::arch::x86_64::halt_forever();
		}

		kernel::mm::vmalloc::vfree(reinterpret_cast<void*>(page), total);
	}

	bool is_slab_ptr(const void* ptr) noexcept
	{
		const uint64_t addr = reinterpret_cast<uint64_t>(ptr);
		const uint64_t page = kernel::lib::align_down(addr, page_size);
		const auto* h = slab_from_page(page);
		return h->magic == slab_magic;
	}

	bool is_large_ptr(const void* ptr) noexcept
	{
		const uint64_t addr = reinterpret_cast<uint64_t>(ptr);
		const uint64_t page = kernel::lib::align_down(addr, page_size);
		const auto* h = large_from_page(page);
		return h->magic == large_magic;
	}
}

namespace kernel::mm::heap
{
	void init() noexcept
	{
		kernel::lib::IrqMcsLockGuard guard(heap_virt_lock);

		heap_list_init();
		heap_max_order = heap_compute_max_order(heap_pages);
		for (uint32_t i = 0; i < heap_pages; ++i)
		{
			heap_free_order[i] = 0xFF;
			heap_prev[i] = heap_null_index;
			heap_next[i] = heap_null_index;
		}

		heap_add_free_block(0, heap_max_order);

		for (size_t i = 0; i < class_count; ++i)
		{
			classes[i].head_empty = 0;
			classes[i].head_partial = 0;
			classes[i].head_full = 0;
		}

		for (uint32_t cpu = 0; cpu < max_apic_id; ++cpu)
		{
			for (size_t ci = 0; ci < class_count; ++ci)
			{
				per_cpu_cache[cpu][ci].count = 0;
			}
		}

		kernel::log::write("heap base=");
		kernel::log::write_u64_hex(heap_base);
		kernel::log::write(" limit=");
		kernel::log::write_u64_hex(heap_limit);
		kernel::log::write(" pages=");
		kernel::log::write_u64_dec(heap_pages);
		kernel::log::write("\n", 1);
	}

	void* alloc(size_t size) noexcept
	{
		if (size == 0)
		{
			size = 1;
		}

		const size_t ci = class_index_for(size);
		if (ci < class_count)
		{
			void* cached = nullptr;
			if (cache_pop(ci, cached))
			{
				return cached;
			}

			if (refill_cache(ci) && cache_pop(ci, cached))
			{
				return cached;
			}

			return alloc_from_class(ci);
		}

		return alloc_large(size);
	}

	void free(void* ptr) noexcept
	{
		if (!ptr)
		{
			return;
		}

		if (is_slab_ptr(ptr))
		{
			free_to_slab(ptr);
			return;
		}

		if (is_large_ptr(ptr))
		{
			free_large(ptr);
			return;
		}

		kernel::log::write_line("heap free unknown pointer");
		kernel::arch::x86_64::halt_forever();
	}
}
