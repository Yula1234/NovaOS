#include "kernel/arch/x86_64/tlb.hpp"

#include <atomic>

#include "kernel/arch/x86_64/apic/lapic.hpp"
#include "kernel/arch/x86_64/cpu.hpp"
#include "kernel/arch/x86_64/smp.hpp"
#include "lib/lock.hpp"

namespace
{
	constexpr uint64_t cr3_mask = 0x000FFFFFFFFFF000ull;

	kernel::lib::McsLock shoot_lock;

	std::atomic<uint64_t> shoot_seq{0};
	std::atomic<uint32_t> shoot_acks{0};

	std::atomic<uint64_t> shoot_target_cr3{0};
	std::atomic<uint64_t> shoot_virt{0};

	uint64_t last_handled_seq[256]{};

	uint32_t cpu_expected_acks() noexcept
	{
		const size_t n = kernel::arch::x86_64::smp::cpu_count();
		return n > 0 ? static_cast<uint32_t>(n - 1) : 0;
	}

	void handle_shootdown() noexcept
	{
		const uint32_t apic_id = kernel::arch::x86_64::apic::lapic::id() & 0xFFu;

		const uint64_t seq = shoot_seq.load(std::memory_order_acquire);
		if (seq == 0 || last_handled_seq[apic_id] == seq)
		{
			return;
		}

		last_handled_seq[apic_id] = seq;

		const uint64_t target_cr3 = shoot_target_cr3.load(std::memory_order_relaxed);
		const uint64_t virt = shoot_virt.load(std::memory_order_relaxed);

		const uint64_t cur_cr3 = kernel::arch::x86_64::read_cr3() & cr3_mask;
		if (cur_cr3 == (target_cr3 & cr3_mask))
		{
			kernel::arch::x86_64::invlpg(reinterpret_cast<void*>(virt));
		}

		shoot_acks.fetch_add(1, std::memory_order_release);
	}
}

namespace kernel::arch::x86_64::tlb
{
	void init() noexcept
	{
		(void)0;
	}

	void on_nmi() noexcept
	{
		handle_shootdown();
		kernel::arch::x86_64::apic::lapic::eoi();
	}

	void shootdown_page(uint64_t target_cr3_phys, uint64_t virt) noexcept
	{
		if (!kernel::arch::x86_64::apic::lapic::available())
		{
			kernel::arch::x86_64::invlpg(reinterpret_cast<void*>(virt));
			return;
		}

		const uint32_t expected = cpu_expected_acks();
		if (expected == 0)
		{
			kernel::arch::x86_64::invlpg(reinterpret_cast<void*>(virt));
			return;
		}

		kernel::lib::IrqMcsLockGuard guard(shoot_lock);

		shoot_target_cr3.store(target_cr3_phys & cr3_mask, std::memory_order_relaxed);
		shoot_virt.store(virt, std::memory_order_relaxed);

		shoot_acks.store(0, std::memory_order_relaxed);
		shoot_seq.fetch_add(1, std::memory_order_release);

		kernel::arch::x86_64::apic::lapic::broadcast_nmi(false);
		kernel::arch::x86_64::invlpg(reinterpret_cast<void*>(virt));

		while (shoot_acks.load(std::memory_order_acquire) < expected)
		{
			asm volatile("pause");
		}
	}
}
