#pragma once

#include "kernel/log/sink.hpp"
#include "kernel/serial/com1.hpp"

namespace kernel::log
{
	class SerialSink final : public Sink
	{
	public:
		explicit SerialSink(kernel::serial::Com1& com1) noexcept;
		void write(const char* s, size_t len) noexcept override;

	private:
		kernel::serial::Com1* com1_;
	};
}
