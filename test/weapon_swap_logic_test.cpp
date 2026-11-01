#include <string>

#include <gtest/gtest.h>
#include <sol/sol.hpp>

#include "utils/paths.h"
#include "utils/str_cat.hpp"

namespace devilution {
namespace {

// Builds views and prints plans. A plan prints as its moves in order, then the sound item's origin:
// "L>12 R>14 3>L s3" (L and R are the hands, numbers are cells), or "nil" if there is no plan.
constexpr const char *Helpers = R"lua(
function item(origin, w, h, hand, shield)
	local cells = {}
	for y = 0, h - 1 do
		for x = 0, w - 1 do
			cells[#cells + 1] = origin + y * 10 + x
		end
	end
	return { origin = origin, cells = cells, w = w, h = h, hand = hand, shield = shield == true }
end
function sword(origin) return item(origin, 1, 3, "one") end
function shield(origin) return item(origin, 2, 2, "one", true) end
function staff(origin) return item(origin, 2, 3, "two") end
function potion(origin) return item(origin, 1, 1) end
function marks(...)
	local marked = {}
	for _, cell in ipairs({ ... }) do marked[cell] = true end
	return marked
end
function view(marked, items, left, right, compatible)
	return {
		columns = 10, rows = 4, marked = marked, items = items, held = { left = left, right = right },
		compatible = compatible or function(a, b) return a.shield ~= b.shield end,
	}
end
local function where(place)
	if place.body == "left" then return "L" end
	if place.body == "right" then return "R" end
	return tostring(place.cell)
end
function showOutline(marked)
	local parts = {}
	for _, s in ipairs(logic.outline(marked, 10, 4)) do
		parts[#parts + 1] = (s.horizontal and "h" or "v") .. s.x .. "," .. s.y .. "+" .. s.length
	end
	table.sort(parts)
	return table.concat(parts, " ")
end
function show(plan)
	if plan == nil then return "nil" end
	local parts = {}
	for _, move in ipairs(plan.moves) do
		parts[#parts + 1] = where(move.from) .. ">" .. where(move.to)
	end
	parts[#parts + 1] = "s" .. plan.soundItem.origin
	return table.concat(parts, " ")
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

	/** @brief Runs a Lua body that returns a string. */
	std::string Run(const std::string &body)
	{
		const sol::protected_function_result result = lua_.safe_script(body, sol::script_pass_on_error);
		if (!result.valid())
			return StrCat("error: ", result.get<sol::error>().what());
		return result.get<std::string>();
	}

	sol::state lua_;
};

TEST_F(WeaponSwapLogicTest, Toggle_MarksAll_UnmarksAll_PartialBecomesAll)
{
	EXPECT_EQ(Run(R"lua(
		local m = {}
		logic.toggle(m, { 1, 2 })
		local a = tostring(m[1]) .. tostring(m[2])
		m[2] = nil
		logic.toggle(m, { 1, 2 })
		local b = tostring(m[1]) .. tostring(m[2])
		logic.toggle(m, { 1, 2 })
		return a .. " " .. b .. " " .. tostring(m[1]) .. tostring(m[2])
	)lua"),
	    "truetrue truetrue nilnil");
}

TEST_F(WeaponSwapLogicTest, Outline_NoMarks_NoEdges)
{
	EXPECT_EQ(Run("return showOutline(marks())"), "");
}

TEST_F(WeaponSwapLogicTest, Outline_OneCell_FourSides)
{
	EXPECT_EQ(Run("return showOutline(marks(12))"), "h2,1+1 h2,2+1 v2,1+1 v3,1+1");
}

TEST_F(WeaponSwapLogicTest, Outline_Block_OnlyTheRim_InOneRun)
{
	// 2 wide, 3 high: no edge runs between two marked cells.
	EXPECT_EQ(Run("return showOutline(marks(0, 1, 10, 11, 20, 21))"), "h0,0+2 h0,3+2 v0,0+3 v2,0+3");
}

TEST_F(WeaponSwapLogicTest, Outline_LShape)
{
	// X.
	// XX
	EXPECT_EQ(Run("return showOutline(marks(4, 14, 15))"), "h4,0+1 h4,2+2 h5,1+1 v4,0+2 v5,0+1 v6,1+1");
}

TEST_F(WeaponSwapLogicTest, Outline_GridCorner_DrawsItsEdges)
{
	EXPECT_EQ(Run("return showOutline(marks(39))"), "h9,3+1 h9,4+1 v10,3+1 v9,3+1");
}

TEST_F(WeaponSwapLogicTest, Outline_RowEnd_DoesNotJoinTheNextRow)
{
	// Cells 9 and 10 are neighbours by number only.
	EXPECT_EQ(Run("return showOutline(marks(9, 10))"), "h0,1+1 h0,2+1 h9,0+1 h9,1+1 v0,1+1 v1,1+1 v10,0+1 v9,0+1");
}

TEST_F(WeaponSwapLogicTest, Outline_Diagonal_MeetsInOneRun)
{
	EXPECT_EQ(Run("return showOutline(marks(0, 11))"), "h0,0+1 h0,1+2 h1,2+1 v0,0+1 v1,0+2 v2,1+1");
}

TEST_F(WeaponSwapLogicTest, Outline_FalseEntries_CountAsUnmarked)
{
	EXPECT_EQ(Run("local m = marks(0); m[1] = false; return showOutline(m)"), "h0,0+1 h0,1+1 v0,0+1 v1,0+1");
}

TEST_F(WeaponSwapLogicTest, SixCells_Sword_Staff_Works)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 1, 10, 11, 20, 21), { sword(0) }, staff(-1))))
	)lua"),
	    "L>0 0>L s0");
}

