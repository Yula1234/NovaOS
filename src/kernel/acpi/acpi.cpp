#include "kernel/acpi/acpi.hpp"

#include "lib/align.hpp"
#include "kernel/arch/x86_64/cpu.hpp"
#include "kernel/boot/multiboot2.hpp"
#include "kernel/log/log.hpp"
#include "kernel/mm/ioremap.hpp"

namespace
{
	struct [[gnu::packed]] Rsdp
	{
		char signature[8];
		uint8_t checksum;
		char oem_id[6];
		uint8_t revision;
		uint32_t rsdt_address;

		uint32_t length;
		uint64_t xsdt_address;
		uint8_t extended_checksum;
		uint8_t reserved[3];
	};

	struct [[gnu::packed]] SdtHeader
	{
		char signature[4];
		uint32_t length;
		uint8_t revision;
		uint8_t checksum;
		char oem_id[6];
		char oem_table_id[8];
		uint32_t oem_revision;
		uint32_t creator_id;
		uint32_t creator_revision;
	};

	struct [[gnu::packed]] Xsdt
	{
		SdtHeader h;
		uint64_t entries[1];
	};

	struct [[gnu::packed]] Madt
	{
		SdtHeader h;
		uint32_t lapic_address;
		uint32_t flags;
		uint8_t entries[1];
	};

	struct [[gnu::packed]] MadtEntryHeader
	{
		uint8_t type;
		uint8_t length;
	};

	enum class MadtEntryType : uint8_t
	{
		LocalApic = 0,
		IoApic = 1,
		InterruptSourceOverride = 2,
	};

	struct [[gnu::packed]] MadtLocalApic
	{
		MadtEntryHeader h;
		uint8_t acpi_processor_id;
		uint8_t apic_id;
		uint32_t flags;
	};

	struct [[gnu::packed]] MadtIoApic
	{
		MadtEntryHeader h;
		uint8_t ioapic_id;
		uint8_t reserved;
		uint32_t ioapic_address;
		uint32_t gsi_base;
	};

	struct [[gnu::packed]] MadtIso
	{
		MadtEntryHeader h;
		uint8_t bus;
		uint8_t source_irq;
		uint32_t gsi;
		uint16_t flags;
	};

	struct [[gnu::packed]] Gas
	{
		uint8_t address_space;
		uint8_t bit_width;
		uint8_t bit_offset;
		uint8_t access_size;
		uint64_t address;
	};

	struct [[gnu::packed]] HpetTable
	{
		SdtHeader h;
		uint32_t event_timer_block_id;
		Gas base_address;
		uint8_t hpet_number;
		uint16_t minimum_tick;
		uint8_t page_protection;
	};

	constexpr bool sig_eq(const char a[4], const char b[4]) noexcept
	{
		return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
	}

	uint8_t checksum8(const void* data, size_t len) noexcept
	{
		const auto* p = static_cast<const uint8_t*>(data);
		uint8_t sum = 0;
		for (size_t i = 0; i < len; ++i)
		{
			sum = static_cast<uint8_t>(sum + p[i]);
		}
		return sum;
	}

	bool rsdp_valid(const Rsdp* r) noexcept
	{
		if (!r)
		{
			return false;
		}

		if (!(r->signature[0] == 'R' && r->signature[1] == 'S' && r->signature[2] == 'D' && r->signature[3] == ' ' &&
			r->signature[4] == 'P' && r->signature[5] == 'T' && r->signature[6] == 'R' && r->signature[7] == ' '))
		{
			return false;
		}

		if (checksum8(r, 20) != 0)
		{
			return false;
		}

		if (r->revision >= 2)
		{
			if (r->length < sizeof(Rsdp))
			{
				return false;
			}

			if (checksum8(r, r->length) != 0)
			{
				return false;
			}
		}

		return true;
	}

	bool sdt_valid(const SdtHeader* h) noexcept
	{
		if (!h)
		{
			return false;
		}

		if (h->length < sizeof(SdtHeader))
		{
			return false;
		}

		return checksum8(h, h->length) == 0;
	}

