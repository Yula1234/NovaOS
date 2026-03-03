#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/log/sink.hpp"

namespace kernel::log
{
	void set_sink(Sink& sink) noexcept;
	Sink& sink() noexcept;

	void write(const char* s) noexcept;
	void write(const char* s, size_t len) noexcept;
	void write_line(const char* s) noexcept;

	void write_u64_dec(uint64_t value) noexcept;
	void write_u64_hex(uint64_t value, bool prefix = true) noexcept;
}
