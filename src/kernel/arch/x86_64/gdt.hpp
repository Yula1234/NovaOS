#pragma once

#include <stdint.h>

namespace kernel::arch::x86_64::gdt
{
	namespace selectors
	{
		/* These selectors match the layout filled in gdt.cpp; keep them in sync. */
		constexpr uint16_t kernel_code = 0x08;
		constexpr uint16_t kernel_data = 0x10;
		constexpr uint16_t user_data = 0x20;
		constexpr uint16_t user_code = 0x18;
		constexpr uint16_t tss = 0x28;
	}

	void init_bsp() noexcept;
	/* rsp0 is the ring0 stack pointer loaded by the CPU on CPL3->CPL0 transitions. */
	void init_cpu(uint32_t cpu_slot, uint64_t rsp0) noexcept;
}
