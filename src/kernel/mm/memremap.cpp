#include "kernel/mm/memremap.hpp"

#include "kernel/mm/physmap.hpp"

namespace kernel::mm::memremap
{
	const void* map(uint64_t phys, size_t) noexcept
	{
		/* For now we rely on a global physmap mapping; size is kept for future dynamic mappings. */
		return kernel::mm::physmap::to_virt_const(phys);
	}
}
