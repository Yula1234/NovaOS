#pragma once

namespace kernel::runtime
{
	/* Runs global C++ constructors from .init_array; must be called once during early boot. */
	void init_global_constructors() noexcept;
}
