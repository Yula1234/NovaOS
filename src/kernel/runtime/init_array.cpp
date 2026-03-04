#include "kernel/runtime/init_array.hpp"

#include <stddef.h>
#include <stdint.h>

using InitFunc = void (*)();

extern "C" InitFunc __init_array_start[];
extern "C" InitFunc __init_array_end[];

namespace kernel::runtime
{
	void init_global_constructors() noexcept
	{
		for (auto* fn = __init_array_start; fn != __init_array_end; ++fn)
		{
			if (*fn)
			{
				(*fn)();
			}
		}
	}
}
