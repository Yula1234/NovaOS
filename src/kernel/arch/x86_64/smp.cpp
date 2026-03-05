#include "kernel/arch/x86_64/smp.hpp"

#include <atomic>
#include <stdint.h>

#include "kernel/arch/x86_64/apic/lapic.hpp"
#include "kernel/arch/x86_64/apic/timer.hpp"
#include "kernel/arch/x86_64/cpu.hpp"
#include "kernel/arch/x86_64/cpu_local.hpp"
#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/log/log.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/physmap.hpp"
#include "kernel/mm/vmm.hpp"
#include "kernel/time/time.hpp"

#include "string.h"

extern "C" uint8_t ap_trampoline_start;
extern "C" uint8_t ap_trampoline_end;
extern "C" uint8_t ap_trampoline_mailbox;
extern "C" uint8_t ap_trampoline_gdt;
extern "C" uint8_t ap_trampoline_gdt_ptr;

extern "C"
{
	uint64_t smp_ap_mailbox_ptrs[256]{};
}

namespace
{
	constexpr uint64_t ap_lowmem_limit = 0x100000ull;
	constexpr uint64_t page_size = 4096;

	/*
	 * Shared mailbox between BSP and the AP trampoline.
	 * It lives inside the trampoline page and is addressed via physical memory during early AP bring-up.
	 */
	struct alignas(8) TrampolineMailbox
	{
		uint64_t cr3;
		uint64_t rsp;
		uint64_t entry;
		uint64_t ready_ptr;
		uint32_t stage;
		uint32_t _pad;
		uint64_t trampoline_phys;
		uint64_t go_ptr;
	};

	constexpr size_t max_cpus = 256;

	kernel::arch::x86_64::smp::Cpu cpu_list[max_cpus]{};
	size_t cpu_count = 0;
	bool smp_available = false;

	uint64_t trampoline_phys = 0;
	uint64_t trampoline_size = 0;
	uint64_t mailbox_off = 0;

	std::atomic<uint32_t> ap_ready[max_cpus]{};
	std::atomic<uint32_t> ap_go[max_cpus]{};

	uint64_t read_cr3_phys() noexcept
	{
		return kernel::arch::x86_64::read_cr3() & 0x000FFFFFFFFFF000ull;
	}

	bool setup_trampoline_image() noexcept
	{
		/* SIPI targets a 4KiB physical page below 1MiB. Temporarily clamp PMM allocations there. */
		const uint64_t saved_limit = kernel::mm::pmm::alloc_limit();
		kernel::mm::pmm::set_alloc_limit(ap_lowmem_limit);
		/*
		 * A few conventional low-memory slots. Some firmware leaves holes, so we probe a small list first.
		 * If it doesn't work, fall back to any lowmem page.
		 */
		constexpr uint64_t preferred_addrs[] = {
			0x7000,
			0x8000,
			0x9000,
			0xA000,
		};

		trampoline_phys = 0;
		for (const uint64_t addr : preferred_addrs)
		{
			trampoline_phys = kernel::mm::pmm::alloc_page_at(addr);
			if (trampoline_phys != 0)
			{
				break;
			}
		}

		if (trampoline_phys == 0)
		{
			trampoline_phys = kernel::mm::pmm::alloc_page();
		}
		kernel::mm::pmm::set_alloc_limit(saved_limit);

		if (trampoline_phys == 0)
		{
			kernel::log::write_line("smp trampoline alloc failed");
			return false;
		}

		const auto* start = &ap_trampoline_start;
		const auto* end = &ap_trampoline_end;
		trampoline_size = static_cast<uint64_t>(end - start);

		if (trampoline_size == 0 || trampoline_size > page_size)
		{
			kernel::log::write_line("smp trampoline size invalid");
			return false;
		}

		mailbox_off = static_cast<uint64_t>(&ap_trampoline_mailbox - &ap_trampoline_start);
		if (mailbox_off + sizeof(TrampolineMailbox) > trampoline_size)
		{
			kernel::log::write_line("smp trampoline mailbox invalid");
			return false;
		}

		const uint64_t gdt_off = static_cast<uint64_t>(&ap_trampoline_gdt - &ap_trampoline_start);
		const uint64_t gdt_ptr_off = static_cast<uint64_t>(&ap_trampoline_gdt_ptr - &ap_trampoline_start);
		if (gdt_off >= trampoline_size || gdt_ptr_off + 6 > trampoline_size)
		{
			kernel::log::write_line("smp trampoline gdt layout invalid");
			return false;
		}

		void* dst = kernel::mm::physmap::to_virt(trampoline_phys);
		memcpy(dst, start, static_cast<size_t>(trampoline_size));

		auto* image = static_cast<uint8_t*>(dst);
		const uint32_t gdt_base = static_cast<uint32_t>(trampoline_phys + gdt_off);
		*reinterpret_cast<uint32_t*>(image + gdt_ptr_off + 2) = gdt_base;

		const uint32_t seg_base = static_cast<uint32_t>(trampoline_phys);
		for (size_t i = 1; i <= 2; ++i)
		{
			auto* desc = reinterpret_cast<uint8_t*>(image + gdt_off + i * 8);
			desc[2] = static_cast<uint8_t>(seg_base);
			desc[3] = static_cast<uint8_t>(seg_base >> 8);
			desc[4] = static_cast<uint8_t>(seg_base >> 16);
			desc[7] = static_cast<uint8_t>(seg_base >> 24);
		}

		/*
		 * The AP executes with paging enabled (we load CR3 in the trampoline). To keep the trampoline
		 * simple we require an identity mapping for the trampoline page.
		 */
		auto& kas = kernel::mm::vmm::kernel_space();
		const uint64_t already = kas.translate(trampoline_phys);
		if (already != trampoline_phys)
		{
			const bool mapped = kas.map_page(
				trampoline_phys,
				trampoline_phys,
				kernel::mm::vmm::PageFlags::Writable
			);

			const uint64_t after = kas.translate(trampoline_phys);
			if (!mapped && after != trampoline_phys)
			{
				kernel::log::write_line("smp trampoline identity map failed");
				return false;
			}
		}
		return true;
	}

