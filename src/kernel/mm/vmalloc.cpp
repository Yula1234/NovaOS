#include "kernel/mm/vmalloc.hpp"

#include "kernel/arch/x86_64/cpu.hpp"
#include "kernel/log/log.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmar.hpp"
#include "kernel/mm/vmm.hpp"
#include "lib/align.hpp"

namespace
{
	constexpr uint64_t page_size = 4096;

	bool map_anonymous_pages(uint64_t base, uint64_t pages) noexcept
	{
		for (uint64_t i = 0; i < pages; ++i)
		{
			const uint64_t v = base + i * page_size;
			const uint64_t phys = kernel::mm::pmm::alloc_page();
			if (phys == 0)
			{
				for (uint64_t j = 0; j < i; ++j)
				{
					const uint64_t rollback_v = base + j * page_size;
					const uint64_t rollback_phys = kernel::mm::vmm::kernel_space().translate(rollback_v);
					kernel::mm::vmm::kernel_space().unmap_page(rollback_v);
					if (rollback_phys != 0)
					{
						kernel::mm::pmm::free_page(rollback_phys);
					}
				}
				return false;
			}

			const auto flags = kernel::mm::vmm::PageFlags::Writable | kernel::mm::vmm::PageFlags::NoExecute;
			if (!kernel::mm::vmm::kernel_space().map_page(v, phys, flags))
			{
				kernel::mm::pmm::free_page(phys);
				for (uint64_t j = 0; j < i; ++j)
				{
					const uint64_t rollback_v = base + j * page_size;
					const uint64_t rollback_phys = kernel::mm::vmm::kernel_space().translate(rollback_v);
					kernel::mm::vmm::kernel_space().unmap_page(rollback_v);
					if (rollback_phys != 0)
					{
						kernel::mm::pmm::free_page(rollback_phys);
					}
				}
				return false;
			}
		}

		return true;
	}

	void unmap_and_free_pages(uint64_t base, uint64_t pages) noexcept
	{
		for (uint64_t i = 0; i < pages; ++i)
		{
			const uint64_t v = base + i * page_size;
			const uint64_t phys = kernel::mm::vmm::kernel_space().translate(v);
			kernel::mm::vmm::kernel_space().unmap_page(v);
			if (phys != 0)
			{
				kernel::mm::pmm::free_page(phys);
			}
		}
	}

	[[noreturn]] void vmalloc_panic(const char* msg) noexcept
	{
		kernel::log::write_line(msg);
		kernel::arch::x86_64::halt_forever();
	}
}

namespace kernel::mm::vmalloc
{
	void* vmalloc(uint64_t size, uint64_t align) noexcept
	{
		if (size == 0)
		{
			return nullptr;
		}

		if (align == 0)
		{
			align = page_size;
		}

		if ((align & (align - 1)) != 0)
		{
			vmalloc_panic("vmalloc invalid align");
		}

		if (align < page_size)
		{
			align = page_size;
		}

		const uint64_t total = kernel::lib::align_up(size, page_size);
		const uint64_t pages = total / page_size;

		auto* base_ptr = kernel::mm::vmar::alloc(kernel::mm::vmar::Arena::Vmalloc, total, align);
		if (!base_ptr)
		{
			return nullptr;
		}

		const uint64_t base = reinterpret_cast<uint64_t>(base_ptr);
		if (!map_anonymous_pages(base, pages))
		{
			kernel::mm::vmar::free(kernel::mm::vmar::Arena::Vmalloc, base_ptr, total);
			return nullptr;
		}

		return base_ptr;
	}

	void vfree(void* addr, uint64_t size) noexcept
	{
		if (!addr || size == 0)
		{
			return;
		}

		const uint64_t base = reinterpret_cast<uint64_t>(addr);
		if ((base % page_size) != 0)
		{
			vmalloc_panic("vfree addr not page aligned");
		}

		const uint64_t total = kernel::lib::align_up(size, page_size);
		void* recorded_base = nullptr;
		uint64_t recorded_size = 0;
		if (!kernel::mm::vmar::lookup(kernel::mm::vmar::Arena::Vmalloc, addr, recorded_base, recorded_size))
		{
			vmalloc_panic("vfree lookup failed");
		}

		if (recorded_base != addr)
		{
			vmalloc_panic("vfree requires base addr");
		}

		if (recorded_size != total)
		{
			vmalloc_panic("vfree size mismatch");
		}

		const uint64_t pages = total / page_size;

		unmap_and_free_pages(base, pages);

		if (!kernel::mm::vmar::free(kernel::mm::vmar::Arena::Vmalloc, addr, total))
		{
			vmalloc_panic("vfree vmar free failed");
		}
	}

	void vfree(void* addr) noexcept
	{
		if (!addr)
		{
			return;
		}

		void* base_ptr = nullptr;
		uint64_t size = 0;
		if (!kernel::mm::vmar::lookup(kernel::mm::vmar::Arena::Vmalloc, addr, base_ptr, size))
		{
			vmalloc_panic("vfree lookup failed");
		}

		if (base_ptr != addr)
		{
			vmalloc_panic("vfree requires base addr");
		}

		vfree(base_ptr, size);
	}
}
