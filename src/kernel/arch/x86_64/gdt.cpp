#include "kernel/arch/x86_64/gdt.hpp"

#include <stddef.h>

namespace
{
	struct [[gnu::packed]] Gdtr
	{
		uint16_t limit;
		uint64_t base;
	};

	struct [[gnu::packed]] Tss64
	{
		uint32_t reserved0;
		uint64_t rsp0;
		uint64_t rsp1;
		uint64_t rsp2;
		uint64_t reserved1;
		uint64_t ist1;
		uint64_t ist2;
		uint64_t ist3;
		uint64_t ist4;
		uint64_t ist5;
		uint64_t ist6;
		uint64_t ist7;
		uint64_t reserved2;
		uint16_t reserved3;
		uint16_t iomap_base;
	};

	static_assert(sizeof(Tss64) == 104);

	constexpr size_t max_cpus = 256;
	constexpr size_t gdt_entries = 7;
	constexpr size_t ist_stack_size = 4096;

	alignas(16) uint64_t gdt_table[max_cpus][gdt_entries]{};
	alignas(16) Tss64 tss_table[max_cpus]{};

	alignas(16) uint8_t ist1_stack[max_cpus][ist_stack_size]{};
	alignas(16) uint8_t ist2_stack[max_cpus][ist_stack_size]{};
	alignas(16) uint8_t ist3_stack[max_cpus][ist_stack_size]{};

	uint64_t read_rsp() noexcept
	{
		uint64_t value = 0;
		asm volatile("mov %%rsp, %0" : "=r"(value));
		return value;
	}

	uint64_t stack_top(uint8_t* stack, size_t size) noexcept
	{
		return reinterpret_cast<uint64_t>(stack + size);
	}

	uint64_t make_segment(uint64_t value) noexcept
	{
		return value;
	}

	void set_tss_descriptor(uint64_t* gdt, uint16_t selector, uint64_t base, uint32_t limit) noexcept
	{
		const uint16_t index = static_cast<uint16_t>(selector >> 3);
		const uint64_t type = 0x9ull;
		const uint64_t present = 1ull;
		const uint64_t access = (present << 7) | type;
		const uint64_t flags = 0ull;

		uint64_t low = 0;
		low |= (limit & 0xFFFFull);
		low |= (base & 0xFFFFFFull) << 16;
		low |= (access & 0xFFull) << 40;
		low |= ((limit >> 16) & 0xFull) << 48;
		low |= (flags & 0xFull) << 52;
		low |= ((base >> 24) & 0xFFull) << 56;

		uint64_t high = 0;
		high |= (base >> 32) & 0xFFFFFFFFull;

		gdt[index] = low;
		gdt[index + 1] = high;
	}

	void load_gdt_and_segments(const Gdtr& gdtr) noexcept
	{
		asm volatile("lgdt %0" : : "m"(gdtr) : "memory");

		asm volatile(
			"pushq %[cs]\n"
			"lea 1f(%%rip), %%rax\n"
			"pushq %%rax\n"
			"lretq\n"
			"1:\n"
			:
			: [cs] "i"(kernel::arch::x86_64::gdt::selectors::kernel_code)
			: "rax", "memory"
		);

		asm volatile(
			"mov %[ds], %%ax\n"
			"mov %%ax, %%ds\n"
			"mov %%ax, %%es\n"
			"mov %%ax, %%ss\n"
			"mov %%ax, %%fs\n"
			"mov %%ax, %%gs\n"
			:
			: [ds] "i"(kernel::arch::x86_64::gdt::selectors::kernel_data)
			: "rax", "memory"
		);
	}

	void load_tr(uint16_t selector) noexcept
	{
		asm volatile("ltr %0" : : "r"(selector) : "memory");
	}
}

namespace kernel::arch::x86_64::gdt
{
	void init_cpu(uint32_t cpu_slot, uint64_t rsp0) noexcept
	{
		if (cpu_slot >= max_cpus)
		{
			return;
		}

		if (rsp0 == 0)
		{
			rsp0 = read_rsp();
		}

		auto& gdt = gdt_table[cpu_slot];
		auto& tss = tss_table[cpu_slot];

		gdt[0] = 0;
		gdt[1] = make_segment(0x00AF9A000000FFFFull);
		gdt[2] = make_segment(0x00AF92000000FFFFull);
		gdt[3] = make_segment(0x00AFFA000000FFFFull);
		gdt[4] = make_segment(0x00AFF2000000FFFFull);
		gdt[5] = 0;
		gdt[6] = 0;

		tss = {};
		tss.rsp0 = rsp0;
		tss.ist1 = stack_top(&ist1_stack[cpu_slot][0], ist_stack_size);
		tss.ist2 = stack_top(&ist2_stack[cpu_slot][0], ist_stack_size);
		tss.ist3 = stack_top(&ist3_stack[cpu_slot][0], ist_stack_size);
		tss.iomap_base = sizeof(Tss64);

		set_tss_descriptor(&gdt[0], selectors::tss, reinterpret_cast<uint64_t>(&tss), sizeof(Tss64) - 1);

		const Gdtr gdtr{
			.limit = static_cast<uint16_t>(sizeof(gdt) - 1),
			.base = reinterpret_cast<uint64_t>(&gdt[0]),
		};

		load_gdt_and_segments(gdtr);
		load_tr(selectors::tss);
	}

	void init_bsp() noexcept
	{
		init_cpu(0, 0);
	}
}
