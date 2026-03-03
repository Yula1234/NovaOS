#include "kernel/log/sinks_serial.hpp"

namespace kernel::log
{
	SerialSink::SerialSink(kernel::serial::Com1& com1) noexcept
		: com1_(&com1)
	{
	}

	void SerialSink::write(const char* s, size_t len) noexcept
	{
		if (!com1_)
		{
			return;
		}

		com1_->write(s, len);
	}
}
