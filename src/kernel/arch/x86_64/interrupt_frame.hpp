#pragma once

#include <stdint.h>

namespace kernel::arch::x86_64
{
	struct [[gnu::packed]] InterruptFrameView
	{
		/* The CPU always pushes RIP/CS/RFLAGS. */
		uint64_t rip;
		uint64_t cs;
		uint64_t rflags;
	};

	struct [[gnu::packed]] InterruptFrame
	{
		/* Full hardware frame when the interrupt/trap crosses privilege levels (adds RSP/SS). */
		uint64_t rip;
		uint64_t cs;
		uint64_t rflags;
		uint64_t rsp;
		uint64_t ss;
	};

	struct [[gnu::packed]] InterruptContext
	{
		/* Saved by our assembly stubs, not by hardware. Keep in sync with isr stub prologue. */
		uint64_t r15;
		uint64_t r14;
		uint64_t r13;
		uint64_t r12;
		uint64_t r11;
		uint64_t r10;
		uint64_t r9;
		uint64_t r8;
		uint64_t rdi;
		uint64_t rsi;
		uint64_t rbp;
		uint64_t rbx;
		uint64_t rdx;
		uint64_t rcx;
		uint64_t rax;

		uint64_t vector;
		uint64_t error_code;

		/* Hardware frame follows; RSP/SS may be absent depending on CPL change. */
		uint64_t rip;
		uint64_t cs;
		uint64_t rflags;
	};
}
