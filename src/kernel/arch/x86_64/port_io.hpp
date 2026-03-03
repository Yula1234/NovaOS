#pragma once

#include <stdint.h>

namespace kernel::arch::x86_64
{
	inline void io_wait() noexcept
	{
		asm volatile("outb %%al, $0x80" : : "a"(0));
	}

	inline uint8_t inb(uint16_t port) noexcept
	{
		uint8_t value;
		asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
		return value;
	}

	inline void outb(uint16_t port, uint8_t value) noexcept
	{
		asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
	}

	inline uint16_t inw(uint16_t port) noexcept
	{
		uint16_t value;
		asm volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
		return value;
	}

	inline void outw(uint16_t port, uint16_t value) noexcept
	{
		asm volatile("outw %0, %1" : : "a"(value), "Nd"(port));
	}

	inline uint32_t inl(uint16_t port) noexcept
	{
		uint32_t value;
		asm volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
		return value;
	}

	inline void outl(uint16_t port, uint32_t value) noexcept
	{
		asm volatile("outl %0, %1" : : "a"(value), "Nd"(port));
	}
}
