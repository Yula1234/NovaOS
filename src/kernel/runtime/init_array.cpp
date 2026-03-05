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
		/* Linker provides the init_array bounds; entries are plain function pointers. */
		for (auto* fn = __init_array_start; fn != __init_array_end; ++fn)
		{
			/* Some toolchains may emit null slots; skip them. */
			if (*fn)
			{
				(*fn)();
			}
		}
	}
}
