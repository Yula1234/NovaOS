#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kernel::boot::multiboot2
{
	constexpr uint32_t bootloader_magic = 0x36D76289;

	struct Info
	{
		uint32_t magic;
		uint32_t mbi_address;
	};

	enum class TagType : uint32_t
	{
		End = 0,
		BootCommandLine = 1,
		BootLoaderName = 2,
		BasicMeminfo = 4,
		MemoryMap = 6,
		Framebuffer = 8,
	};

	struct [[gnu::packed]] Tag
	{
		uint32_t type;
		uint32_t size;
	};

	struct [[gnu::packed]] MemoryMapTag
	{
		Tag tag;
		uint32_t entry_size;
		uint32_t entry_version;
	};

	enum class MemoryType : uint32_t
	{
		Available = 1,
		Reserved = 2,
		AcpiReclaimable = 3,
		Nvs = 4,
		BadRam = 5,
	};

	struct [[gnu::packed]] MemoryMapEntry
	{
		uint64_t addr;
		uint64_t len;
		uint32_t type;
		uint32_t zero;
	};

	class Reader
	{
	public:
		explicit Reader(uint32_t mbi_address) noexcept;

		const Tag* find(TagType type) const noexcept;
		const MemoryMapTag* memory_map() const noexcept;

		size_t memory_map_entry_count() const noexcept;
		const MemoryMapEntry* memory_map_entries() const noexcept;
		uint32_t memory_map_entry_size() const noexcept;

	private:
		const uint8_t* base_;
		uint32_t total_size_;
	};
}