TEST_F(WeaponSwapLogicTest, SwordOnly_KeepsShield)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 10, 20), { sword(0) }, sword(-1), shield(-1))))
	)lua"),
	    "L>0 0>L s0");
}

TEST_F(WeaponSwapLogicTest, SwordShield_ForStaff)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 1, 2, 10, 11, 12, 20, 21, 22), { sword(0), shield(1) }, staff(-1))))
	)lua"),
	    "L>0 0>L 1>R s0");
}

TEST_F(WeaponSwapLogicTest, Placement_LShape_NeedsBacktrack)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 10, 20, 11, 12, 21, 22, 33), { item(33, 1, 1, "two") }, sword(-1), shield(-1))))
	)lua"),
	    "L>0 R>11 33>L s33");
}

TEST_F(WeaponSwapLogicTest, ShieldOnly_Beside2H_KeepsTheTwoHander)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 1, 10, 11), { shield(0) }, staff(-1), shield(-1))))
	)lua"),
	    "R>0 0>R s0");
}

TEST_F(WeaponSwapLogicTest, ThreeCells_Sword_Staff_Fails)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 10, 20), { sword(0) }, staff(-1))))
	)lua"),
	    "nil");
}

TEST_F(WeaponSwapLogicTest, TwoHand_IntoExactlyFitting)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 1, 10, 11, 20, 21), { staff(0) }, staff(-1))))
	)lua"),
	    "L>0 0>L s0");
}

TEST_F(WeaponSwapLogicTest, ShieldOnly_KeepsSword)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 1, 10, 11), { shield(0) }, sword(-1), shield(-1))))
	)lua"),
	    "R>0 0>R s0");
}

TEST_F(WeaponSwapLogicTest, SwordIntoEmptyHandBesideShield_NoOutgoing)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 10, 20), { sword(0) }, nil, shield(-1))))
	)lua"),
	    "0>L s0");
}

TEST_F(WeaponSwapLogicTest, ShieldIntoEmptyHandBesideSword_NoOutgoing)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 1, 10, 11), { shield(0) }, sword(-1))))
	)lua"),
	    "0>R s0");
}

