#include "kernel/mm/heap.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/arch/x86_64/cpu.hpp"
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

	constexpr uint32_t slab_magic = 0x534C4142u;
	constexpr uint32_t large_magic = 0x4C415247u;

	constexpr size_t class_count = 8;
	constexpr size_t size_classes[class_count] = { 16, 32, 64, 128, 256, 512, 1024, 2048 };

	struct [[gnu::packed]] SlabHeader
	{
		uint32_t magic;
		uint16_t object_size;
		uint16_t reserved;
		uint64_t free_list;
		uint32_t free_count;
		uint32_t capacity;
		uint64_t next_slab;
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
		uint64_t head_page;
	};

	uint64_t next_heap_virt = heap_base;
	ClassList classes[class_count] = {};
	kernel::lib::SpinLock heap_lock;

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

	uint64_t alloc_heap_pages(uint32_t pages) noexcept
	{
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

	SlabHeader* slab_from_page(uint64_t page_virt) noexcept
	{
		return reinterpret_cast<SlabHeader*>(page_virt);
	}

	LargeHeader* large_from_page(uint64_t page_virt) noexcept
	{
		return reinterpret_cast<LargeHeader*>(page_virt);
	}

	void* alloc_from_slab(size_t class_i) noexcept
	{
		uint64_t page = classes[class_i].head_page;

		while (page != 0)
		{
			auto* h = slab_from_page(page);
			if (h->magic == slab_magic && h->free_count != 0)
			{
				break;
			}

			page = h->next_slab;
		}

		if (page == 0)
		{
			page = alloc_heap_pages(1);
			if (page == 0)
			{
				return nullptr;
			}

			auto* h = slab_from_page(page);
			h->magic = slab_magic;
			h->object_size = static_cast<uint16_t>(size_classes[class_i]);
			h->reserved = 0;
			h->free_list = 0;
			h->free_count = 0;
			h->capacity = 0;
			h->next_slab = classes[class_i].head_page;
			classes[class_i].head_page = page;

			const uint64_t data_start = kernel::lib::align_up(page + sizeof(SlabHeader), static_cast<uint64_t>(16));
			const uint64_t data_end = page + page_size;
			const uint64_t obj_size = h->object_size;

			uint64_t cursor = data_start;
			FreeNode* head = nullptr;

			while (cursor + obj_size <= data_end)
			{
				auto* node = reinterpret_cast<FreeNode*>(cursor);
				node->next = head;
				head = node;

				cursor += obj_size;
				++h->capacity;
				++h->free_count;
			}

			h->free_list = reinterpret_cast<uint64_t>(head);
		}

		auto* h = slab_from_page(page);
		auto* node = reinterpret_cast<FreeNode*>(h->free_list);
		h->free_list = reinterpret_cast<uint64_t>(node->next);
		--h->free_count;

		return node;
	}

	void free_to_slab(void* ptr) noexcept
	{
		const uint64_t addr = reinterpret_cast<uint64_t>(ptr);
		const uint64_t page = kernel::lib::align_down(addr, page_size);
		auto* h = slab_from_page(page);

		auto* node = static_cast<FreeNode*>(ptr);
		node->next = reinterpret_cast<FreeNode*>(h->free_list);
		h->free_list = reinterpret_cast<uint64_t>(node);
		++h->free_count;
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
		kernel::lib::IrqLockGuard<kernel::lib::SpinLock> guard(heap_lock);

		next_heap_virt = heap_base;

		for (size_t i = 0; i < class_count; ++i)
		{
			classes[i].head_page = 0;
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

		kernel::lib::IrqLockGuard<kernel::lib::SpinLock> guard(heap_lock);

		const size_t ci = class_index_for(size);
		if (ci < class_count)
		{
			return alloc_from_slab(ci);
		}

		return alloc_large(size);
	}

	void free(void* ptr) noexcept
	{
		if (!ptr)
		{
			return;
		}

		kernel::lib::IrqLockGuard<kernel::lib::SpinLock> guard(heap_lock);

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
