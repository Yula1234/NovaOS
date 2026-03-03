#pragma once

#include <stdint.h>

namespace kernel::lib
{
	class SpinLock
	{
	public:
		SpinLock() noexcept = default;

		SpinLock(const SpinLock&) = delete;
		SpinLock& operator=(const SpinLock&) = delete;

		void lock() noexcept
		{
			for (;;)
			{
				bool expected = false;
				if (__atomic_compare_exchange_n(&locked_, &expected, true, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
				{
					return;
				}

				asm volatile("pause");
			}
		}

		bool try_lock() noexcept
		{
			bool expected = false;
			return __atomic_compare_exchange_n(&locked_, &expected, true, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
		}

		void unlock() noexcept
		{
			__atomic_store_n(&locked_, false, __ATOMIC_RELEASE);
		}

		bool is_locked() const noexcept
		{
			return __atomic_load_n(&locked_, __ATOMIC_RELAXED);
		}

	private:
		bool locked_ = false;
	};

	template<typename Lock>
	class LockGuard
	{
	public:
		explicit LockGuard(Lock& lock) noexcept
			: lock_(lock)
		{
			lock_.lock();
		}

		LockGuard(const LockGuard&) = delete;
		LockGuard& operator=(const LockGuard&) = delete;

		~LockGuard()
		{
			lock_.unlock();
		}

	private:
		Lock& lock_;
	};
}
