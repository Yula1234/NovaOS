#include "kernel/abi.hpp"
#include "kernel/console/vga.hpp"
#include "kernel/log/log.hpp"
#include "kernel/log/sink.hpp"
#include "kernel/log/sinks_serial.hpp"
#include "kernel/log/sinks_vga.hpp"
#include "kernel/serial/com1.hpp"

#include "kernel/arch/x86_64/cpu.hpp"
#include "kernel/arch/x86_64/cpu_local.hpp"
#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/arch/x86_64/interrupts.hpp"
#include "kernel/arch/x86_64/apic/ioapic.hpp"
#include "kernel/arch/x86_64/apic/lapic.hpp"
#include "kernel/arch/x86_64/pic.hpp"
#include "kernel/arch/x86_64/smp.hpp"

#include "kernel/time/time.hpp"

#include "kernel/acpi/acpi.hpp"
#include "kernel/boot/multiboot2.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/ioremap.hpp"
#include "kernel/mm/vmm.hpp"
#include "kernel/mm/heap.hpp"

#include "kernel/runtime/init_array.hpp"

extern "C" void kmain(unsigned multiboot_magic, unsigned multiboot_info_addr)
{
	kernel::console::vga::clear();

	static kernel::serial::Com1 com1;
	com1.init();

	static kernel::log::SerialSink serial_sink(com1);
	static kernel::log::VgaSink vga_sink;
	static kernel::log::MultiSink multi_sink(serial_sink, vga_sink);
	kernel::log::set_sink(multi_sink);

	kernel::arch::x86_64::gdt::init_bsp();
	kernel::arch::x86_64::cpu_local::init_bsp();
	kernel::arch::x86_64::idt::init();
	kernel::arch::x86_64::enable_nx();

	kernel::log::write_line("Hello, world!");

	kernel::log::write("multiboot magic=");
	kernel::log::write_u64_hex(multiboot_magic);
	kernel::log::write(" info=");
	kernel::log::write_u64_hex(multiboot_info_addr);
	kernel::log::write("\n", 1);

	bool apic_ok = false;

	if (multiboot_magic == kernel::boot::multiboot2::bootloader_magic)
	{
		kernel::boot::multiboot2::Reader reader(multiboot_info_addr);
		const auto count = reader.memory_map_entry_count();
		const auto* entries = reader.memory_map_entries();
		const auto entry_size = reader.memory_map_entry_size();

		kernel::log::write("mmap entries=");
		kernel::log::write_u64_dec(count);
		kernel::log::write(" entry_size=");
		kernel::log::write_u64_dec(entry_size);
		kernel::log::write("\n", 1);

		for (size_t i = 0; i < count; ++i)
		{
			const auto* e = reinterpret_cast<const kernel::boot::multiboot2::MemoryMapEntry*>(
				reinterpret_cast<const uint8_t*>(entries) + i * entry_size
			);

			kernel::log::write("mmap ");
			kernel::log::write_u64_dec(i);
			kernel::log::write(" addr=");
			kernel::log::write_u64_hex(e->addr);
			kernel::log::write(" len=");
			kernel::log::write_u64_hex(e->len);
			kernel::log::write(" type=");
			kernel::log::write_u64_dec(e->type);
			kernel::log::write("\n", 1);
		}

		kernel::mm::pmm::init(reader);
		kernel::mm::vmm::init();
		kernel::mm::vmm::map_physmap_all(reader);
		kernel::mm::pmm::set_alloc_limit(~0ull);
		kernel::mm::ioremap::init();
		kernel::mm::heap::init();
		kernel::runtime::init_global_constructors();

		constexpr uint64_t test_virt = 0xFFFFFE0000100000ull;
		const uint64_t test_phys = kernel::mm::pmm::alloc_page();
		if (test_phys != 0)
		{
			kernel::mm::vmm::kernel_space().map_page(
				test_virt,
				test_phys,
				kernel::mm::vmm::PageFlags::Writable
			);

			auto* p = reinterpret_cast<volatile uint64_t*>(test_virt);
			*p = 0x1122334455667788ull;

			kernel::log::write("vmm test read=");
			kernel::log::write_u64_hex(*p);
			kernel::log::write(" phys=");
			kernel::log::write_u64_hex(kernel::mm::vmm::kernel_space().translate(test_virt));
			kernel::log::write("\n", 1);

			kernel::mm::vmm::kernel_space().unmap_page(test_virt);
			kernel::mm::pmm::free_page(test_phys);
		}

		void* h1 = kernel::mm::heap::alloc(24);
		void* h2 = kernel::mm::heap::alloc(4096);
		kernel::log::write_line("heap alloc ok");
		kernel::mm::heap::free(h1);
		kernel::log::write_line("heap free h1 ok");
		kernel::mm::heap::free(h2);
		kernel::log::write_line("heap free h2 ok");

		struct Pair
		{
			uint64_t a;
			uint64_t b;
		};

		auto* p = new Pair{ 1, 2 };
		kernel::log::write_line("new ok");
		kernel::log::write("new pair=");
		kernel::log::write_u64_hex(reinterpret_cast<uint64_t>(p));
		kernel::log::write("\n", 1);
		delete p;
		kernel::log::write_line("delete ok");

		if (kernel::acpi::init(reader))
		{
			if (const auto* madt = kernel::acpi::madt())
			{
				kernel::arch::x86_64::apic::lapic::init(madt->lapic_phys);
				kernel::arch::x86_64::apic::ioapic::init(madt->ioapic_phys, madt->ioapic_gsi_base);
				kernel::arch::x86_64::apic::ioapic::route_irq(
					madt->irq0_gsi,
					0x20,
					kernel::arch::x86_64::apic::lapic::id(),
					madt->irq0_flags
				);
				kernel::arch::x86_64::interrupts::use_apic();

				kernel::arch::x86_64::pic::set_mask(0xFFFF);
				apic_ok = true;
			}
		}
	}

	if (!apic_ok)
	{
		kernel::arch::x86_64::interrupts::use_pic();
		kernel::arch::x86_64::pic::remap(0x20, 0x28);
		kernel::arch::x86_64::pic::set_mask(0xFFFE);
	}

	kernel::time::init(1000);

	if (apic_ok)
	{
		if (const auto* madt = kernel::acpi::madt())
		{
			kernel::arch::x86_64::smp::init(*madt);
		}
	}
	kernel::arch::x86_64::sti();

	uint64_t last_second_tick = 0;

	for (;;)
	{
		const uint32_t hz = kernel::time::frequency_hz();
		const uint64_t current = kernel::time::ticks();

		if (hz != 0 && current - last_second_tick >= hz)
		{
			last_second_tick = current;

			kernel::log::write("tick ");
			kernel::log::write_u64_dec(current);
			kernel::log::write(" ms=");
			kernel::log::write_u64_dec(kernel::time::ms_since_time_init());
			kernel::log::write("\n", 1);
		}

		asm volatile("hlt");
	}
}
