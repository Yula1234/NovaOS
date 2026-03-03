#pragma once

#include <stdint.h>

namespace kernel::arch::x86_64
{
	struct [[gnu::packed]] InterruptFrame
	{
		uint64_t rip;
		uint64_t cs;
		uint64_t rflags;
		uint64_t rsp;
		uint64_t ss;
	};
}
