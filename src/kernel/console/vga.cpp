#include "kernel/console/vga.hpp"

#include "lib/lock.hpp"

namespace
{
	/* VGA text mode buffer. This assumes identity-mapped low memory in early boot. */
	constexpr uintptr_t vga_phys = 0xB8000;
	constexpr size_t cols = 80;
	constexpr size_t rows = 25;
	constexpr uint8_t default_attr = 0x07;

	/* Volatile to prevent the compiler from folding/ordering MMIO-like stores to the text buffer. */
	volatile uint16_t* const buffer = reinterpret_cast<volatile uint16_t*>(vga_phys);

	size_t cursor_col = 0;
	size_t cursor_row = 0;

	kernel::lib::McsLock vga_lock;

	uint16_t make_cell(char c, uint8_t attr) noexcept
	{
		return static_cast<uint16_t>(attr) << 8 | static_cast<uint8_t>(c);
	}

	void put_at(size_t col, size_t row, char c, uint8_t attr) noexcept
	{
		buffer[row * cols + col] = make_cell(c, attr);
	}

	void newline() noexcept
	{
		cursor_col = 0;

		if (cursor_row + 1 < rows)
		{
			++cursor_row;
			return;
		}

		/* Simple scroll: shift the buffer up by one row. Cheap and good enough for early console. */
		for (size_t r = 1; r < rows; ++r)
		{
			for (size_t c = 0; c < cols; ++c)
			{
				buffer[(r - 1) * cols + c] = buffer[r * cols + c];
			}
		}

		for (size_t c = 0; c < cols; ++c)
		{
			put_at(c, rows - 1, ' ', default_attr);
		}
	}

	void put(char c) noexcept
	{
		if (c == '\n')
		{
			newline();
			return;
		}

		put_at(cursor_col, cursor_row, c, default_attr);

		if (++cursor_col >= cols)
		{
			newline();
		}
	}
}

namespace kernel::console::vga
{
	void clear() noexcept
	{
		/* IRQ-safe lock: console writes may happen from exception paths. */
		kernel::lib::IrqMcsLockGuard guard(vga_lock);

		for (size_t r = 0; r < rows; ++r)
		{
			for (size_t c = 0; c < cols; ++c)
			{
				put_at(c, r, ' ', default_attr);
			}
		}

		cursor_col = 0;
		cursor_row = 0;
	}

	void write(const char* s) noexcept
	{
		kernel::lib::IrqMcsLockGuard guard(vga_lock);

		if (!s)
		{
			return;
		}

		for (const char* p = s; *p; ++p)
		{
			put(*p);
		}
	}

	void write(const char* s, size_t len) noexcept
	{
		kernel::lib::IrqMcsLockGuard guard(vga_lock);

		if (!s)
		{
			return;
		}

		for (size_t i = 0; i < len; ++i)
		{
			put(s[i]);
		}
	}
}
