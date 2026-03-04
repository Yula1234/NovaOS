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

	inline bool interrupts_enabled() noexcept
	{
		uint64_t rflags;
		asm volatile(
			"pushfq\n"
			"pop %0\n"
			: "=r"(rflags)
		);

		return (rflags & (1ull << 9)) != 0;
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

	inline uint64_t rdtsc() noexcept
	{
		uint32_t lo = 0;
		uint32_t hi = 0;
		asm volatile(
			"lfence\n"
			"rdtsc\n"
			: "=a"(lo), "=d"(hi)
			:
			: "memory"
		);

		return (static_cast<uint64_t>(hi) << 32) | lo;
	}

	inline void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t& eax, uint32_t& ebx, uint32_t& ecx, uint32_t& edx) noexcept
	{
		asm volatile(
			"cpuid"
			: "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
			: "a"(leaf), "c"(subleaf)
		);
	}

	inline uint64_t rdmsr(uint32_t msr) noexcept
	{
		uint32_t lo;
		uint32_t hi;
		asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
		return (static_cast<uint64_t>(hi) << 32) | lo;
	}

	inline void wrmsr(uint32_t msr, uint64_t value) noexcept
	{
		const uint32_t lo = static_cast<uint32_t>(value);
		const uint32_t hi = static_cast<uint32_t>(value >> 32);
		asm volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
	}

	inline bool nx_supported() noexcept
	{
		uint32_t eax;
		uint32_t ebx;
		uint32_t ecx;
		uint32_t edx;

		cpuid(0x80000000u, 0, eax, ebx, ecx, edx);
		if (eax < 0x80000001u)
		{
			return false;
		}

		cpuid(0x80000001u, 0, eax, ebx, ecx, edx);
		return (edx & (1u << 20)) != 0;
	}

	inline void enable_nx() noexcept
	{
		if (!nx_supported())
		{
			return;
		}

		constexpr uint32_t ia32_efer = 0xC0000080u;
		constexpr uint64_t efer_nxe = 1ull << 11;

		const uint64_t efer = rdmsr(ia32_efer);
		wrmsr(ia32_efer, efer | efer_nxe);
	}
}
