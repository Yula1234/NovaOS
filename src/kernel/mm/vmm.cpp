#include "kernel/mm/vmm.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/arch/x86_64/cpu.hpp"
#include "kernel/arch/x86_64/tlb.hpp"
#include "kernel/log/log.hpp"
#include "kernel/mm/physmap.hpp"
#include "kernel/mm/pmm.hpp"

namespace
{
	constexpr uint64_t page_size = 4096;
	constexpr uint64_t large_page_size = 0x200000;
	constexpr uint64_t early_mapped_limit = 0x100000000ull;
	constexpr uint64_t addr_mask = 0x000FFFFFFFFFF000ull;

	constexpr uint64_t bit_present = 1ull << 0;
	constexpr uint64_t bit_writable = 1ull << 1;
	[[maybe_unused]] constexpr uint64_t bit_user = 1ull << 2;
	constexpr uint64_t bit_ps = 1ull << 7;
	constexpr uint64_t bit_nx = 1ull << 63;
	constexpr uint64_t bit_owned = 1ull << 9;

	constexpr uint64_t pml4_index(uint64_t v) noexcept
	{
		return (v >> 39) & 0x1FF;
	}

	uint64_t make_large_pde(uint64_t phys, kernel::mm::vmm::PageFlags flags) noexcept
	{
		uint64_t value = phys & 0x000FFFFFFFE00000ull;
		value |= static_cast<uint64_t>(flags) & ~bit_nx;
		value |= bit_ps;

		if ((static_cast<uint64_t>(flags) & static_cast<uint64_t>(kernel::mm::vmm::PageFlags::NoExecute)) != 0)
		{
			value |= bit_nx;
		}

		return value;
	}

	constexpr uint64_t pdpt_index(uint64_t v) noexcept
	{
		return (v >> 30) & 0x1FF;
	}

	constexpr uint64_t pd_index(uint64_t v) noexcept
	{
		return (v >> 21) & 0x1FF;
	}

	constexpr uint64_t pt_index(uint64_t v) noexcept
	{
		return (v >> 12) & 0x1FF;
	}

	inline uint64_t* table_virt(uint64_t table_phys) noexcept
	{
		return static_cast<uint64_t*>(kernel::mm::physmap::to_virt(table_phys));
	}

	inline const uint64_t* table_virt_const(uint64_t table_phys) noexcept
	{
		return static_cast<const uint64_t*>(kernel::mm::physmap::to_virt_const(table_phys));
	}

	uint64_t alloc_table_page() noexcept
	{
		const uint64_t phys = kernel::mm::pmm::alloc_page();
		if (phys == 0)
		{
			return 0;
		}

		auto* t = table_virt(phys);
		for (size_t i = 0; i < 512; ++i)
		{
			t[i] = 0;
		}

		return phys;
	}

	uint64_t make_entry(uint64_t phys, kernel::mm::vmm::PageFlags flags) noexcept
	{
		uint64_t value = phys & addr_mask;

		value |= static_cast<uint64_t>(flags) & ~bit_nx;

		if ((static_cast<uint64_t>(flags) & static_cast<uint64_t>(kernel::mm::vmm::PageFlags::NoExecute)) != 0)
		{
			value |= bit_nx;
		}

		return value;
	}

	bool entry_present(uint64_t entry) noexcept
	{
		return (entry & bit_present) != 0;
	}

	bool entry_is_large(uint64_t entry) noexcept
	{
		return (entry & bit_ps) != 0;
	}

	uint64_t entry_addr(uint64_t entry) noexcept
	{
		return entry & addr_mask;
	}

	bool table_empty(uint64_t table_phys) noexcept
	{
		const auto* t = table_virt_const(table_phys);
		for (size_t i = 0; i < 512; ++i)
		{
			if ((t[i] & bit_present) != 0)
			{
				return false;
			}
		}

		return true;
	}

	uint64_t ensure_table(uint64_t parent_phys, size_t index, uint64_t flags) noexcept;

