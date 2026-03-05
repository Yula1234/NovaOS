#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/acpi/acpi.hpp"

namespace kernel::arch::x86_64::smp
{
	struct Cpu
	{
		/* LAPIC ID used for IPI targeting and SIPI bring-up. */
		uint8_t apic_id;
		/* ACPI processor UID, useful for matching firmware objects. */
		uint8_t acpi_uid;
		bool enabled;
	};

	bool available() noexcept;

	size_t cpu_count() noexcept;
	const Cpu* cpus() noexcept;

	/* Builds CPU list from MADT and brings up APs via INIT/SIPI. */
	bool init(const kernel::acpi::MadtInfo& madt) noexcept;
}
