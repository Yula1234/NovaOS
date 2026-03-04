#pragma once

#include <stdint.h>

namespace kernel::lib
{
	inline uint64_t irq_save_disable() noexcept
	{
		uint32_t depth = 0;
		asm volatile("mov %%gs:32, %0" : "=r"(depth) :: "memory");

		if (depth == 0)
		{
			uint64_t rflags = 0;
			asm volatile(
				"pushfq\n"
				"pop %0\n"
				"cli\n"
				: "=r"(rflags)
				:
				: "memory", "cc"
			);

			const uint64_t prev_if = (rflags >> 9) & 1ull;
			const uint8_t prev_if8 = static_cast<uint8_t>(prev_if);
			asm volatile("mov %0, %%gs:36" :: "r"(prev_if8) : "memory");
			asm volatile("movl $1, %%gs:32" ::: "memory");
			return prev_if;
		}

		asm volatile("cli" ::: "memory", "cc");
		++depth;
		asm volatile("mov %0, %%gs:32" :: "r"(depth) : "memory");
		return 0;
	}

	inline void irq_restore(uint64_t rflags) noexcept
	{
		uint32_t depth = 0;
		asm volatile("mov %%gs:32, %0" : "=r"(depth) :: "memory");
		if (depth == 0)
		{
			return;
		}

		--depth;
		asm volatile("mov %0, %%gs:32" :: "r"(depth) : "memory");
		if (depth != 0)
		{
			return;
		}

		uint8_t prev_if = 0;
		asm volatile("mov %%gs:36, %0" : "=r"(prev_if) :: "memory");
		if (prev_if != 0 && rflags != 0)
		{
			asm volatile("sti" ::: "memory", "cc");
		}
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

	struct alignas(64) McsNode
	{
		volatile uint8_t waiting = 1;
		McsNode* next = nullptr;
		uint8_t padding_[48] = {};
	};

	static_assert(sizeof(McsNode) == 64, "McsNode must be exactly one cache line");

	class McsLock
	{
	public:
		McsLock() noexcept = default;

		McsLock(const McsLock&) = delete;
		McsLock& operator=(const McsLock&) = delete;

		void lock(McsNode* node) noexcept
		{
			node->next = nullptr;
			node->waiting = 1;

			McsNode* prev = __atomic_exchange_n(&tail_, node, __ATOMIC_ACQ_REL);

			if (prev == nullptr)
			{
				return;
			}

			prev->next = node;
			__atomic_thread_fence(__ATOMIC_RELEASE);

			while (__atomic_load_n(&node->waiting, __ATOMIC_ACQUIRE) != 0)
			{
				asm volatile("pause");
			}
		}

		void unlock(McsNode* node) noexcept
		{
			McsNode* next = __atomic_load_n(&node->next, __ATOMIC_ACQUIRE);

			if (next == nullptr)
			{
				McsNode* expected = node;
				if (__atomic_compare_exchange_n(&tail_, &expected, static_cast<McsNode*>(nullptr), false, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
				{
					return;
				}

				while ((next = __atomic_load_n(&node->next, __ATOMIC_ACQUIRE)) == nullptr)
				{
					asm volatile("pause");
				}
			}

			__atomic_store_n(&next->waiting, static_cast<uint8_t>(0), __ATOMIC_RELEASE);
		}

		bool is_locked() const noexcept
		{
			return __atomic_load_n(&tail_, __ATOMIC_RELAXED) != nullptr;
		}

	private:
		alignas(64) McsNode* tail_ = nullptr;
		uint8_t padding_[64 - sizeof(McsNode*)] = {};
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

	class McsLockGuard
	{
	public:
		explicit McsLockGuard(McsLock& lock) noexcept
			: lock_(lock)
		{
			lock_.lock(&node_);
		}

		McsLockGuard(const McsLockGuard&) = delete;
		McsLockGuard& operator=(const McsLockGuard&) = delete;

		~McsLockGuard()
		{
			lock_.unlock(&node_);
		}

	private:
		McsLock& lock_;
		McsNode node_;
	};

	class IrqMcsLockGuard
	{
	public:
		explicit IrqMcsLockGuard(McsLock& lock) noexcept
			: lock_(lock)
			, rflags_(irq_save_disable())
		{
			lock_.lock(&node_);
		}

		IrqMcsLockGuard(const IrqMcsLockGuard&) = delete;
		IrqMcsLockGuard& operator=(const IrqMcsLockGuard&) = delete;

		~IrqMcsLockGuard()
		{
			lock_.unlock(&node_);
			irq_restore(rflags_);
		}

	private:
		McsLock& lock_;
		McsNode node_;
		uint64_t rflags_;
	};
}
