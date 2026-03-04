#include "kernel/mm/heap.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/arch/x86_64/cpu.hpp"
#include "kernel/arch/x86_64/apic/lapic.hpp"
#include "lib/align.hpp"
#include "lib/lock.hpp"
#include "kernel/log/log.hpp"
#include "kernel/mm/pmm.hpp"
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
		kernel::lib::SpinLock lock;
	};

	uint64_t next_heap_virt = heap_base;
	ClassList classes[class_count] = {};
	kernel::lib::SpinLock heap_virt_lock;

	constexpr uint32_t max_apic_id = 256;
	constexpr uint32_t per_cpu_obj_capacity = 32;

	struct PerCpuClassCache
	{
		kernel::lib::SpinLock lock;
		uint32_t count;
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

		kernel::lib::IrqLockGuard<kernel::lib::SpinLock> class_guard(list.lock);
		kernel::lib::IrqLockGuard<kernel::lib::SpinLock> cache_guard(c.lock);

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

		return c.count != 0;
	}

	void drain_cache(size_t class_i, uint32_t drain_count) noexcept
	{
		auto& list = classes[class_i];
		auto& c = per_cpu_cache[cpu_index()][class_i];

		void* to_free[per_cpu_obj_capacity]{};
		uint32_t count = 0;

		{
			kernel::lib::IrqLockGuard<kernel::lib::SpinLock> cache_guard(c.lock);
			const uint32_t available = c.count;
			count = drain_count < available ? drain_count : available;
			for (uint32_t i = 0; i < count; ++i)
			{
				to_free[i] = c.objects[c.count - 1];
				--c.count;
			}
		}

		if (count == 0)
		{
			return;
		}

		kernel::lib::IrqLockGuard<kernel::lib::SpinLock> class_guard(list.lock);
		for (uint32_t i = 0; i < count; ++i)
		{
			free_to_class_locked(list, to_free[i]);
		}
	}

	uint64_t alloc_heap_pages(uint32_t pages) noexcept
	{
		kernel::lib::IrqLockGuard<kernel::lib::SpinLock> guard(heap_virt_lock);

		const uint64_t total = static_cast<uint64_t>(pages) * page_size;
		const uint64_t base = kernel::lib::align_up(next_heap_virt, page_size);

		if (base + total > heap_limit)
		{
			return 0;
		}

		for (uint32_t i = 0; i < pages; ++i)
		{
			const uint64_t v = base + static_cast<uint64_t>(i) * page_size;
			if (!map_fresh_page(v))
			{
				return 0;
			}
		}

		next_heap_virt = base + total;
		return base;
	}

	uint32_t cpu_index() noexcept
	{
		return kernel::arch::x86_64::apic::lapic::id() & 0xFFu;
	}

	uint64_t slab_header_addr(uint64_t page_virt) noexcept
	{
		const uint64_t raw = page_virt + page_size - sizeof(SlabHeader);
		return kernel::lib::align_down(raw, cacheline_size);
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

		const uint64_t data_start = kernel::lib::align_up(page, cacheline_size);
		const uint64_t data_end = slab_header_addr(page);
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
		kernel::lib::IrqLockGuard<kernel::lib::SpinLock> guard(list.lock);

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

		kernel::lib::IrqLockGuard<kernel::lib::SpinLock> guard(list.lock);
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
		kernel::lib::IrqLockGuard<kernel::lib::SpinLock> guard(c.lock);
		if (c.count == 0)
		{
			return false;
		}

		out = c.objects[c.count - 1];
		--c.count;
		return true;
	}

	bool cache_push(size_t class_i, void* ptr) noexcept
	{
		auto& c = per_cpu_cache[cpu_index()][class_i];
		kernel::lib::IrqLockGuard<kernel::lib::SpinLock> guard(c.lock);
		if (c.count >= per_cpu_obj_capacity)
		{
			return false;
		}

		c.objects[c.count] = ptr;
		++c.count;
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

		const uint64_t base = alloc_heap_pages(pages);
		if (base == 0)
		{
			return nullptr;
		}

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

		if (h->magic != large_magic || pages == 0)
		{
			kernel::log::write_line("heap free large corrupted header");
			kernel::arch::x86_64::halt_forever();
		}

		for (uint32_t i = 0; i < pages; ++i)
		{
			const uint64_t v = page + static_cast<uint64_t>(i) * page_size;
			const uint64_t phys = kernel::mm::vmm::kernel_space().translate(v);
			kernel::mm::vmm::kernel_space().unmap_page(v);

			if (phys != 0)
			{
				kernel::mm::pmm::free_page(phys);
			}
		}
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
		kernel::lib::IrqLockGuard<kernel::lib::SpinLock> guard(heap_virt_lock);
		next_heap_virt = heap_base;

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
