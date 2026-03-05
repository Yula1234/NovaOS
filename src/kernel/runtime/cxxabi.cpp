#include <stddef.h>
#include <stdint.h>

#include <new>

#include "kernel/mm/heap.hpp"
#include "kernel/panic.hpp"

extern "C"
{
	int __cxa_atexit(void (*)(void*), void*, void*)
	{
		/* No userspace, no dynamic unload: destructors are not run on shutdown yet. */
		return 0;
	}

	void __cxa_pure_virtual()
	{
		/* Calling a pure virtual function is a hard bug in kernel code. */
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

void* operator new(size_t size, std::align_val_t align)
{
	/*
	 * Kernel heap may not provide arbitrary alignment.
	 * We over-allocate and stash the raw pointer right before the aligned address.
	 */
	const size_t a = static_cast<size_t>(align);
	if (a == 0 || (a & (a - 1)) != 0)
	{
		kernel::panic("operator new aligned invalid align");
	}

	const size_t min_a = alignof(void*);
	const size_t effective_a = a < min_a ? min_a : a;

	if (size > static_cast<size_t>(~0ull) - effective_a - sizeof(void*))
	{
		kernel::panic("operator new aligned overflow");
	}

	const size_t total = size + effective_a + sizeof(void*);

	auto* raw = static_cast<uint8_t*>(kernel::mm::heap::alloc(total));
	if (!raw)
	{
		kernel::panic("operator new aligned");
	}

	uint64_t p = reinterpret_cast<uint64_t>(raw) + sizeof(void*);
	p = (p + static_cast<uint64_t>(effective_a - 1)) & ~static_cast<uint64_t>(effective_a - 1);

	auto** slot = reinterpret_cast<void**>(p - sizeof(void*));
	*slot = raw;

	return reinterpret_cast<void*>(p);
}

void* operator new[](size_t size, std::align_val_t align)
{
	return ::operator new(size, align);
}

void operator delete(void* ptr) noexcept
{
	kernel::mm::heap::free(ptr);
}

void operator delete[](void* ptr) noexcept
{
	kernel::mm::heap::free(ptr);
}

void operator delete(void* ptr, std::align_val_t) noexcept
{
	if (!ptr)
	{
		return;
	}

	/* raw pointer is stored in the word immediately preceding the returned aligned address. */
	auto** slot = reinterpret_cast<void**>(reinterpret_cast<uint64_t>(ptr) - sizeof(void*));
	kernel::mm::heap::free(*slot);
}

void operator delete[](void* ptr, std::align_val_t align) noexcept
{
	::operator delete(ptr, align);
}

void operator delete(void* ptr, size_t) noexcept
{
	kernel::mm::heap::free(ptr);
}

void operator delete[](void* ptr, size_t) noexcept
{
	kernel::mm::heap::free(ptr);
}

void operator delete(void* ptr, size_t, std::align_val_t align) noexcept
{
	::operator delete(ptr, align);
}

void operator delete[](void* ptr, size_t, std::align_val_t align) noexcept
{
	::operator delete(ptr, align);
}
