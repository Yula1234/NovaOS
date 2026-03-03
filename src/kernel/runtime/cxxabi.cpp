#include <stddef.h>
#include <stdint.h>

#include "kernel/mm/heap.hpp"
#include "kernel/panic.hpp"

extern "C"
{
	int __cxa_atexit(void (*)(void*), void*, void*)
	{
		return 0;
	}

	void __cxa_pure_virtual()
	{
		kernel::panic("pure virtual call");
	}
}

void* operator new(size_t size)
{
	void* p = kernel::mm::heap::alloc(size);
	if (!p)
	{
		kernel::panic("operator new");
	}

	return p;
}

void* operator new[](size_t size)
{
	void* p = kernel::mm::heap::alloc(size);
	if (!p)
	{
		kernel::panic("operator new[]");
	}

	return p;
}

void operator delete(void* ptr) noexcept
{
	kernel::mm::heap::free(ptr);
}

void operator delete[](void* ptr) noexcept
{
	kernel::mm::heap::free(ptr);
}

void operator delete(void* ptr, size_t) noexcept
{
	kernel::mm::heap::free(ptr);
}

void operator delete[](void* ptr, size_t) noexcept
{
	kernel::mm::heap::free(ptr);
}