	static bool map_large_2m(
		kernel::mm::vmm::AddressSpace& as,
		uint64_t virt,
		uint64_t phys,
		kernel::mm::vmm::PageFlags flags
	) noexcept
	{
		if ((virt % large_page_size) != 0 || (phys % large_page_size) != 0)
		{
			return false;
		}

		const uint64_t pml4_i = pml4_index(virt);
		const uint64_t pdpt_i = pdpt_index(virt);
		const uint64_t pd_i = pd_index(virt);

		const uint64_t table_flags = bit_present | bit_writable;

		const uint64_t pdpt_phys = ensure_table(as.pml4_phys(), pml4_i, table_flags);
		if (!pdpt_phys)
		{
			return false;
		}

		const uint64_t pd_phys = ensure_table(pdpt_phys, pdpt_i, table_flags);
		if (!pd_phys)
		{
			return false;
		}

		auto* pd = table_virt(pd_phys);
		pd[pd_i] = make_large_pde(phys, flags | kernel::mm::vmm::PageFlags::Present);
		kernel::arch::x86_64::invlpg(reinterpret_cast<void*>(virt));
		return true;
	}

	uint64_t ensure_table(uint64_t parent_phys, size_t index, uint64_t flags) noexcept
	{
		auto* parent = table_virt(parent_phys);
		const uint64_t entry = parent[index];
		if (entry_present(entry))
		{
			if (entry_is_large(entry))
			{
				return 0;
			}

			return entry_addr(entry);
		}

		const uint64_t child_phys = alloc_table_page();
		if (child_phys == 0)
		{
			return 0;
		}

		parent[index] = (child_phys & addr_mask) | flags | bit_owned;
		return child_phys;
	}

	bool remove_table_if_empty(uint64_t parent_phys, size_t index) noexcept
	{
		auto* parent = table_virt(parent_phys);
		const uint64_t entry = parent[index];
		if (!entry_present(entry) || entry_is_large(entry))
		{
			return false;
		}

		if ((entry & bit_owned) == 0)
		{
			return false;
		}

		const uint64_t child_phys = entry_addr(entry);
		if (!table_empty(child_phys))
		{
			return false;
		}

		parent[index] = 0;
		kernel::mm::pmm::free_page(child_phys);
		return true;
	}

	kernel::mm::vmm::AddressSpace kernel_as;
}

namespace kernel::mm::vmm
{
	AddressSpace::AddressSpace() noexcept
		: pml4_phys_(0)
	{
	}

	AddressSpace::AddressSpace(uint64_t pml4_phys) noexcept
		: pml4_phys_(pml4_phys & addr_mask)
	{
	}

	void AddressSpace::reset(uint64_t pml4_phys) noexcept
	{
		kernel::lib::IrqLockGuard<kernel::lib::SpinLock> guard(lock_);
		pml4_phys_ = pml4_phys & addr_mask;
	}

	uint64_t AddressSpace::pml4_phys() const noexcept
	{
		return pml4_phys_;
	}

	bool AddressSpace::map_page(uint64_t virt, uint64_t phys, PageFlags flags) noexcept
	{
		kernel::lib::IrqLockGuard<kernel::lib::SpinLock> guard(lock_);

		if ((virt % page_size) != 0 || (phys % page_size) != 0)
		{
			return false;
		}

		const uint64_t pml4_i = pml4_index(virt);
		const uint64_t pdpt_i = pdpt_index(virt);
		const uint64_t pd_i = pd_index(virt);
		const uint64_t pt_i = pt_index(virt);

		const uint64_t table_flags = bit_present | bit_writable;

		const uint64_t pdpt_phys = ensure_table(pml4_phys_, pml4_i, table_flags);
		if (!pdpt_phys)
		{
			return false;
		}

		const uint64_t pd_phys = ensure_table(pdpt_phys, pdpt_i, table_flags);
		if (!pd_phys)
		{
			return false;
		}

		const uint64_t pt_phys = ensure_table(pd_phys, pd_i, table_flags);
		if (!pt_phys)
		{
			return false;
		}

		auto* pt = table_virt(pt_phys);
		pt[pt_i] = make_entry(phys, flags | PageFlags::Present);

		kernel::arch::x86_64::tlb::shootdown_page(pml4_phys_, virt);
		return true;
	}

