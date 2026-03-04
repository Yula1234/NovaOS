#pragma once

#include <stdint.h>

namespace kernel::arch::x86_64
{
	struct CpuLocalData
	{
		uint32_t cpu_id;
		uint32_t apic_id;
		uint64_t kernel_stack;
		uint64_t current_thread;
		void* per_cpu_heap_cache;
	};

	static_assert(sizeof(CpuLocalData) <= 64, "CpuLocalData must fit in single cache line");

	namespace cpu_local
	{
		void init_bsp() noexcept;

		void init_ap(uint32_t apic_id) noexcept;

		CpuLocalData* get() noexcept;

		uint32_t cpu_id() noexcept;

		uint32_t apic_id() noexcept;

		void set_kernel_stack(uint64_t stack) noexcept;

		void set_current_thread(uint64_t thread_ptr) noexcept;
	}
}
