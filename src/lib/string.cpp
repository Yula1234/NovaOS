#include "string.h"

#include "kernel/arch/x86_64/cpu.hpp"

namespace
{
	bool erms_available() noexcept
	{
		/* Cache CPUID once; used by memcpy/memset to pick rep movsb/stosb fast path. */
		static uint8_t cached = 0xFF;
		if (cached != 0xFF)
		{
			return cached != 0;
		}

		cached = kernel::arch::x86_64::erms_supported() ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0);
		return cached != 0;
	}
}

extern "C"
{
	void* memcpy(void* dst, const void* src, size_t n)
	{
		if (n == 0 || dst == src)
		{
			return dst;
		}

		void* out = dst;
		auto* d = static_cast<uint8_t*>(dst);
		auto* s = static_cast<const uint8_t*>(src);

		if (erms_available())
		{
			/* ERMS makes rep movsb competitive across most sizes; DF must be clear. */
			asm volatile(
				"cld\n"
				"rep movsb\n"
				: "+D"(d), "+S"(s), "+c"(n)
				:
				: "memory"
			);

			return out;
		}

		size_t qwords = n / 8;
		size_t bytes = n % 8;

		asm volatile(
			"cld\n"
			"rep movsq\n"
			"mov %3, %%rcx\n"
			"rep movsb\n"
			: "+D"(d), "+S"(s), "+c"(qwords)
			: "r"(bytes)
			: "memory"
		);

		return out;
	}

	void* memmove(void* dst, const void* src, size_t n)
	{
		if (n == 0 || dst == src)
		{
			return dst;
		}

		auto* d = static_cast<uint8_t*>(dst);
		auto* s = static_cast<const uint8_t*>(src);
		const uintptr_t da = reinterpret_cast<uintptr_t>(d);
		const uintptr_t sa = reinterpret_cast<uintptr_t>(s);

		/* If ranges don't overlap (or dst is before src), memcpy is safe. */
		if (da < sa || da - sa >= n)
		{
			memcpy(dst, src, n);
			return dst;
		}

		auto* d_end = d + n;
		auto* s_end = s + n;

		while (n >= 8)
		{
			d_end -= 8;
			s_end -= 8;
			*reinterpret_cast<uint64_t*>(d_end) = *reinterpret_cast<const uint64_t*>(s_end);
			n -= 8;
		}

		while (n != 0)
		{
			--d_end;
			--s_end;
			*d_end = *s_end;
			--n;
		}

		return dst;
	}

	void* memset(void* dst, int value, size_t n)
	{
		if (n == 0)
		{
			return dst;
		}

		void* out = dst;
		auto* d = static_cast<uint8_t*>(dst);

		if (erms_available())
		{
			asm volatile(
				"cld\n"
				"rep stosb\n"
				: "+D"(d), "+c"(n)
				: "a"(static_cast<uint8_t>(value))
				: "memory"
			);

			return out;
		}

		const uint64_t byte = static_cast<uint8_t>(value);
		const uint64_t pattern = byte * 0x0101010101010101ull;

		size_t qwords = n / 8;
		size_t bytes = n % 8;

		asm volatile(
			"cld\n"
			"rep stosq\n"
			"mov %3, %%rcx\n"
			"rep stosb\n"
			: "+D"(d), "+c"(qwords)
			: "a"(pattern), "r"(bytes)
			: "memory"
		);

		return out;
	}

	int memcmp(const void* a, const void* b, size_t n)
	{
		if (n == 0 || a == b)
		{
			return 0;
		}

		const uint8_t* pa = static_cast<const uint8_t*>(a);
		const uint8_t* pb = static_cast<const uint8_t*>(b);
		size_t remaining = n;

		asm volatile(
			"cld\n"
			"repe cmpsb\n"
			: "+D"(pa), "+S"(pb), "+c"(remaining)
			:
			: "memory"
		);

		if (remaining == 0)
		{
			return 0;
		}

		const uint8_t va = pa[-1];
		const uint8_t vb = pb[-1];
		return (va < vb) ? -1 : 1;
	}

	size_t strlen(const char* s)
	{
		if (!s)
		{
			return 0;
		}

		const char* p = s;
		size_t cnt = static_cast<size_t>(-1);

		asm volatile(
			"cld\n"
			"xor %%al, %%al\n"
			"repne scasb\n"
			: "+D"(p), "+c"(cnt)
			:
			: "rax", "memory"
		);

		return ~cnt - 1;
	}

	int strcmp(const char* a, const char* b)
	{
		if (a == b)
		{
			return 0;
		}

		if (!a)
		{
			return -1;
		}

		if (!b)
		{
			return 1;
		}

		int result = 0;
		size_t n = static_cast<size_t>(-1);
		asm volatile(
			"cld\n"
			"xor %%eax, %%eax\n"
			"1:\n"
			"movzbq (%%rdi), %%rax\n"
			"movzbq (%%rsi), %%rdx\n"
			"cmp %%dl, %%al\n"
			"jne 2f\n"
			"test %%al, %%al\n"
			"je 3f\n"
			"inc %%rdi\n"
			"inc %%rsi\n"
			"dec %%rcx\n"
			"jnz 1b\n"
			"3:\n"
			"xor %%eax, %%eax\n"
			"jmp 4f\n"
			"2:\n"
			"sub %%edx, %%eax\n"
			"4:\n"
			: "=a"(result), "+D"(a), "+S"(b), "+c"(n)
			:
			: "rdx", "memory"
		);

		return result;
	}

	int strncmp(const char* a, const char* b, size_t n)
	{
		if (n == 0 || a == b)
		{
			return 0;
		}

		if (!a)
		{
			return -1;
		}

		if (!b)
		{
			return 1;
		}

		int result = 0;
		asm volatile(
			"cld\n"
			"xor %%eax, %%eax\n"
			"test %%rcx, %%rcx\n"
			"je 3f\n"
			"1:\n"
			"movzbq (%%rdi), %%rax\n"
			"movzbq (%%rsi), %%rdx\n"
			"cmp %%dl, %%al\n"
			"jne 2f\n"
			"test %%al, %%al\n"
			"je 3f\n"
			"inc %%rdi\n"
			"inc %%rsi\n"
			"dec %%rcx\n"
			"jnz 1b\n"
			"3:\n"
			"xor %%eax, %%eax\n"
			"jmp 4f\n"
			"2:\n"
			"sub %%edx, %%eax\n"
			"4:\n"
			: "=a"(result), "+D"(a), "+S"(b), "+c"(n)
			:
			: "rdx", "memory"
		);

		return result;
	}
}