	extern "C" void ap_main(TrampolineMailbox* mailbox) noexcept;

	extern "C" void ap_main(TrampolineMailbox* mailbox) noexcept
	{
		const uint8_t apic_id = kernel::arch::x86_64::apic::lapic::id() & 0xFFu;

		kernel::arch::x86_64::cpu_local::init_ap(apic_id);

		if (mailbox)
		{
			mailbox->stage = 100;
		}

		kernel::arch::x86_64::enable_nx();
		if (mailbox)
		{
			mailbox->stage = 101;
		}

		kernel::arch::x86_64::apic::lapic::init_cpu();
		if (mailbox)
		{
			mailbox->stage = 102;
		}

		kernel::arch::x86_64::apic::timer::init_cpu();
		if (mailbox)
		{
			mailbox->stage = 103;
		}

		if (mailbox)
		{
			mailbox->stage = 104;
		}

		/* Exposed for diagnostics; useful when debugging AP hangs in early stages. */
		::smp_ap_mailbox_ptrs[apic_id] = reinterpret_cast<uint64_t>(mailbox);

		/*
		 * Two-phase AP start:
		 *  - AP sets *ready_ptr when it reached long mode and is about to jump into ap_main.
		 *  - AP waits for *go_ptr so BSP can finish global init before AP enables interrupts.
		 */
		const auto go = reinterpret_cast<std::atomic<uint32_t>*>(mailbox ? mailbox->go_ptr : 0);
		while (go && go->load(std::memory_order_acquire) == 0)
		{
			asm volatile("pause");
		}

		if (mailbox)
		{
			mailbox->stage = 106;
		}

		kernel::arch::x86_64::gdt::init_cpu(apic_id, mailbox ? mailbox->rsp : 0);

		kernel::arch::x86_64::idt::reload();

		kernel::arch::x86_64::sti();

		for (;;)
		{
			asm volatile("hlt");
		}
	}

	uint64_t alloc_stack_phys() noexcept
	{
		return kernel::mm::pmm::alloc_page();
	}

	uint64_t stack_top_virt(uint64_t stack_phys) noexcept
	{
		return reinterpret_cast<uint64_t>(kernel::mm::physmap::to_virt(stack_phys + page_size));
	}

