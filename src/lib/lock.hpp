#pragma once

#include <stdint.h>

namespace kernel::lib
{
	inline uint64_t irq_save_disable() noexcept
	{
		uint64_t rflags = 0;
		asm volatile(
			"pushfq\n"
			"pop %0\n"
			"cli\n"
			: "=r"(rflags)
			:
			: "memory"
		);

		return rflags;
	}

	inline void irq_restore(uint64_t rflags) noexcept
	{
		asm volatile(
			"push %0\n"
			"popfq\n"
			:
			: "r"(rflags)
			: "memory", "cc"
		);
	}

	class SpinLock
	{
	public:
		SpinLock() noexcept = default;

		SpinLock(const SpinLock&) = delete;
		SpinLock& operator=(const SpinLock&) = delete;

		void lock() noexcept
		{
			uint32_t backoff = 1;
			for (;;)
			{
				while (__atomic_load_n(&locked_, __ATOMIC_RELAXED) != 0)
				{
					for (uint32_t i = 0; i < backoff; ++i)
					{
						asm volatile("pause");
					}

					if (backoff < 1024)
					{
						backoff <<= 1;
					}
				}

				uint8_t expected = 0;
				if (__atomic_compare_exchange_n(&locked_, &expected, static_cast<uint8_t>(1), false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
				{
					return;
				}

				asm volatile("pause");
			}
		}

		bool try_lock() noexcept
		{
			uint8_t expected = 0;
			return __atomic_compare_exchange_n(&locked_, &expected, static_cast<uint8_t>(1), false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
		}

		void unlock() noexcept
		{
			__atomic_store_n(&locked_, static_cast<uint8_t>(0), __ATOMIC_RELEASE);
		}

		bool is_locked() const noexcept
		{
			return __atomic_load_n(&locked_, __ATOMIC_RELAXED) != 0;
		}

	private:
		alignas(64) uint8_t locked_ = 0;
		uint8_t padding_[63] = {};
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

	template<typename Lock>
	class IrqLockGuard
	{
	public:
		explicit IrqLockGuard(Lock& lock) noexcept
			: lock_(lock)
			, rflags_(irq_save_disable())
		{
			lock_.lock();
		}

		IrqLockGuard(const IrqLockGuard&) = delete;
		IrqLockGuard& operator=(const IrqLockGuard&) = delete;

		~IrqLockGuard()
		{
			lock_.unlock();
			irq_restore(rflags_);
		}

	private:
		Lock& lock_;
		uint64_t rflags_;
	};
}
