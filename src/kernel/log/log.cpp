#include "kernel/log/log.hpp"

#include "lib/lock.hpp"

namespace
{
	kernel::log::NullSink null_sink;
	kernel::log::Sink* active_sink = &null_sink;
	kernel::lib::McsLock log_lock;

	void write_unlocked(const char* s, size_t len) noexcept
	{
		if (!s || len == 0)
		{
			return;
		}
		active_sink->write(s, len);
	}

	void write_char_unlocked(char c) noexcept
	{
		active_sink->write(&c, 1);
	}
}

namespace kernel::log
{
	void set_sink(Sink& sink) noexcept
	{
		kernel::lib::IrqMcsLockGuard guard(log_lock);
		active_sink = &sink;
	}

	Sink& sink() noexcept
	{
		kernel::lib::IrqMcsLockGuard guard(log_lock);
		return *active_sink;
	}

	void write(const char* s) noexcept
	{
		if (!s)
		{
			return;
		}

		size_t len = 0;
		for (const char* p = s; *p; ++p)
		{
			++len;
		}

		kernel::lib::IrqMcsLockGuard guard(log_lock);
		write_unlocked(s, len);
	}

	void write(const char* s, size_t len) noexcept
	{
		kernel::lib::IrqMcsLockGuard guard(log_lock);
		write_unlocked(s, len);
	}

	void write_line(const char* s) noexcept
	{
		if (!s)
		{
			return;
		}

		size_t len = 0;
		for (const char* p = s; *p; ++p)
		{
			++len;
		}

		kernel::lib::IrqMcsLockGuard guard(log_lock);
		write_unlocked(s, len);
		write_char_unlocked('\n');
	}

	void write_u64_dec(uint64_t value) noexcept
	{
		kernel::lib::IrqMcsLockGuard guard(log_lock);

		char buf[32];
		size_t i = 0;

		if (value == 0)
		{
			write_char_unlocked('0');
			return;
		}

		while (value != 0)
		{
			const uint64_t digit = value % 10;
			buf[i++] = static_cast<char>('0' + digit);
			value /= 10;
		}

		while (i > 0)
		{
			write_char_unlocked(buf[--i]);
		}
	}

	void write_u64_hex(uint64_t value, bool prefix) noexcept
	{
		kernel::lib::IrqMcsLockGuard guard(log_lock);

		if (prefix)
		{
			active_sink->write("0x", 2);
		}

		bool started = false;
		for (int shift = 60; shift >= 0; shift -= 4)
		{
			const uint8_t nibble = static_cast<uint8_t>((value >> shift) & 0xF);

			if (!started)
			{
				if (nibble == 0 && shift != 0)
				{
					continue;
				}

				started = true;
			}

			const char c = nibble < 10 ? static_cast<char>('0' + nibble)
				: static_cast<char>('a' + (nibble - 10));
			write_char_unlocked(c);
		}
	}
}
