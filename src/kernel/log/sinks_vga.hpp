#pragma once

#include "kernel/console/vga.hpp"
#include "kernel/log/sink.hpp"

namespace kernel::log
{
	class VgaSink final : public Sink
	{
	public:
		void write(const char* s, size_t len) noexcept override;
	};
}
