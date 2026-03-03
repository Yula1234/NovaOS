#pragma once

#include <stdint.h>

namespace kernel::arch::x86_64
{
	struct [[gnu::packed]] Idtr
	{
		uint16_t limit;
		uint64_t base;
	};

	inline void lidt(const Idtr& idtr) noexcept
	{
		asm volatile("lidt %0" : : "m"(idtr));
	}

	inline void sti() noexcept
	{
		asm volatile("sti");
	}

	inline void cli() noexcept
	{
		asm volatile("cli");
	}

	[[noreturn]] inline void halt_forever() noexcept
	{
		for (;;)
		{
			asm volatile("cli");
			asm volatile("hlt");
		}
	}

	inline uint64_t read_cr2() noexcept
	{
		uint64_t value;
		asm volatile("mov %%cr2, %0" : "=r"(value));
		return value;
	}

	inline uint64_t read_cr3() noexcept
	{
		uint64_t value;
		asm volatile("mov %%cr3, %0" : "=r"(value));
		return value;
	}

	inline void write_cr3(uint64_t value) noexcept
	{
		asm volatile("mov %0, %%cr3" : : "r"(value) : "memory");
	}

	inline void invlpg(const void* address) noexcept
	{
		asm volatile("invlpg (%0)" : : "r"(address) : "memory");
	}
}
