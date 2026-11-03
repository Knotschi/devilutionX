#include <string>

#include <gtest/gtest.h>
#include <sol/sol.hpp>

#include "utils/paths.h"
#include "utils/str_cat.hpp"

namespace devilution {
namespace {

class ReservedInventoryLogicTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		lua_.open_libraries(sol::lib::base, sol::lib::table, sol::lib::math, sol::lib::string);
		const std::string path = StrCat(paths::AssetsPath(), "lua/mods/reserved_inventory/logic.lua");
		const sol::protected_function_result loaded = lua_.safe_script_file(path, sol::script_pass_on_error);
		ASSERT_TRUE(loaded.valid()) << loaded.get<sol::error>().what();
		lua_["logic"] = loaded.get<sol::table>();
	}

	bool Overlaps(int cell, int width, int height)
	{
		lua_["cell"] = cell;
		lua_["w"] = width;
		lua_["h"] = height;
		return lua_.script("return logic.overlaps({ [12] = true }, cell, w, h, 10)").get<bool>();
	}

	sol::state lua_;
};

TEST_F(ReservedInventoryLogicTest, Overlaps_AnyCoveredCell)
{
	EXPECT_TRUE(Overlaps(12, 1, 1));
	EXPECT_TRUE(Overlaps(1, 2, 2));
	EXPECT_TRUE(Overlaps(2, 1, 3));
}

TEST_F(ReservedInventoryLogicTest, Overlaps_NeighboursAreFine)
{
	EXPECT_FALSE(Overlaps(11, 1, 1));
	EXPECT_FALSE(Overlaps(13, 2, 2));
	EXPECT_FALSE(Overlaps(0, 2, 3));
}

TEST_F(ReservedInventoryLogicTest, Toggle_MarksThenUnmarks)
{
	EXPECT_EQ(lua_.script(R"lua(
		local m = {}
		logic.toggle(m, { 1, 2 })
		local first = tostring(m[1]) .. tostring(m[2])
		logic.toggle(m, { 1, 2 })
		return first .. tostring(m[1])
	)lua").get<std::string>(),
	    "truetruenil");
}

} // namespace
} // namespace devilution