	bool AddressSpace::unmap_page(uint64_t virt) noexcept
	{
		kernel::lib::IrqLockGuard<kernel::lib::SpinLock> guard(lock_);

		if ((virt % page_size) != 0)
		{
			return false;
		}

		const uint64_t pml4_i = pml4_index(virt);
		const uint64_t pdpt_i = pdpt_index(virt);
		const uint64_t pd_i = pd_index(virt);
		const uint64_t pt_i = pt_index(virt);

		auto* pml4 = table_virt(pml4_phys_);
		const uint64_t pml4e = pml4[pml4_i];
		if (!entry_present(pml4e) || entry_is_large(pml4e))
		{
			return false;
		}

		const uint64_t pdpt_phys = entry_addr(pml4e);
		auto* pdpt = table_virt(pdpt_phys);
		const uint64_t pdpte = pdpt[pdpt_i];
		if (!entry_present(pdpte) || entry_is_large(pdpte))
		{
			return false;
		}

		const uint64_t pd_phys = entry_addr(pdpte);
		auto* pd = table_virt(pd_phys);
		const uint64_t pde = pd[pd_i];
		if (!entry_present(pde))
		{
			return false;
		}

		if (entry_is_large(pde))
		{
			pd[pd_i] = 0;
			kernel::arch::x86_64::tlb::shootdown_page(pml4_phys_, virt);

			if (table_empty(pd_phys))
			{
				remove_table_if_empty(pdpt_phys, pdpt_i);
			}

			if (table_empty(pdpt_phys))
			{
				remove_table_if_empty(pml4_phys_, pml4_i);
			}

			return true;
		}

		const uint64_t pt_phys = entry_addr(pde);
		auto* pt = table_virt(pt_phys);
		const uint64_t pte = pt[pt_i];
		if (!entry_present(pte))
		{
			return false;
		}

		pt[pt_i] = 0;
		kernel::arch::x86_64::tlb::shootdown_page(pml4_phys_, virt);

		if (table_empty(pt_phys))
		{
			remove_table_if_empty(pd_phys, pd_i);
		}

		if (table_empty(pd_phys))
		{
			remove_table_if_empty(pdpt_phys, pdpt_i);
		}

		if (table_empty(pdpt_phys))
		{
			remove_table_if_empty(pml4_phys_, pml4_i);
		}

		return true;
	}

	uint64_t AddressSpace::translate(uint64_t virt) const noexcept
	{
		kernel::lib::IrqLockGuard<kernel::lib::SpinLock> guard(lock_);

		const uint64_t offset = virt & (page_size - 1);

		const uint64_t pml4_i = pml4_index(virt);
		const uint64_t pdpt_i = pdpt_index(virt);
		const uint64_t pd_i = pd_index(virt);
		const uint64_t pt_i = pt_index(virt);

		const auto* pml4 = table_virt_const(pml4_phys_);
		const uint64_t pml4e = pml4[pml4_i];
		if (!entry_present(pml4e))
		{
			return 0;
		}

		const uint64_t pdpt_phys = entry_addr(pml4e);
		const auto* pdpt = table_virt_const(pdpt_phys);
		const uint64_t pdpte = pdpt[pdpt_i];
		if (!entry_present(pdpte))
		{
			return 0;
		}

		if (entry_is_large(pdpte))
		{
			const uint64_t base = entry_addr(pdpte);
			const uint64_t page_off = virt & ((1ull << 30) - 1);
			return base + page_off;
		}

		const uint64_t pd_phys = entry_addr(pdpte);
		const auto* pd = table_virt_const(pd_phys);
		const uint64_t pde = pd[pd_i];
		if (!entry_present(pde))
		{
			return 0;
		}

		if (entry_is_large(pde))
		{
			const uint64_t base = entry_addr(pde);
			const uint64_t page_off = virt & ((1ull << 21) - 1);
			return base + page_off;
		}

		const uint64_t pt_phys = entry_addr(pde);
		const auto* pt = table_virt_const(pt_phys);
		const uint64_t pte = pt[pt_i];
		if (!entry_present(pte))
		{
			return 0;
		}

		const uint64_t phys = entry_addr(pte);
		return phys + offset;
	}