	bool start_cpu(uint8_t apic_id) noexcept
	{
		const uint64_t stack_phys = alloc_stack_phys();
		if (stack_phys == 0)
		{
			kernel::log::write_line("smp ap stack alloc failed");
			return false;
		}

		/* ready/go are single-writer on BSP side; AP uses acquire loads. */
		ap_ready[apic_id].store(0, std::memory_order_relaxed);
		ap_go[apic_id].store(0, std::memory_order_relaxed);

		auto* mailbox = reinterpret_cast<TrampolineMailbox*>(
			reinterpret_cast<uint8_t*>(kernel::mm::physmap::to_virt(trampoline_phys)) + mailbox_off
		);

		mailbox->cr3 = read_cr3_phys();
		mailbox->rsp = stack_top_virt(stack_phys);
		mailbox->entry = reinterpret_cast<uint64_t>(&ap_main);
		mailbox->ready_ptr = reinterpret_cast<uint64_t>(&ap_ready[apic_id]);
		mailbox->stage = 0;
		mailbox->trampoline_phys = trampoline_phys;
		mailbox->go_ptr = reinterpret_cast<uint64_t>(&ap_go[apic_id]);

		/* SIPI vector is the page number; must be 4KiB aligned. */
		const uint8_t startup_vector = static_cast<uint8_t>(trampoline_phys / page_size);
		if ((trampoline_phys % page_size) != 0)
		{
			kernel::log::write_line("smp trampoline phys not aligned");
			return false;
		}

		/* INIT deassert+SIPI timing is deliberately conservative here. */
		kernel::arch::x86_64::apic::lapic::send_init_ipi_assert(apic_id);
		kernel::time::sleep_ms(10);
		kernel::arch::x86_64::apic::lapic::send_init_ipi_deassert(apic_id);
		kernel::time::sleep_ms(1);

		kernel::arch::x86_64::apic::lapic::send_startup_ipi(apic_id, startup_vector);
		kernel::time::sleep_ms(1);
		kernel::arch::x86_64::apic::lapic::send_startup_ipi(apic_id, startup_vector);

		const uint64_t deadline = kernel::time::monotonic::now_ns() + 1000ull * 1000ull * 1000ull;
		while (kernel::time::monotonic::available() && kernel::time::monotonic::now_ns() < deadline)
		{
			if (ap_ready[apic_id].load(std::memory_order_acquire) != 0)
			{
				return true;
			}

			asm volatile("pause");
		}

		const uint32_t last_ready = ap_ready[apic_id].load(std::memory_order_acquire);
		kernel::log::write("smp ap timeout apic_id=");
		kernel::log::write_u64_dec(apic_id);
		kernel::log::write(" ready=");
		kernel::log::write_u64_dec(last_ready);
		kernel::log::write(" vector=");
		kernel::log::write_u64_hex(startup_vector);
		kernel::log::write(" trampoline=");
		kernel::log::write_u64_hex(trampoline_phys);
		kernel::log::write(" cr3=");
		kernel::log::write_u64_hex(mailbox->cr3);
		kernel::log::write(" rsp=");
		kernel::log::write_u64_hex(mailbox->rsp);
		kernel::log::write(" entry=");
		kernel::log::write_u64_hex(mailbox->entry);
		kernel::log::write(" ready_ptr=");
		kernel::log::write_u64_hex(mailbox->ready_ptr);
		kernel::log::write(" stage=");
		kernel::log::write_u64_dec(mailbox->stage);
		kernel::log::write(" esr=");
		kernel::log::write_u64_hex(kernel::arch::x86_64::apic::lapic::read_esr());
		kernel::log::write(" icr_hi=");
		kernel::log::write_u64_hex(kernel::arch::x86_64::apic::lapic::read_icr_high());
		kernel::log::write(" icr_lo=");
		kernel::log::write_u64_hex(kernel::arch::x86_64::apic::lapic::read_icr_low());
		kernel::log::write("\n", 1);

		return last_ready != 0;
	}
}

namespace kernel::arch::x86_64::smp
{
	bool available() noexcept
	{
		return smp_available;
	}

	size_t cpu_count() noexcept
	{
		return ::cpu_count;
	}

	const Cpu* cpus() noexcept
	{
		return ::cpu_list;
	}

	bool init(const kernel::acpi::MadtInfo& madt) noexcept
	{
		::cpu_count = 0;
		::smp_available = false;

		if (madt.cpu_count == 0 || madt.cpus == nullptr)
		{
			return false;
		}

		for (size_t i = 0; i < madt.cpu_count && ::cpu_count < max_cpus; ++i)
		{
			const auto& in = madt.cpus[i];

			Cpu out{};
			out.apic_id = in.apic_id;
			out.acpi_uid = in.acpi_uid;
			out.enabled = in.enabled;

			cpu_list[::cpu_count] = out;
			++::cpu_count;
		}

		if (::cpu_count <= 1)
		{
			return false;
		}

		if (!kernel::time::monotonic::available())
		{
			kernel::log::write_line("smp needs monotonic time for bring-up");
			return false;
		}

		if (!setup_trampoline_image())
		{
			return false;
		}

		const uint32_t bsp_apic_id = kernel::arch::x86_64::apic::lapic::id() & 0xFFu;

		size_t started = 1;

		for (size_t i = 0; i < ::cpu_count; ++i)
		{
			const Cpu& c = cpu_list[i];
			if (!c.enabled)
			{
				continue;
			}

			if (c.apic_id == bsp_apic_id)
			{
				continue;
			}

			if (start_cpu(c.apic_id))
			{
				++started;
			}
			else
			{
				kernel::log::write("smp ap failed apic_id=");
				kernel::log::write_u64_dec(c.apic_id);
				kernel::log::write("\n", 1);
			}
		}

		for (size_t i = 0; i < ::cpu_count; ++i)
		{
			const Cpu& c = cpu_list[i];
			if (!c.enabled)
			{
				continue;
			}

			if (c.apic_id == bsp_apic_id)
			{
				continue;
			}

			if (ap_ready[c.apic_id].load(std::memory_order_acquire) != 0)
			{
				ap_go[c.apic_id].store(1, std::memory_order_release);
			}
		}

		for (size_t i = 0; i < ::cpu_count; ++i)
		{
			const Cpu& c = cpu_list[i];
			if (!c.enabled)
			{
				continue;
			}

			if (c.apic_id == bsp_apic_id)
			{
				continue;
			}

		}

		kernel::log::write("smp started=");
		kernel::log::write_u64_dec(started);
		kernel::log::write("/");
		kernel::log::write_u64_dec(::cpu_count);
		kernel::log::write("\n", 1);

		::smp_available = started > 1;
		return ::smp_available;
	}
}
