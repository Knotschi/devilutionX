#include <string>

#include <gtest/gtest.h>
#include <sol/sol.hpp>

#include "utils/paths.h"
#include "utils/str_cat.hpp"

namespace devilution {
namespace {

// A plan prints as its moves: "L>12 R>14 3>L" (L and R are the hands, numbers are cells).
constexpr const char *Helpers = R"lua(
function item(origin, w, h, shield)
	local cells = {}
	for y = 0, h - 1 do
		for x = 0, w - 1 do
			cells[#cells + 1] = origin + y * 10 + x
		end
	end
	return { origin = origin, cells = cells, w = w, h = h, shield = shield == true }
end
function plan(items, marked, left, right)
	local occupied = {}
	for _, rec in ipairs(items) do
		for _, cell in ipairs(rec.cells) do occupied[cell] = true end
	end
	local moves = logic.planSwap({ columns = 10, rows = 4, occupied = occupied, marked = marked,
		held = { left = left, right = right },
		compatible = function(a, b) return a.shield ~= b.shield end })
	if moves == nil then return "nil" end
	local function where(place)
		if place.body == "left" then return "L" end
		if place.body == "right" then return "R" end
		return tostring(place.cell)
	end
	local parts = {}
	for _, move in ipairs(moves) do parts[#parts + 1] = where(move.from) .. ">" .. where(move.to) end
	return table.concat(parts, " ")
end
function fill(except)
	local items = {}
	for cell = 0, 39 do
		if not except[cell] then items[#items + 1] = item(cell, 1, 1) end
	end
	return items
end
)lua";

class WeaponSwapLogicTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		lua_.open_libraries(sol::lib::base, sol::lib::table, sol::lib::math, sol::lib::string);
		const std::string path = StrCat(paths::AssetsPath(), "lua/mods/weapon_swap/logic.lua");
		const sol::protected_function_result loaded = lua_.safe_script_file(path, sol::script_pass_on_error);
		ASSERT_TRUE(loaded.valid()) << loaded.get<sol::error>().what();
		lua_["logic"] = loaded.get<sol::table>();
		lua_.script(Helpers);
	}

	std::string Run(const std::string &body)
	{
		const sol::protected_function_result result = lua_.safe_script(body, sol::script_pass_on_error);
		if (!result.valid())
			return StrCat("error: ", result.get<sol::error>().what());
		return result.get<std::string>();
	}

	sol::state lua_;
};

TEST_F(WeaponSwapLogicTest, NothingMarked_NoPlan)
{
	EXPECT_EQ(Run("return plan({}, {}, item(0, 1, 3))"), "nil");
}

TEST_F(WeaponSwapLogicTest, EmptyHands_MarkedItemsEquip)
{
	EXPECT_EQ(Run("local a = item(0, 1, 3) return plan({ a }, { a })"), "0>L");
}

TEST_F(WeaponSwapLogicTest, ShieldGoesRight_WhicheverWasPickedFirst)
{
	EXPECT_EQ(Run("local s, a = item(0, 2, 2, true), item(2, 1, 3) return plan({ s, a }, { s, a })"), "2>L 0>R");
}

TEST_F(WeaponSwapLogicTest, TwoOneHanders_LeftThenRight)
{
	EXPECT_EQ(Run("local a, b = item(0, 1, 3), item(1, 1, 3) return plan({ a, b }, { a, b })"), "0>L 1>R");
}

TEST_F(WeaponSwapLogicTest, ShieldStays_WhenItGoesWithTheNewWeapon)
{
	EXPECT_EQ(Run(R"lua(
		local a = item(0, 1, 2)
		local others = fill({ [0] = true, [10] = true, [20] = true })
		others[#others + 1] = a
		return plan(others, { a }, item(0, 1, 3), item(0, 2, 2, true))
	)lua"),
	    "L>0 0>L");
}

TEST_F(WeaponSwapLogicTest, TwoMarked_BothHandsComeOut)
{
	EXPECT_EQ(Run("local a, b = item(0, 1, 3), item(1, 2, 2, true) return plan({ a, b }, { a, b }, item(0, 1, 3), item(0, 2, 2, true))"), "L>0 R>1 0>L 1>R");
}

TEST_F(WeaponSwapLogicTest, MarkedShield_ReplacesHeldShield_WeaponStays)
{
	EXPECT_EQ(Run("local s = item(0, 2, 2, true) return plan({ s }, { s }, item(0, 1, 3), item(0, 2, 2, true))"), "R>0 0>R");
}

TEST_F(WeaponSwapLogicTest, BothStayable_KindMatchGoes)
{
	EXPECT_EQ(Run(R"lua(
		local a = item(0, 1, 3)
		local occupied = { [0] = true, [10] = true, [20] = true }
		local moves = logic.planSwap({ columns = 10, rows = 4, occupied = occupied, marked = { a },
			held = { left = item(0, 1, 3), right = item(0, 1, 3) }, compatible = function() return true end })
		return moves[1].from.body .. " " .. moves[2].to.body
	)lua"),
	    "left left");
}

TEST_F(WeaponSwapLogicTest, OutgoingUsesCellsFreedByIncoming)
{
	EXPECT_EQ(Run(R"lua(
		local a = item(0, 2, 3)
		local others = fill({ [0] = true, [1] = true, [10] = true, [11] = true, [20] = true, [21] = true })
		others[#others + 1] = a
		return plan(others, { a }, item(0, 2, 3))
	)lua"),
	    "L>0 0>L");
}

TEST_F(WeaponSwapLogicTest, NoRoom_NoPlan)
{
	EXPECT_EQ(Run(R"lua(
		local a = item(0, 1, 3)
		local others = fill({ [0] = true, [10] = true, [20] = true })
		others[#others + 1] = a
		return plan(others, { a }, item(0, 2, 3))
	)lua"),
	    "nil");
}

TEST_F(WeaponSwapLogicTest, TwoOutgoing_BacktracksToFit)
{
	// First fit puts the small item in the only gap that the wide one could use.
	EXPECT_EQ(Run(R"lua(
		local a = item(3, 1, 1)
		local others = fill({ [0] = true, [1] = true, [3] = true })
		others[#others + 1] = a
		return plan(others, { a }, item(0, 1, 1), item(0, 2, 1))
	)lua"),
	    "L>3 R>0 3>L");
}

} // namespace
} // namespace devilution
