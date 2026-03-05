#pragma once

#include <stddef.h>
#include <stdint.h>
#include "lib/lock.hpp"

namespace kernel::arch::x86_64
{
	struct CpuLocalData
	{
		/* This struct is addressed via GS base; offsets are part of a tiny ABI used by asm fast paths. */
		uint32_t cpu_id;
		uint32_t apic_id;
		/* Top of the current kernel stack for this CPU (used by low-level entry paths). */
		uint64_t kernel_stack;
		uint64_t current_thread;
		/* Opaque pointer to per-CPU allocator state (slab/heap caches). */
		void* per_cpu_heap_cache;
		/* Nesting counter for irq_save_disable-style sections. */
		uint32_t irq_depth;
		/* Previous IF value captured on first disable; used to restore correctly on unwind. */
		uint8_t irq_prev_if;
		uint8_t padding_0[3];
	};

	/* Keep the whole hot per-CPU block in one cache line to avoid false sharing. */
	static_assert(sizeof(CpuLocalData) <= 64, "CpuLocalData must fit in one cache line");
	static_assert(offsetof(CpuLocalData, cpu_id) == 0);
	static_assert(offsetof(CpuLocalData, apic_id) == 4);
	static_assert(offsetof(CpuLocalData, kernel_stack) == 8);
	static_assert(offsetof(CpuLocalData, current_thread) == 16);
	static_assert(offsetof(CpuLocalData, per_cpu_heap_cache) == 24);
	static_assert(offsetof(CpuLocalData, irq_depth) == 32);
	static_assert(offsetof(CpuLocalData, irq_prev_if) == 36);

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