TEST_F(WeaponSwapLogicTest, Sword_Replaces2H)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 1, 10, 11, 20, 21, 3, 13, 23), { sword(3) }, staff(-1))))
	)lua"),
	    "L>0 3>L s3");
}

TEST_F(WeaponSwapLogicTest, HandsEmpty_Equips)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 10, 20), { sword(0) })))
	)lua"),
	    "0>L s0");
}

TEST_F(WeaponSwapLogicTest, TwoSwords_Bard)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 1, 2, 10, 11, 12, 20, 21, 22), { sword(0), sword(1) }, sword(-1), nil, function() return true end)))
	)lua"),
	    "L>0 0>L 1>R s0");
}

TEST_F(WeaponSwapLogicTest, TwoSwords_Warrior_OnlyFirst)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 2, 10, 12, 20, 22, 1, 11, 21), { sword(0), sword(2) }, sword(-1), nil, function() return false end)))
	)lua"),
	    "L>0 0>L s0");
}

TEST_F(WeaponSwapLogicTest, BardTwoSwords_ShieldIn_ReplacesRight)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 1, 2, 10, 11, 12, 20, 21, 22), { shield(0) }, sword(-1), sword(-1), function() return true end)))
	)lua"),
	    "R>0 0>R s0");
}

TEST_F(WeaponSwapLogicTest, Staff_First_TakenAlone)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 1, 2, 10, 11, 12, 20, 21, 22, 4, 14, 24), { staff(0), sword(2) }, sword(-1))))
	)lua"),
	    "L>0 0>L s0");
}

TEST_F(WeaponSwapLogicTest, ShieldFirst_SwordSecond_WeaponGoesLeft)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 1, 2, 10, 11, 12, 20, 21, 22), { shield(0), sword(2) })))
	)lua"),
	    "2>L 0>R s0");
}

TEST_F(WeaponSwapLogicTest, Potion_InArea_BlocksCells)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 1, 10, 11, 20, 21), { sword(0), potion(1) }, staff(-1))))
	)lua"),
	    "nil");
}

TEST_F(WeaponSwapLogicTest, GoldBlocks)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 1, 10, 11, 20, 21), { sword(0), item(21, 1, 1) }, staff(-1))))
	)lua"),
	    "nil");
}

TEST_F(WeaponSwapLogicTest, PartiallyMarkedItem_NotCandidate_Blocks)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 10), { sword(0) }, sword(-1))))
	)lua"),
	    "nil");
}

TEST_F(WeaponSwapLogicTest, PartlyMarkedItem_NeverSelected_AndBlocks)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 1, 10, 11, 20, 21), { sword(0), sword(21) }, staff(-1))))
	)lua"),
	    "nil");
}

TEST_F(WeaponSwapLogicTest, LeftoverCandidate_Blocks)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 1, 2, 10, 11, 12, 20, 21, 22), { sword(0), shield(1), sword(21) }, staff(-1))))
	)lua"),
	    "nil");
}

TEST_F(WeaponSwapLogicTest, UnusableItem_NotCandidate)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 1, 10, 11, 21, 31), { item(0, 1, 1), sword(1) }, sword(-1))))
	)lua"),
	    "L>1 1>L s1");
}

TEST_F(WeaponSwapLogicTest, UnusableOnly_Nil)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 10, 20), { item(0, 1, 3) })))
	)lua"),
	    "nil");
}

TEST_F(WeaponSwapLogicTest, EmptyArea_Nil)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 1, 10, 11), {}, sword(-1))))
	)lua"),
	    "nil");
}

TEST_F(WeaponSwapLogicTest, NoMarks_Nil)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(), { sword(0) }, sword(-1))))
	)lua"),
	    "nil");
}

TEST_F(WeaponSwapLogicTest, TwoOutgoing_BothPlaced)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 1, 2, 3, 10, 11, 12, 13, 20, 21, 4, 5, 14, 15, 24, 25), { staff(4) }, staff(-1), shield(-1))))
	)lua"),
	    "L>0 R>2 4>L s4");
}