	kernel::acpi::MadtInfo madt_info{};
	bool madt_present = false;

	constexpr size_t max_cpus = 256;
	kernel::acpi::CpuInfo cpu_list[max_cpus]{};
	size_t cpu_count = 0;

	kernel::acpi::HpetInfo hpet_info{};
	bool hpet_present = false;
}

namespace kernel::acpi
{
	bool init(const kernel::boot::multiboot2::Reader& multiboot) noexcept
	{
		const uint64_t rsdp_phys = multiboot.acpi_rsdp_phys();
		if (rsdp_phys == 0)
		{
			kernel::log::write_line("acpi rsdp not found");
			return false;
		}

		const auto* rsdp = static_cast<const Rsdp*>(kernel::mm::ioremap::map(rsdp_phys, sizeof(Rsdp)));
		if (!rsdp_valid(rsdp))
		{
			kernel::log::write_line("acpi rsdp invalid");
			return false;
		}

		const uint64_t xsdt_phys = rsdp->revision >= 2 ? rsdp->xsdt_address : 0;
		if (xsdt_phys == 0)
		{
			kernel::log::write_line("acpi xsdt missing");
			return false;
		}

		const auto* xsdt_hdr = static_cast<const SdtHeader*>(kernel::mm::ioremap::map(xsdt_phys, sizeof(SdtHeader)));
		if (!xsdt_hdr || xsdt_hdr->length < sizeof(SdtHeader))
		{
			kernel::log::write_line("acpi xsdt header invalid");
			return false;
		}

		const auto* xsdt_full = static_cast<const Xsdt*>(kernel::mm::ioremap::map(xsdt_phys, xsdt_hdr->length));
		if (!sdt_valid(&xsdt_full->h))
		{
			kernel::log::write_line("acpi xsdt invalid");
			return false;
		}
		const size_t entries = (xsdt_full->h.length - sizeof(SdtHeader)) / sizeof(uint64_t);

		uint64_t madt_phys = 0;
		uint64_t hpet_phys = 0;

		for (size_t i = 0; i < entries; ++i)
		{
			const uint64_t sdt_phys = xsdt_full->entries[i];
			const auto* h = static_cast<const SdtHeader*>(kernel::mm::ioremap::map(sdt_phys, sizeof(SdtHeader)));
			if (!h || h->length < sizeof(SdtHeader))
			{
				continue;
			}

			const auto* full = static_cast<const SdtHeader*>(kernel::mm::ioremap::map(sdt_phys, h->length));
			if (!sdt_valid(full))
			{
				continue;
			}

			if (sig_eq(full->signature, "APIC"))
			{
				madt_phys = sdt_phys;
				continue;
			}

			if (sig_eq(full->signature, "HPET"))
			{
				hpet_phys = sdt_phys;
				continue;
			}
		}

		if (madt_phys == 0)
		{
			kernel::log::write_line("acpi madt not found");
			return false;
		}

		hpet_present = false;
		hpet_info = HpetInfo{};
		if (hpet_phys != 0)
		{
			const auto* hpet_hdr = static_cast<const SdtHeader*>(kernel::mm::ioremap::map(hpet_phys, sizeof(SdtHeader)));
			if (hpet_hdr && hpet_hdr->length >= sizeof(HpetTable))
			{
				const auto* hpet_tbl = static_cast<const HpetTable*>(kernel::mm::ioremap::map(hpet_phys, hpet_hdr->length));
				if (sdt_valid(&hpet_tbl->h) && hpet_tbl->base_address.address_space == 0)
				{
					hpet_info.hpet_phys = hpet_tbl->base_address.address;
					hpet_present = hpet_info.hpet_phys != 0;
				}
			}
		}

		const auto* madt_hdr = static_cast<const SdtHeader*>(kernel::mm::ioremap::map(madt_phys, sizeof(SdtHeader)));
		if (!madt_hdr || madt_hdr->length < sizeof(Madt))
		{
			kernel::log::write_line("acpi madt header invalid");
			return false;
		}

		const auto* madt_full = static_cast<const Madt*>(kernel::mm::ioremap::map(madt_phys, madt_hdr->length));
		if (!sdt_valid(&madt_full->h))
		{
			kernel::log::write_line("acpi madt invalid");
			return false;
		}

		madt_info = MadtInfo{};
		madt_info.lapic_phys = madt_full->lapic_address;
		madt_info.ioapic_phys = 0;
		madt_info.ioapic_gsi_base = 0;
		madt_info.irq0_gsi = 0;
		madt_info.irq0_flags = 0;
		madt_info.cpus = nullptr;
		madt_info.cpu_count = 0;

		cpu_count = 0;

		uint32_t irq0_gsi = 0;
		uint16_t irq0_flags = 0;

		const uint8_t* p = madt_full->entries;
		const uint8_t* const end = reinterpret_cast<const uint8_t*>(madt_full) + madt_full->h.length;

		while (p + sizeof(MadtEntryHeader) <= end)
		{
			const auto* eh = reinterpret_cast<const MadtEntryHeader*>(p);
			if (eh->length < sizeof(MadtEntryHeader) || p + eh->length > end)
			{
				break;
			}

			if (eh->type == static_cast<uint8_t>(MadtEntryType::LocalApic) && eh->length >= sizeof(MadtLocalApic))
			{
				const auto* la = reinterpret_cast<const MadtLocalApic*>(p);
				if (cpu_count < max_cpus)
				{
					kernel::acpi::CpuInfo info{};
					info.apic_id = la->apic_id;
					info.acpi_uid = la->acpi_processor_id;
					info.enabled = (la->flags & 1u) != 0;

					cpu_list[cpu_count] = info;
					++cpu_count;
				}
			}

			if (eh->type == static_cast<uint8_t>(MadtEntryType::IoApic) && eh->length >= sizeof(MadtIoApic))
			{
				const auto* io = reinterpret_cast<const MadtIoApic*>(p);
				if (madt_info.ioapic_phys == 0)
				{
					madt_info.ioapic_phys = io->ioapic_address;
					madt_info.ioapic_gsi_base = io->gsi_base;
				}
			}

			if (eh->type == static_cast<uint8_t>(MadtEntryType::InterruptSourceOverride) && eh->length >= sizeof(MadtIso))
			{
				const auto* iso = reinterpret_cast<const MadtIso*>(p);
				if (iso->bus == 0 && iso->source_irq == 0)
				{
					irq0_gsi = iso->gsi;
					irq0_flags = iso->flags;
				}
			}

			p += eh->length;
		}

		madt_info.irq0_gsi = irq0_gsi;
		madt_info.irq0_flags = irq0_flags;
		madt_info.cpus = cpu_list;
		madt_info.cpu_count = cpu_count;

		madt_present = madt_info.ioapic_phys != 0 && madt_info.lapic_phys != 0;

		kernel::log::write("acpi madt lapic=");
		kernel::log::write_u64_hex(madt_info.lapic_phys);
		kernel::log::write(" ioapic=");
		kernel::log::write_u64_hex(madt_info.ioapic_phys);
		kernel::log::write(" cpus=");
		kernel::log::write_u64_dec(cpu_count);
		kernel::log::write(" irq0_gsi=");
		kernel::log::write_u64_dec(madt_info.irq0_gsi);
		kernel::log::write(" hpet=");
		kernel::log::write_u64_hex(hpet_present ? hpet_info.hpet_phys : 0);
		kernel::log::write("\n", 1);

		return madt_present;
	}

	const MadtInfo* madt() noexcept
	{
		return madt_present ? &madt_info : nullptr;
	}

	const HpetInfo* hpet() noexcept
	{
		return hpet_present ? &hpet_info : nullptr;
	}
}
