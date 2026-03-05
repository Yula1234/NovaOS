#include "kernel/time/hpet.hpp"

#include <stddef.h>

#include "kernel/arch/x86_64/cpu.hpp"
#include "kernel/log/log.hpp"
#include "kernel/mm/ioremap.hpp"

namespace
{
	/* HPET register block offsets. */
	constexpr uint64_t reg_gc_id = 0x000;
	constexpr uint64_t reg_gc_cfg = 0x010;
	constexpr uint64_t reg_main_counter = 0x0F0;

	constexpr uint64_t cfg_enable = 1ull << 0;

	/* MMIO register window. */
	volatile uint64_t* regs = nullptr;
	uint64_t period_fs = 0;
	uint64_t ns_mul_q32 = 0;
	bool inited = false;

	constexpr uint64_t q32_shift = 32;

	void mul_u64_u64_128(uint64_t a, uint64_t b, uint64_t& lo, uint64_t& hi) noexcept
	{
		asm volatile(
			"mulq %[b]"
			: "=a"(lo), "=d"(hi)
			: "a"(a), [b] "r"(b)
			: "cc"
		);
	}

	uint64_t mul_shift_right_32(uint64_t a, uint64_t b) noexcept
	{
		uint64_t lo = 0;
		uint64_t hi = 0;
		mul_u64_u64_128(a, b, lo, hi);
		return (hi << (64 - q32_shift)) | (lo >> q32_shift);
	}

	uint64_t div_u128_u32(uint64_t hi, uint64_t lo, uint32_t d) noexcept
	{
		const uint32_t parts[4] = {
			static_cast<uint32_t>(hi >> 32),
			static_cast<uint32_t>(hi),
			static_cast<uint32_t>(lo >> 32),
			static_cast<uint32_t>(lo),
		};

		uint32_t q[4]{};
		uint64_t rem = 0;

		for (size_t i = 0; i < 4; ++i)
		{
			const uint64_t cur = (rem << 32) | parts[i];
			q[i] = static_cast<uint32_t>(cur / d);
			rem = cur % d;
		}

		const uint64_t q_hi = (static_cast<uint64_t>(q[0]) << 32) | q[1];
		const uint64_t q_lo = (static_cast<uint64_t>(q[2]) << 32) | q[3];

		if (q_hi != 0)
		{
			return UINT64_MAX;
		}

		return q_lo;
	}

	inline uint64_t read64(uint64_t off) noexcept
	{
		return regs[off / 8];
	}

	inline void write64(uint64_t off, uint64_t value) noexcept
	{
		regs[off / 8] = value;
	}
}

namespace kernel::time::hpet
{
	bool available() noexcept
	{
		return inited;
	}

	bool init(uint64_t hpet_phys) noexcept
	{
		if (hpet_phys == 0)
		{
			return false;
		}

		regs = static_cast<volatile uint64_t*>(kernel::mm::ioremap::map(hpet_phys, 0x1000));

		const uint64_t id = read64(reg_gc_id);
		/* Period is reported in femtoseconds per tick in the upper dword of GC_ID. */
		period_fs = (id >> 32) & 0xFFFFFFFFull;
		if (period_fs == 0)
		{
			kernel::log::write_line("hpet invalid period");
			return false;
		}

		constexpr uint32_t fs_per_ns = 1000000u;
		const __uint128_t numer = static_cast<__uint128_t>(period_fs) << q32_shift;
		const uint64_t hi = static_cast<uint64_t>(numer >> 64);
		const uint64_t lo = static_cast<uint64_t>(numer);
		/* Precompute (period_fs / 1e6) in Q32 fixed-point so counter->ns is a single mul+shift. */
		ns_mul_q32 = div_u128_u32(hi, lo, fs_per_ns);

		const uint64_t cfg = read64(reg_gc_cfg);
		write64(reg_gc_cfg, cfg & ~cfg_enable);
		write64(reg_main_counter, 0);
		write64(reg_gc_cfg, (cfg & ~cfg_enable) | cfg_enable);

		inited = true;

		kernel::log::write("hpet period_fs=");
		kernel::log::write_u64_dec(period_fs);
		kernel::log::write(" ns_mul_q32=");
		kernel::log::write_u64_hex(ns_mul_q32);
		kernel::log::write("\n", 1);

		return true;
	}

	uint64_t counter() noexcept
	{
		return inited ? read64(reg_main_counter) : 0;
	}

	uint64_t ns_since_boot() noexcept
	{
		if (!inited)
		{
			return 0;
		}

		return mul_shift_right_32(counter(), ns_mul_q32);
	}

	void busy_wait_ns(uint64_t ns) noexcept
	{
		if (!inited || ns == 0)
		{
			return;
		}

		const uint64_t start_ns = ns_since_boot();
		const uint64_t deadline_ns = start_ns + ns;

		while (static_cast<int64_t>(ns_since_boot() - deadline_ns) < 0)
		{
			asm volatile("pause");
		}
	}
}
