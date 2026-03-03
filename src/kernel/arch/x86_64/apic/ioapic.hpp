#pragma once

#include <stdint.h>

namespace kernel::arch::x86_64::apic::ioapic
{
	void init(uint64_t ioapic_phys, uint32_t gsi_base) noexcept;

	void route_irq(uint32_t gsi, uint8_t vector, uint32_t dest_apic_id, uint16_t flags) noexcept;
}
