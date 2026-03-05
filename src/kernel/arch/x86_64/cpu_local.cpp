#include "kernel/arch/x86_64/cpu_local.hpp"

#include "kernel/arch/x86_64/cpu.hpp"
#include "kernel/mm/heap.hpp"

namespace
{
	/* x86-64 GS base MSRs. We use plain GS base (no SWAPGS yet), so KERNEL_GS_BASE stays zero. */
	constexpr uint32_t msr_gs_base = 0xC0000101u;
	constexpr uint32_t msr_kernel_gs_base = 0xC0000102u;
	constexpr uint64_t per_cpu_struct_size = 64;

	/* BSP gets a fixed, early storage block so cpu_local is usable before the heap is up. */
	alignas(64) static uint8_t bsp_cpu_local_storage[per_cpu_struct_size];
	constexpr uint32_t max_cpus = 256;

	void write_gs_base(uint64_t value) noexcept
	{
		kernel::arch::x86_64::wrmsr(msr_gs_base, value);
	}

	void write_kernel_gs_base(uint64_t value) noexcept
	{
		kernel::arch::x86_64::wrmsr(msr_kernel_gs_base, value);
	}

	kernel::arch::x86_64::CpuLocalData* allocate_cpu_local(uint32_t cpu_id) noexcept
	{
		/* Per-CPU block is intentionally cacheline-sized and aligned; don't let the allocator pick odd alignments. */
		void* mem = kernel::mm::heap::alloc(per_cpu_struct_size);
		if (!mem)
		{
			return nullptr;
		}

		auto* data = static_cast<kernel::arch::x86_64::CpuLocalData*>(mem);
		data->cpu_id = cpu_id;
		data->apic_id = 0;
		data->kernel_stack = 0;
		data->current_thread = 0;
		data->per_cpu_heap_cache = nullptr;
		data->irq_depth = 0;
		data->irq_prev_if = 0;

		return data;
	}
}

namespace kernel::arch::x86_64::cpu_local
{
	void init_bsp() noexcept
	{
		/* BSP uses static storage; APs allocate their block from the heap once it is available. */
		auto* data = reinterpret_cast<CpuLocalData*>(bsp_cpu_local_storage);
		data->cpu_id = 0;
		data->apic_id = 0;
		data->kernel_stack = 0;
		data->current_thread = 0;
		data->per_cpu_heap_cache = nullptr;
		data->irq_depth = 0;
		data->irq_prev_if = 0;

		write_gs_base(reinterpret_cast<uint64_t>(data));
		write_kernel_gs_base(0);
	}

	void init_ap(uint32_t apic_id) noexcept
	{
		/* Current code keeps a trivial mapping cpu_id == low 8 bits of APIC ID (xAPIC). */
		/* If you ever support sparse APIC IDs or x2APIC, this will need a real allocator/map. */
		const uint32_t cpu_id = apic_id & 0xFFu;

		auto* data = allocate_cpu_local(cpu_id);
		if (!data)
		{
			for (;;)
			{
				asm volatile("hlt");
			}
		}

		data->apic_id = apic_id;

		write_gs_base(reinterpret_cast<uint64_t>(data));
		write_kernel_gs_base(0);
	}

	CpuLocalData* get() noexcept
	{
		constexpr uint32_t ia32_gs_base = 0xC0000101u;
		return reinterpret_cast<CpuLocalData*>(kernel::arch::x86_64::rdmsr(ia32_gs_base));
	}

	uint32_t cpu_id() noexcept
	{
		uint32_t id;
		asm volatile("mov %%gs:0, %0" : "=r"(id));
		return id;
	}

	uint32_t apic_id() noexcept
	{
		uint32_t id;
		asm volatile("mov %%gs:4, %0" : "=r"(id));
		return id;
	}

	void set_kernel_stack(uint64_t stack) noexcept
	{
		asm volatile("mov %0, %%gs:16" : : "r"(stack) : "memory");
	}

	void set_current_thread(uint64_t thread_ptr) noexcept
	{
		asm volatile("mov %0, %%gs:24" : : "r"(thread_ptr) : "memory");
	}
}
