#include "kernel/mm/ioremap.hpp"

#include "lib/align.hpp"
#include "kernel/arch/x86_64/cpu.hpp"
#include "kernel/log/log.hpp"
#include "kernel/mm/vmar.hpp"
#include "kernel/mm/vmm.hpp"

namespace
{
	constexpr uint64_t page_size = 4096;
}

namespace kernel::mm::ioremap
{
	void init() noexcept
	{
		(void)0;
	}

	void* map(uint64_t phys, size_t size) noexcept
	{
		if (size == 0)
		{
			return nullptr;
		}

		/* Align down to a page and return a pointer with the original byte offset restored. */
		const uint64_t phys_page = kernel::lib::align_down(phys, page_size);
		const uint64_t off = phys - phys_page;

		const uint64_t total = kernel::lib::align_up(static_cast<uint64_t>(size) + off, page_size);
		/* VMAR hands us a unique virtual range; VMM then wires it to the requested phys pages. */
		auto* base_ptr = kernel::mm::vmar::ioremap_alloc(total, page_size);
		if (!base_ptr)
		{
			kernel::log::write_line("ioremap oom");
			kernel::arch::x86_64::halt_forever();
		}
		const uint64_t base = reinterpret_cast<uint64_t>(base_ptr);

		/* MMIO mappings are typically uncached and never executable. */
		const auto flags = kernel::mm::vmm::PageFlags::Writable |
			kernel::mm::vmm::PageFlags::NoExecute |
			kernel::mm::vmm::PageFlags::CacheDisable;

		if (!kernel::mm::vmm::map_range(base, phys_page, total, flags))
		{
			kernel::log::write_line("ioremap map failed");
			kernel::arch::x86_64::halt_forever();
		}
		return reinterpret_cast<void*>(base + off);
	}

	void unmap(void* addr, size_t size) noexcept
	{
		if (!addr || size == 0)
		{
			return;
		}

		const uint64_t virt = reinterpret_cast<uint64_t>(addr);
		const uint64_t base = kernel::lib::align_down(virt, page_size);
		const uint64_t off = virt - base;
		const uint64_t total = kernel::lib::align_up(static_cast<uint64_t>(size) + off, page_size);

		uint64_t v = base;
		uint64_t remaining = total;
		while (remaining != 0)
		{
			kernel::mm::vmm::kernel_space().unmap_page(v);
			v += page_size;
			remaining -= page_size;
		}

		kernel::mm::vmar::ioremap_free(reinterpret_cast<void*>(base), total);
	}
}
