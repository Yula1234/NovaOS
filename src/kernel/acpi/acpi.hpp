#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kernel::boot::multiboot2
{
	class Reader;
}

namespace kernel::acpi
{
	struct CpuInfo
	{
		/* APIC ID used for IPI targeting; comes from MADT Local APIC entries. */
		uint8_t apic_id;
		/* Firmware-provided processor UID (ACPI Processor ID); stable identifier across boots. */
		uint8_t acpi_uid;
		bool enabled;
	};

	struct MadtInfo
	{
		uint64_t lapic_phys;
		uint64_t ioapic_phys;
		uint32_t ioapic_gsi_base;

		/* IRQ0 remap from legacy PIC space to a GSI; used for the timer interrupt routing. */
		uint32_t irq0_gsi;
		uint16_t irq0_flags;

		const CpuInfo* cpus;
		size_t cpu_count;
	};

	struct HpetInfo
	{
		uint64_t hpet_phys;
	};

	bool init(const kernel::boot::multiboot2::Reader& multiboot) noexcept;

	const MadtInfo* madt() noexcept;
	const HpetInfo* hpet() noexcept;
}
