#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kernel::log
{
	class Sink
	{
	public:
		virtual ~Sink() = default;

		virtual void write(const char* s, size_t len) noexcept = 0;
	};

	class NullSink final : public Sink
	{
	public:
		void write(const char*, size_t) noexcept override
		{
		}
	};

	class MultiSink final : public Sink
	{
	public:
		MultiSink(Sink& first, Sink& second) noexcept;
		void write(const char* s, size_t len) noexcept override;

	private:
		Sink* first_;
		Sink* second_;
	};
}
