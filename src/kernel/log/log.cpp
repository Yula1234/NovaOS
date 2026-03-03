#include "kernel/log/log.hpp"

namespace
{
	kernel::log::NullSink null_sink;
	kernel::log::Sink* active_sink = &null_sink;

	void write_char(char c) noexcept
	{
		active_sink->write(&c, 1);
	}
}

namespace kernel::log
{
	void set_sink(Sink& sink) noexcept
	{
		active_sink = &sink;
	}

	Sink& sink() noexcept
	{
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

		active_sink->write(s, len);
	}

	void write(const char* s, size_t len) noexcept
	{
		if (!s)
		{
			return;
		}

		active_sink->write(s, len);
	}

	void write_line(const char* s) noexcept
	{
		write(s);
		write("\n", 1);
	}

	void write_u64_dec(uint64_t value) noexcept
	{
		char buf[32];
		size_t i = 0;

		if (value == 0)
		{
			write_char('0');
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
			write_char(buf[--i]);
		}
	}

	void write_u64_hex(uint64_t value, bool prefix) noexcept
	{
		if (prefix)
		{
			write("0x", 2);
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
			write_char(c);
		}
	}
}
