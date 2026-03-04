#pragma once

#include <stdint.h>
#include "lib/lock.hpp"

namespace kernel::arch::x86_64
{
	struct CpuLocalData
	{
		uint32_t cpu_id;
		uint32_t apic_id;
		uint64_t kernel_stack;
		uint64_t current_thread;
		void* per_cpu_heap_cache;
		kernel::lib::McsNode mcs_node;
		kernel::lib::McsNode mcs_node2;
	};

	static_assert(sizeof(CpuLocalData) <= 320, "CpuLocalData must fit in five cache lines");

	namespace cpu_local
	{
		void init_bsp() noexcept;

		void init_ap(uint32_t apic_id) noexcept;

		CpuLocalData* get() noexcept;

		uint32_t cpu_id() noexcept;

		uint32_t apic_id() noexcept;

		void set_kernel_stack(uint64_t stack) noexcept;

		void set_current_thread(uint64_t thread_ptr) noexcept;

		kernel::lib::McsNode* mcs_node() noexcept;

		kernel::lib::McsNode* mcs_node2() noexcept;
	}
}
