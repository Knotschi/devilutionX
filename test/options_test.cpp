#include "options.h"

#include <cstddef>

#include <gtest/gtest.h>

namespace devilution {
namespace {

/**
 * @brief Overwrites the stack below the caller, where the frames of returned calls were.
 *
 * Anything still read from those frames is garbage from here on, instead of
 * old data that happens to be intact.
 */
[[gnu::noinline]] void ClobberStack()
{
	constexpr size_t Size = 64 * 1024;
	volatile unsigned char stack[Size];
	for (size_t i = 0; i < Size; ++i) {
		stack[i] = 0xAA;
	}
}

// Without a stored copy of the callback, SetValue reads garbage: the callback is lost or the test crashes.
TEST(OptionEntry, SetValueChangedCallback_Twice_RunsTheLastAfterTheCallReturned)
{
	OptionEntryBoolean option("key", OptionEntryFlags::None, "name", "description", false);
	int firstCalls = 0;
	int lastCalls = 0;
	const auto first = [&firstCalls]() { ++firstCalls; };
	const auto last = [&lastCalls]() { ++lastCalls; };

	option.SetValueChangedCallback(first);
	option.SetValueChangedCallback(last);
	ClobberStack();
	option.SetValue(true);

	EXPECT_EQ(firstCalls, 0);
	EXPECT_EQ(lastCalls, 1);
}

} // namespace
} // namespace devilution
