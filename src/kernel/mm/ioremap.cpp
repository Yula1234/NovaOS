#include "kernel/mm/ioremap.hpp"

#include "lib/align.hpp"
#include "kernel/arch/x86_64/cpu.hpp"
#include "kernel/log/log.hpp"
#include "kernel/mm/vmm.hpp"

namespace
{
	constexpr uint64_t page_size = 4096;

	constexpr uint64_t mmio_base = 0xFFFFFD0000000000ull;
	constexpr uint64_t mmio_limit = mmio_base + (1ull << 30);

	uint64_t next_mmio_virt = mmio_base;
}

namespace kernel::mm::ioremap
{
	void init() noexcept
	{
		next_mmio_virt = mmio_base;
	}

	void* map(uint64_t phys, size_t size) noexcept
	{
		if (size == 0)
		{
			return nullptr;
		}

		const uint64_t phys_page = kernel::lib::align_down(phys, page_size);
		const uint64_t off = phys - phys_page;

		const uint64_t total = kernel::lib::align_up(static_cast<uint64_t>(size) + off, page_size);
		const uint64_t base = kernel::lib::align_up(next_mmio_virt, page_size);

		if (base + total > mmio_limit)
		{
			kernel::log::write_line("ioremap oom");
			kernel::arch::x86_64::halt_forever();
		}

		const auto flags = kernel::mm::vmm::PageFlags::Writable |
			kernel::mm::vmm::PageFlags::NoExecute |
			kernel::mm::vmm::PageFlags::CacheDisable;

		if (!kernel::mm::vmm::map_range(base, phys_page, total, flags))
		{
			kernel::log::write_line("ioremap map failed");
			kernel::arch::x86_64::halt_forever();
		}

		next_mmio_virt = base + total;
		return reinterpret_cast<void*>(base + off);
	}
}
