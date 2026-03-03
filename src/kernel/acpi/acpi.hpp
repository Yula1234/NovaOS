#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kernel::boot::multiboot2
{
	class Reader;
}

namespace kernel::acpi
{
	struct MadtInfo
	{
		uint64_t lapic_phys;
		uint64_t ioapic_phys;
		uint32_t ioapic_gsi_base;

		uint32_t irq0_gsi;
		uint16_t irq0_flags;
	};

	struct HpetInfo
	{
		uint64_t hpet_phys;
	};

	bool init(const kernel::boot::multiboot2::Reader& multiboot) noexcept;

	const MadtInfo* madt() noexcept;
	const HpetInfo* hpet() noexcept;
}
