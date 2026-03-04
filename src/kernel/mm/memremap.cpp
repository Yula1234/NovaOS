#include "kernel/mm/memremap.hpp"

#include "kernel/mm/physmap.hpp"

namespace kernel::mm::memremap
{
	const void* map(uint64_t phys, size_t) noexcept
	{
		return kernel::mm::physmap::to_virt_const(phys);
	}
}
