#pragma once

#include "kernel/console/vga.hpp"
#include "kernel/log/sink.hpp"

namespace kernel::log
{
	class VgaSink final : public Sink
	{
	public:
		/* Log sink backed by VGA text console (0xB8000). */
		void write(const char* s, size_t len) noexcept override;
	};
}