TEST_F(WeaponSwapLogicTest, TwoOutgoing_EqualSize_KeepHandOrder)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 1, 2, 3, 10, 11, 12, 13, 20, 21, 22, 23), { staff(2) }, sword(-1), sword(-1))))
	)lua"),
	    "L>0 R>1 2>L s2");
}

TEST_F(WeaponSwapLogicTest, TwoOutgoing_OneDoesNotFit_Nil)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 1, 10, 11, 20, 21), { staff(0) }, sword(-1), shield(-1))))
	)lua"),
	    "nil");
}

TEST_F(WeaponSwapLogicTest, Outgoing_DoesNotWrapPastRowEnd)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(9, 10, 19, 20, 29, 30), { item(9, 1, 3, "one") }, staff(-1))))
	)lua"),
	    "nil");
}

TEST_F(WeaponSwapLogicTest, Outgoing_DoesNotLeaveTheGrid)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(20, 21, 30, 31, 40, 41), { item(20, 2, 2, "one") }, staff(-1))))
	)lua"),
	    "nil");
}

TEST_F(WeaponSwapLogicTest, RoundTrip_Restores)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 1, 10, 11, 20, 21), { sword(0) }, staff(-1)))) .. "|" .. show(logic.planSwap(view(marks(0, 1, 10, 11, 20, 21), { staff(0) }, sword(-1))))
	)lua"),
	    "L>0 0>L s0|L>0 0>L s0");
}

TEST_F(WeaponSwapLogicTest, Moves_Shape)
{
	EXPECT_EQ(Run(R"lua(
		return (function() local i = sword(0); local p = logic.planSwap(view(marks(0, 10, 20), { i }, sword(-1))); return p.moves[1].from.body .. " " .. math.type(p.moves[1].to.cell) .. " " .. p.moves[2].to.body .. " " .. tostring(p.soundItem == i) end)()
	)lua"),
	    "left integer left true");
}

TEST_F(WeaponSwapLogicTest, PartlyMarkedItem_NotSelected_EmptyHands)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 10), { sword(0) })))
	)lua"),
	    "nil");
}

TEST_F(WeaponSwapLogicTest, PartlyMarkedItem_SkippedForFullyMarked)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 10, 1, 11, 21), { sword(0), sword(1) }, sword(-1))))
	)lua"),
	    "L>1 1>L s1");
}

TEST_F(WeaponSwapLogicTest, Staff_First_LeavesShieldInBag)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 1, 2, 3, 10, 11, 12, 13, 20, 21), { staff(0), shield(2) })))
	)lua"),
	    "0>L s0");
}

TEST_F(WeaponSwapLogicTest, Sword_First_LeavesStaffInBag)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 1, 2, 10, 11, 12, 20, 21, 22), { sword(0), staff(1) }, nil, nil, function() return true end)))
	)lua"),
	    "0>L s0");
}

TEST_F(WeaponSwapLogicTest, ShieldIn_Replaces2H)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 1, 10, 11, 20, 21), { shield(0) }, staff(-1))))
	)lua"),
	    "L>0 0>L s0");
}

TEST_F(WeaponSwapLogicTest, SwordIntoRight_WhenOnlyRightHoldsWeapon)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 10, 20), { sword(0) }, nil, sword(-1))))
	)lua"),
	    "R>0 0>R s0");
}

TEST_F(WeaponSwapLogicTest, Outgoing_LargerPlacedFirst)
{
	EXPECT_EQ(Run(R"lua(
		return show(logic.planSwap(view(marks(0, 1, 2, 10, 11, 12, 20, 22, 33), { item(33, 1, 1, "two") }, sword(-1), shield(-1))))
	)lua"),
	    "L>2 R>0 33>L s33");
}

} // namespace
} // namespace devilution
