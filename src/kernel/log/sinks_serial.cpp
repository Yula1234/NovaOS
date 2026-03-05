#include "kernel/log/sinks_serial.hpp"

namespace kernel::log
{
	SerialSink::SerialSink(kernel::serial::Com1& com1) noexcept
		: com1_(&com1)
	{
	}

	void SerialSink::write(const char* s, size_t len) noexcept
	{
		/* Sink may be constructed before the serial device is fully ready; drop output in that case. */
		if (!com1_)
		{
			return;
		}

		com1_->write(s, len);
	}
}
