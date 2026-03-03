#include "kernel/log/sink.hpp"

namespace kernel::log
{
	MultiSink::MultiSink(Sink& first, Sink& second) noexcept
		: first_(&first)
		, second_(&second)
	{
	}

	void MultiSink::write(const char* s, size_t len) noexcept
	{
		if (first_)
		{
			first_->write(s, len);
		}

		if (second_)
		{
			second_->write(s, len);
		}
	}
}
