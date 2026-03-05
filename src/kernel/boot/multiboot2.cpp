#include "kernel/boot/multiboot2.hpp"

#include "lib/align.hpp"
#include "kernel/mm/physmap.hpp"

namespace kernel::boot::multiboot2
{
	Reader::Reader(uint32_t mbi_address) noexcept
		: base_(reinterpret_cast<const uint8_t*>(kernel::mm::physmap::to_virt_const(mbi_address)))
		, total_size_(0)
	{
		/* MBI lives in physical memory; physmap gives us a stable direct mapping for parsing. */
		if (!base_)
		{
			return;
		}

		total_size_ = *reinterpret_cast<const uint32_t*>(base_ + 0);
	}

	const Tag* Reader::find(TagType type) const noexcept
	{
		if (!base_ || total_size_ < 16)
		{
			return nullptr;
		}

		const uint8_t* const end = base_ + total_size_;
		const uint8_t* p = base_ + 8;

		/* Multiboot2 tags start at offset 8 and are rounded up to 8-byte alignment. */
		while (p + sizeof(Tag) <= end)
		{
			const auto* tag = reinterpret_cast<const Tag*>(p);
			if (tag->size < sizeof(Tag))
			{
				return nullptr;
			}

			if (tag->type == static_cast<uint32_t>(type))
			{
				return tag;
			}

			if (tag->type == static_cast<uint32_t>(TagType::End))
			{
				return nullptr;
			}

			const uint32_t step = kernel::lib::align_up(tag->size, 8u);
			p += step;
		}

		return nullptr;
	}

	const MemoryMapTag* Reader::memory_map() const noexcept
	{
		return reinterpret_cast<const MemoryMapTag*>(find(TagType::MemoryMap));
	}

	size_t Reader::memory_map_entry_count() const noexcept
	{
		const auto* m = memory_map();
		if (!m)
		{
			return 0;
		}

		if (m->entry_size < sizeof(MemoryMapEntry))
		{
			return 0;
		}

		const uint32_t header_size = static_cast<uint32_t>(sizeof(MemoryMapTag));
		if (m->tag.size < header_size)
		{
			return 0;
		}

		const uint32_t payload = m->tag.size - header_size;
		return payload / m->entry_size;
	}

	const MemoryMapEntry* Reader::memory_map_entries() const noexcept
	{
		const auto* m = memory_map();
		if (!m)
		{
			return nullptr;
		}

		return reinterpret_cast<const MemoryMapEntry*>(reinterpret_cast<const uint8_t*>(m) + sizeof(MemoryMapTag));
	}

	uint32_t Reader::memory_map_entry_size() const noexcept
	{
		const auto* m = memory_map();
		return m ? m->entry_size : 0;
	}

	uint64_t Reader::acpi_rsdp_phys() const noexcept
	{
		/* Prefer the ACPI v2+ tag when present; fall back to v1.0 tag otherwise. */
		const auto* tag_new = find(TagType::AcpiNew);
		const auto* tag_old = find(TagType::AcpiOld);
		const auto* tag = tag_new ? tag_new : tag_old;
		if (!tag)
		{
			return 0;
		}

		const auto* rsdp_virt = reinterpret_cast<const uint8_t*>(tag) + sizeof(Tag);
		return kernel::mm::physmap::to_phys(rsdp_virt);
	}
}