	void AddressSpace::activate() const noexcept
	{
		kernel::arch::x86_64::write_cr3(pml4_phys_ & addr_mask);
	}

	void init() noexcept
	{
		kernel::arch::x86_64::tlb::init();

		const uint64_t current_pml4 = kernel::arch::x86_64::read_cr3() & addr_mask;
		const uint64_t new_pml4 = alloc_table_page();
		if (new_pml4 == 0)
		{
			kernel::log::write_line("vmm pml4 alloc failed");
			kernel::arch::x86_64::halt_forever();
		}

		const auto* current = table_virt_const(current_pml4);
		auto* next = table_virt(new_pml4);

		next[0] = current[0];
		next[511] = current[511];

		kernel_as.reset(new_pml4 & addr_mask);
		kernel_as.activate();

		kernel::log::write("vmm cr3 switched pml4=");
		kernel::log::write_u64_hex(new_pml4);
		kernel::log::write("\n", 1);
	}

	AddressSpace& kernel_space() noexcept
	{
		return kernel_as;
	}

	bool map_range(uint64_t virt, uint64_t phys, uint64_t size, PageFlags flags) noexcept
	{
		if (size == 0)
		{
			return true;
		}

		uint64_t v = virt;
		uint64_t p = phys;
		uint64_t remaining = size;

		while (remaining != 0)
		{
			if ((v % large_page_size) == 0 && (p % large_page_size) == 0 && remaining >= large_page_size)
			{
				if (!map_large_2m(kernel_as, v, p, flags))
				{
					return false;
				}

				v += large_page_size;
				p += large_page_size;
				remaining -= large_page_size;
				continue;
			}

			if (!kernel_as.map_page(v, p, flags))
			{
				return false;
			}

			v += page_size;
			p += page_size;
			remaining -= remaining >= page_size ? page_size : remaining;
		}

		return true;
	}

	void map_physmap_all(const kernel::boot::multiboot2::Reader& multiboot) noexcept
	{
		const auto count = multiboot.memory_map_entry_count();
		const auto* entries = multiboot.memory_map_entries();
		const auto entry_size = multiboot.memory_map_entry_size();

		uint64_t mapped_max = 0;

		for (size_t i = 0; i < count; ++i)
		{
			const auto* e = reinterpret_cast<const kernel::boot::multiboot2::MemoryMapEntry*>(
				reinterpret_cast<const uint8_t*>(entries) + i * entry_size
			);

			if (e->type != static_cast<uint32_t>(kernel::boot::multiboot2::MemoryType::Available))
			{
				continue;
			}

			const uint64_t start = e->addr;
			const uint64_t end = e->addr + e->len;
			if (end <= start)
			{
				continue;
			}

			const uint64_t clipped_start = start < early_mapped_limit ? early_mapped_limit : start;
			if (end <= clipped_start)
			{
				continue;
			}

			const uint64_t v = kernel::mm::physmap::base + clipped_start;
			const uint64_t p = clipped_start;
			const uint64_t len = end - clipped_start;

			if (!map_range(v, p, len, PageFlags::Writable | PageFlags::NoExecute))
			{
				kernel::log::write_line("vmm physmap expand failed");
				kernel::arch::x86_64::halt_forever();
			}

			if (end > mapped_max)
			{
				mapped_max = end;
			}
		}

		kernel::log::write("physmap expanded max=");
		kernel::log::write_u64_hex(mapped_max);
		kernel::log::write("\n", 1);
	}
}
