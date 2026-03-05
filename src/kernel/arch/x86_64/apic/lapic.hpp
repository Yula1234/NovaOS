#pragma once

#include <stdint.h>

namespace kernel::arch::x86_64::apic::lapic
{
	/* xAPIC mode only (MMIO registers). x2APIC is deliberately not enabled yet. */
	bool available() noexcept;

	void init(uint64_t lapic_phys) noexcept;
	void init_cpu() noexcept;

	void eoi() noexcept;

	/* In xAPIC mode the LAPIC ID lives in the top byte of the ID register. */
	uint32_t id() noexcept;

	void send_ipi(uint32_t dest_apic_id, uint8_t vector) noexcept;
	void broadcast_ipi(uint8_t vector, bool include_self) noexcept;
	void broadcast_nmi(bool include_self) noexcept;
	void send_init_ipi_assert(uint32_t dest_apic_id) noexcept;
	void send_init_ipi_deassert(uint32_t dest_apic_id) noexcept;
	void send_init_ipi_edge(uint32_t dest_apic_id) noexcept;
	void send_startup_ipi(uint32_t dest_apic_id, uint8_t startup_vector) noexcept;
	void broadcast_init_ipi_assert(bool include_self) noexcept;
	void broadcast_init_ipi_deassert(bool include_self) noexcept;
	void broadcast_startup_ipi(uint8_t startup_vector, bool include_self) noexcept;
	/* SIPI startup_vector is the 4KiB page number of the real-mode entry point. */

	void set_ipi_handler(void (*handler)() noexcept) noexcept;

	void set_timer_handler(void (*handler)() noexcept) noexcept;
	void handle_timer_vector() noexcept;
	void handle_ipi_vector() noexcept;

	void write_timer_div(uint32_t value) noexcept;
	void write_lvt_timer(uint32_t value) noexcept;
	void write_timer_init(uint32_t value) noexcept;
	uint32_t read_timer_cur() noexcept;

	uint32_t read_esr() noexcept;
	uint32_t read_icr_low() noexcept;
	uint32_t read_icr_high() noexcept;

	void* timer_isr() noexcept;
	void* ipi_isr() noexcept;
}
