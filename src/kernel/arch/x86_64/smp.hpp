#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/acpi/acpi.hpp"

namespace kernel::arch::x86_64::smp
{
	struct Cpu
	{
		uint8_t apic_id;
		uint8_t acpi_uid;
		bool enabled;
	};

	bool available() noexcept;

	size_t cpu_count() noexcept;
	const Cpu* cpus() noexcept;

	bool init(const kernel::acpi::MadtInfo& madt) noexcept;
}
