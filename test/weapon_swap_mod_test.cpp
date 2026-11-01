/**
 * @file weapon_swap_mod_test.cpp
 *
 * Tests for the weapon_swap mod as shipped, and installed as a folder under mods/. Rules are in weapon_swap_logic_test.cpp.
 */

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <SDL.h>
#include <gtest/gtest.h>

#include "lua_test_fixture.hpp"

#include "controls/keymapper.hpp"
#include "diablo.h"
#include "engine/size.hpp"
#include "game_mode.hpp"
#include "hero_store.hpp"
#include "inv.h"
#include "loadsave.h"
#include "lua/lua_event.hpp"
#include "menu.h"
#include "options.h"
#include "pfile.h"
#include "player.h"
#include "utils/display.h"
#include "utils/file_util.h"
#include "utils/paths.h"
#include "utils/str_cat.hpp"
#include "utils/utf8.hpp"

namespace devilution {
namespace {

constexpr SDL_Keycode SwapKey = 'W';
constexpr SDL_Keycode MarkKey = 'H';

OptionEntryBoolean *FindModEntry()
{
	for (OptionEntryBase *entry : GetOptions().Mods.GetEntries()) {
		if (entry->key == "weapon_swap")
			return static_cast<OptionEntryBoolean *>(entry);
	}
	return nullptr;
}

/** Records hero speech and item sounds: NOSOUND builds play nothing to listen for. */
constexpr char Spies[] = R"lua(
local audio = require('devilutionx.audio')
local player = require('devilutionx.player')
sounds = {}
realSfx = realSfx or audio.playSfx
realSelf = realSelf or player.self
rects = {}
local render = require('devilutionx.render')
realRect = realRect or render.rect
render.rect = function(x, y, w, h, color) table.insert(rects, { x, y, w, h, color }) end
audio.playSfx = function(id) table.insert(sounds, 'sfx' .. tostring(id)) end
player.self = function()
	local real = realSelf()
	return setmetatable({}, { __index = function(_, key)
		if key == 'say' then
			return function(_, speech) table.insert(sounds, 'say' .. tostring(speech)) end
		end
		local member = real[key]
		return function(_, ...) return member(real, ...) end
	end })
end
)lua";

constexpr char DropSpies[] = R"lua(
require('devilutionx.audio').playSfx = realSfx
require('devilutionx.player').self = realSelf
require('devilutionx.render').rect = realRect
)lua";

class WeaponSwapModTest : public LuaUITest {
protected:
	static void SetUpTestSuite()
	{
		LuaUITest::SetUpTestSuite();
		if (!HaveMainData())
			return;
		OptionEntryBoolean *mod = FindModEntry();
		if (mod != nullptr)
			mod->SetValue(true);
	}

	static void TearDownTestSuite()
	{
		if (!HaveMainData())
			return;
		OptionEntryBoolean *mod = FindModEntry();
		if (mod != nullptr)
			mod->SetValue(false);
		LuaUITest::TearDownTestSuite();
	}

	void SetUp() override
	{
		LuaUITest::SetUp();
		if (IsSkipped())
			return;
		ASSERT_NE(FindModEntry(), nullptr);
		StripPlayer();
		GivePlentyOfStats();
		AdjustToScreenGeometry({ 640, 480 });
		ClearHeroStore();
		ASSERT_TRUE(Eval(Spies).valid());
		lua::GameStart();
	}

	void TearDown() override
	{
		if (!IsSkipped()) {
			ASSERT_TRUE(Eval(DropSpies).valid());
			MyPlayer->reservedInvCells.reset();
			MyPlayer->HoldItem.clear();
			MyPlayer->_pmode = PM_STAND;
			PauseMode = 0;
			ClearHeroStore();
		}
		LuaUITest::TearDown();
	}

	static void Click(int cell)
	{
		const Point center = InvRect[SLOTXY_INV_FIRST + cell].Center();
		MousePosition = GetRightPanel().position + Displacement { center.x, center.y };
		CheckInvItem();
	}

	/** @brief Hold the mark key, click each cell once, let go. */
	static void Mark(std::initializer_list<int> cells)
	{
		KeymapperPress(MarkKey);
		for (const int cell : cells)
			Click(cell);
		KeymapperRelease(MarkKey);
	}

	/** @brief A key-up as the event handler of a level or game load sees it. */
	static void KeyUpDuringLoad(SDL_Keycode key)
	{
		SDL_Event event {};
		event.type = SDL_EVENT_KEY_UP;
#ifdef USE_SDL3
		event.key.key = key;
#else
		event.key.keysym.sym = key;
#endif
		DisableInputEventHandler(event, 0);
	}

	static void Swap()
	{
		KeymapperPress(SwapKey);
		KeymapperRelease(SwapKey);
	}

	/** @brief Marked cells, as both the player's mask and the hero store see them. */
	static std::vector<int> Reserved()
	{
		std::vector<int> cells;
		for (int cell = 0; cell < InventoryGridCells; cell++) {
			if (MyPlayer->reservedInvCells.test(cell))
				cells.push_back(cell);
		}
		return cells;
	}

	static std::string Stored()
	{
		return HeroStoreGet("weapon_swap.cells").value_or("<none>");
	}

	static std::string Sounds()
	{
		return EvalAs<std::string>("return table.concat(sounds, ' ')").value_or("<error>");
	}

	static std::string ItemSound(std::string_view name)
	{
		return EvalAs<std::string>(StrCat("return 'sfx' .. tostring(require('devilutionx.audio').SfxID.", name, ")")).value_or("<error>");
	}

	static std::string FailureSound()
	{
		return EvalAs<std::string>("return 'say' .. tostring(require('devilutionx.audio').HeroSpeech.ICantDoThat)").value_or("<error>");
	}

	static Item &Left() { return MyPlayer->InvBody[INVLOC_HAND_LEFT]; }
	static Item &Right() { return MyPlayer->InvBody[INVLOC_HAND_RIGHT]; }

	/** @brief The seed of what covers a cell, 0 if nothing does. */
	static uint32_t SeedAt(int cell)
	{
		const int id = std::abs(MyPlayer->InvGrid[cell]);
		return id == 0 ? 0 : MyPlayer->InvList[id - 1]._iSeed;
	}

	/** @brief Everything a failed swap must leave as it was. */
	static std::vector<uint32_t> Snapshot()
	{
		std::vector<uint32_t> state { Left()._iSeed, Right()._iSeed, static_cast<uint32_t>(MyPlayer->_pNumInv) };
		for (int cell = 0; cell < InventoryGridCells; cell++)
			state.push_back(SeedAt(cell));
		return state;
	}

	using Cells = std::vector<int>;
};

TEST_F(WeaponSwapModTest, Mark_Hold_Click_ReservesAndPersists)
{
	Mark({ 12, 3 });

	EXPECT_EQ(Reserved(), (Cells { 3, 12 }));
	EXPECT_EQ(Stored(), "3,12");
}

TEST_F(WeaponSwapModTest, Mark_Click_OnItem_MarksAllItsCells)
{
	PlaceAt(MakeStaff(), 4);

	Mark({ 24 + 1 });

	EXPECT_EQ(Reserved(), (Cells { 4, 5, 14, 15, 24, 25 }));
	EXPECT_TRUE(MyPlayer->HoldItem.isEmpty()) << "A marking click must not pick the item up";
}

TEST_F(WeaponSwapModTest, Mark_ClickAgain_Unmarks)
{
	PlaceAt(MakeStaff(), 4);
	Mark({ 7, 4 });
	Mark({ 15 });

	EXPECT_EQ(Reserved(), (Cells { 7 }));
	EXPECT_EQ(Stored(), "7");

	Mark({ 7 });

	EXPECT_EQ(Reserved(), Cells {});
	EXPECT_EQ(Stored(), "<none>") << "Nothing marked leaves nothing in the save";
}

TEST_F(WeaponSwapModTest, Mark_PartlyMarkedItem_MarksTheRest)
{
	Mark({ 4 });
	PlaceAt(MakeSword(), 4);

	Mark({ 14 });

	EXPECT_EQ(Reserved(), (Cells { 4, 14, 24 }));
}

TEST_F(WeaponSwapModTest, Mark_WithItemOnCursor_ClickConsumed_ItemStays)
{
	MyPlayer->HoldItem = MakePotion();

	Mark({ 5 });

	EXPECT_EQ(Reserved(), (Cells { 5 }));
	EXPECT_FALSE(MyPlayer->HoldItem.isEmpty());
	EXPECT_EQ(MyPlayer->InvGrid[5], 0);
}

TEST_F(WeaponSwapModTest, NoHold_ClickIsNormal)
{
	PlaceAt(MakeSword(), 0);

	Click(0);

	EXPECT_EQ(Reserved(), Cells {});
	EXPECT_FALSE(MyPlayer->HoldItem.isEmpty()) << "The item is picked up as usual";
}

TEST_F(WeaponSwapModTest, MarkKeyReleased_ClickIsNormalAgain)
{
	Mark({ 1 });
	PlaceAt(MakeSword(), 0);

	Click(0);

	EXPECT_EQ(Reserved(), (Cells { 1 }));
	EXPECT_FALSE(MyPlayer->HoldItem.isEmpty());
}

TEST_F(WeaponSwapModTest, Pickup_AvoidsMarkedCells)
{
	KeymapperPress(MarkKey);
	for (int cell = 0; cell < InventoryGridCells; cell++) {
		if (cell != 17)
			Click(cell);
	}
	KeymapperRelease(MarkKey);
	ASSERT_EQ(Reserved().size(), 39U);

	ASSERT_TRUE(AutoPlaceItemInInventory(*MyPlayer, MakePotion()));

	EXPECT_NE(MyPlayer->InvGrid[17], 0);
	EXPECT_EQ(MyPlayer->_pNumInv, 1);
	EXPECT_FALSE(AutoPlaceItemInInventory(*MyPlayer, MakePotion())) << "Only marked cells are left";
	EXPECT_EQ(MyPlayer->_pNumInv, 1);
}

TEST_F(WeaponSwapModTest, Manual_Paste_IntoMarked_Works)
{
	Mark({ 5 });
	MyPlayer->HoldItem = MakePotion();

	Click(5);

	EXPECT_TRUE(MyPlayer->HoldItem.isEmpty());
	EXPECT_NE(MyPlayer->InvGrid[5], 0);
}

TEST_F(WeaponSwapModTest, Swap_SwordForStaff_Spec)
{
	Item staff = MakeStaff();
	Item sword = MakeSword();
	Left() = staff;
	PlaceAt(sword, 0);
	Mark({ 0, 1, 11, 21 });
	ASSERT_EQ(Reserved(), (Cells { 0, 1, 10, 11, 20, 21 }));

	Swap();

	EXPECT_EQ(Left()._iSeed, sword._iSeed);
	for (const int cell : Reserved())
		EXPECT_EQ(SeedAt(cell), staff._iSeed) << cell;
	EXPECT_EQ(Sounds(), ItemSound("ItemSword"));
}

TEST_F(WeaponSwapModTest, Swap_Fails_ThreeCells_StateUnchanged)
{
	Left() = MakeStaff();
	PlaceAt(MakeSword(), 0);
	Mark({ 0 });
	ASSERT_EQ(Reserved(), (Cells { 0, 10, 20 }));
	const std::vector<uint32_t> before = Snapshot();

	Swap();

	EXPECT_EQ(Snapshot(), before);
	EXPECT_EQ(Reserved(), (Cells { 0, 10, 20 }));
	EXPECT_EQ(Sounds(), FailureSound());
}

TEST_F(WeaponSwapModTest, Swap_Partial_KeepsShield)
{
	Item oldSword = MakeSword();
	Item shield = MakeShield();
	Item newSword = MakeSword();
	Left() = oldSword;
	Right() = shield;
	PlaceAt(newSword, 3);
	Mark({ 3 });

	Swap();

	EXPECT_EQ(Left()._iSeed, newSword._iSeed);
	EXPECT_EQ(Right()._iSeed, shield._iSeed);
	EXPECT_EQ(SeedAt(13), oldSword._iSeed);
	EXPECT_EQ(Sounds(), ItemSound("ItemSword"));
}

TEST_F(WeaponSwapModTest, Swap_Shield_Keeps_Sword)
{
	Item sword = MakeSword();
	Item oldShield = MakeShield();
	Item newShield = MakeShield();
	Left() = sword;
	Right() = oldShield;
	PlaceAt(newShield, 0);
	Mark({ 0 });

	Swap();

	EXPECT_EQ(Left()._iSeed, sword._iSeed);
	EXPECT_EQ(Right()._iSeed, newShield._iSeed);
	EXPECT_EQ(Sounds(), ItemSound("ItemShield"));
}

TEST_F(WeaponSwapModTest, Swap_Twice_RoundTrip)
{
	Item sword = MakeSword();
	Item shield = MakeShield();
	Item staff = MakeStaff();
	Left() = sword;
	Right() = shield;
	PlaceAt(staff, 0);
	Mark({ 0, 2, 3, 12 });
	ASSERT_EQ(Reserved().size(), 9U);

	Swap();
	ASSERT_EQ(Left()._iSeed, staff._iSeed);
	ASSERT_TRUE(Right().isEmpty());
	EXPECT_EQ(Sounds(), ItemSound("ItemStaff"));

	Swap();

	EXPECT_EQ(Left()._iSeed, sword._iSeed);
	EXPECT_EQ(Right()._iSeed, shield._iSeed);
	Swap();
	EXPECT_EQ(Left()._iSeed, staff._iSeed);
}

TEST_F(WeaponSwapModTest, Swap_HandsEmpty_Equips)
{
	Item sword = MakeSword();
	PlaceAt(sword, 0);
	Mark({ 0 });

	Swap();

	EXPECT_EQ(Left()._iSeed, sword._iSeed);
	EXPECT_EQ(MyPlayer->_pNumInv, 0);
	EXPECT_EQ(Sounds(), ItemSound("ItemSword"));
}

TEST_F(WeaponSwapModTest, Swap_EmptyArea_Fails)
{
	Left() = MakeSword();
	Mark({ 0, 1 });
	const std::vector<uint32_t> before = Snapshot();

	Swap();

	EXPECT_EQ(Snapshot(), before);
	EXPECT_EQ(Sounds(), FailureSound());
}

TEST_F(WeaponSwapModTest, Swap_NoMarks_Fails)
{
	Left() = MakeSword();

	Swap();

	EXPECT_EQ(Sounds(), FailureSound());
}

TEST_F(WeaponSwapModTest, Swap_UnusableInArea_Blocks)
{
	Item heavy = MakeSword();
	heavy._iMinStr = 100;
	MyPlayer->_pStrength = 50;
	heavy.updateRequiredStatsCacheForPlayer(*MyPlayer);
	PlaceAt(heavy, 0);
	Mark({ 0 });
	const std::vector<uint32_t> before = Snapshot();

	Swap();

	EXPECT_EQ(Snapshot(), before);
	EXPECT_EQ(Sounds(), FailureSound());
}

TEST_F(WeaponSwapModTest, Swap_UnusableInArea_SkippedForUsable)
{
	Item heavy = MakeSword();
	heavy._iMinStr = 100;
	MyPlayer->_pStrength = 50;
	heavy.updateRequiredStatsCacheForPlayer(*MyPlayer);
	Item light = MakeSword();
	light._iMinStr = 0;
	PlaceAt(heavy, 0);
	PlaceAt(light, 1);
	Mark({ 0, 1 });

	Swap();

	EXPECT_EQ(Left()._iSeed, light._iSeed);
	EXPECT_EQ(SeedAt(0), heavy._iSeed);
}

TEST_F(WeaponSwapModTest, Swap_ShieldIn_ReplacesUnusableHeldTwoHander)
{
	Item staff = MakeStaff();
	staff._iMinStr = 100;
	MyPlayer->_pStrength = 50;
	staff.updateRequiredStatsCacheForPlayer(*MyPlayer);
	Item shield = MakeShield();
	Left() = staff;
	PlaceAt(shield, 0);
	Mark({ 0, 2, 12, 22 });
	Mark({ 20 });
	Mark({ 21 });
	ASSERT_EQ(Reserved(), (Cells { 0, 1, 2, 10, 11, 12, 20, 21, 22 }));

	Swap();

	EXPECT_EQ(Sounds(), ItemSound("ItemShield"));
	EXPECT_EQ(Left()._iSeed, shield._iSeed);
	EXPECT_TRUE(Right().isEmpty());
	EXPECT_EQ(MyPlayer->_pNumInv, 1);
}

TEST_F(WeaponSwapModTest, Swap_Bard_DualWield)
{
	Become(HeroClass::Bard);
	Item first = MakeSword();
	Item second = MakeSword();
	Item staff = MakeStaff();
	Left() = staff;
	PlaceAt(first, 0);
	PlaceAt(second, 1);
	Mark({ 0, 1 });
	ASSERT_EQ(Reserved(), (Cells { 0, 1, 10, 11, 20, 21 }));

	Swap();

	EXPECT_EQ(Left()._iSeed, first._iSeed);
	EXPECT_EQ(Right()._iSeed, second._iSeed);
	EXPECT_EQ(SeedAt(0), staff._iSeed);
}

TEST_F(WeaponSwapModTest, Swap_Warrior_TwoSwords_OnlyFirst)
{
	Item first = MakeSword();
	Item second = MakeSword();
	PlaceAt(first, 0);
	PlaceAt(second, 1);
	Mark({ 0, 1 });

	Swap();

	EXPECT_EQ(Left()._iSeed, first._iSeed);
	EXPECT_TRUE(Right().isEmpty());
	EXPECT_EQ(SeedAt(1), second._iSeed);
}

TEST_F(WeaponSwapModTest, Swap_Barbarian_2HSwordShield)
{
	Become(HeroClass::Barbarian);
	Item twoHander = MakeItem(ItemType::Sword, ILOC_TWOHAND);
	const Size size = GetInventorySize(twoHander);
	ASSERT_EQ(size.width, 2);
	Item shield = MakeShield();
	Item sword = MakeSword();
	Left() = twoHander;
	Right() = shield;
	PlaceAt(sword, 0);
	Mark({ 0, 1, 11, 21, 31 });

	Swap();

	EXPECT_EQ(Left()._iSeed, sword._iSeed);
	EXPECT_EQ(Right()._iSeed, shield._iSeed) << "The Barbarian's two-handed sword only takes one hand";
	EXPECT_EQ(SeedAt(0), twoHander._iSeed);
}

TEST_F(WeaponSwapModTest, Swap_FailsWhileAttacking)
{
	Left() = MakeStaff();
	PlaceAt(MakeSword(), 0);
	Mark({ 0, 1, 11, 21 });
	const std::vector<uint32_t> before = Snapshot();
	MyPlayer->_pmode = PM_ATTACK;

	Swap();

	EXPECT_EQ(Snapshot(), before);
	EXPECT_EQ(Sounds(), FailureSound());
}

TEST_F(WeaponSwapModTest, Swap_WhileWalking_Works)
{
	Item sword = MakeSword();
	PlaceAt(sword, 0);
	Mark({ 0 });
	MyPlayer->_pmode = PM_WALK_SIDEWAYS;

	Swap();

	EXPECT_EQ(Left()._iSeed, sword._iSeed);
}

TEST_F(WeaponSwapModTest, Swap_IgnoredWhenDead)
{
	Left() = MakeStaff();
	PlaceAt(MakeSword(), 0);
	Mark({ 0, 1, 11, 21 });
	const std::vector<uint32_t> before = Snapshot();
	MyPlayer->_pmode = PM_DEATH;

	Swap();

	EXPECT_EQ(Snapshot(), before);
	EXPECT_EQ(Sounds(), "");
}

TEST_F(WeaponSwapModTest, Swap_IgnoredWhenPaused)
{
	PlaceAt(MakeSword(), 0);
	Mark({ 0 });
	const std::vector<uint32_t> before = Snapshot();
	PauseMode = 2;

	Swap();

	EXPECT_EQ(Snapshot(), before);
	EXPECT_EQ(Sounds(), "");
}

TEST_F(WeaponSwapModTest, Swap_WithItemOnCursor_LeavesCursorItem)
{
	Item sword = MakeSword();
	Item potion = MakePotion();
	potion._iSeed = 987654;
	PlaceAt(sword, 0);
	Mark({ 0 });
	MyPlayer->HoldItem = potion;

	Swap();

	EXPECT_EQ(Left()._iSeed, sword._iSeed);
	EXPECT_EQ(MyPlayer->HoldItem._iSeed, potion._iSeed);
}

TEST_F(WeaponSwapModTest, Swap_KeyAutoRepeat_SwapsOnce)
{
	Item sword = MakeSword();
	Item staff = MakeStaff();
	Left() = sword;
	PlaceAt(staff, 0);
	Mark({ 0 });

	KeymapperPress(SwapKey);
	KeymapperPress(SwapKey);

	EXPECT_EQ(Left()._iSeed, staff._iSeed);
	KeymapperRelease(SwapKey);
}

TEST_F(WeaponSwapModTest, Swap_NextPhysicalPress_SwapsBack)
{
	Item sword = MakeSword();
	Item staff = MakeStaff();
	Left() = sword;
	PlaceAt(staff, 0);
	Mark({ 0 });

	Swap();
	Swap();

	EXPECT_EQ(Left()._iSeed, sword._iSeed);
}

TEST_F(WeaponSwapModTest, Swap_KeyHeldIntoNewGame_WorksAgain)
{
	Item sword = MakeSword();
	Item staff = MakeStaff();
	Left() = sword;
	PlaceAt(staff, 0);
	Mark({ 0 });
	KeymapperPress(SwapKey);
	lua::GameStart();
	ASSERT_EQ(Left()._iSeed, staff._iSeed);

	KeymapperPress(SwapKey);

	EXPECT_EQ(Left()._iSeed, sword._iSeed);
	KeymapperRelease(SwapKey);
}

TEST_F(WeaponSwapModTest, MarkKeyReleasedDuringLoad_ClickIsNormalAfterLoad)
{
	PlaceAt(MakeSword(), 0);
	KeymapperPress(MarkKey);
	KeyUpDuringLoad(MarkKey);
	ReleaseKeysDeferredByLoad();

	Click(0);

	EXPECT_EQ(Reserved(), Cells {});
}

TEST_F(WeaponSwapModTest, SwapKeyReleasedDuringLoad_NextPressSwaps)
{
	Item sword = MakeSword();
	Item staff = MakeStaff();
	Left() = sword;
	PlaceAt(staff, 0);
	Mark({ 0 });
	KeymapperPress(SwapKey);
	ASSERT_EQ(Left()._iSeed, staff._iSeed);
	KeyUpDuringLoad(SwapKey);
	ReleaseKeysDeferredByLoad();

	Swap();

	EXPECT_EQ(Left()._iSeed, sword._iSeed);
}

TEST_F(WeaponSwapModTest, KeyUpDuringLoad_WaitsForTheLoadToEnd)
{
	PlaceAt(MakeSword(), 0);
	KeymapperPress(MarkKey);
	KeyUpDuringLoad(MarkKey);

	Click(0);

	EXPECT_FALSE(Reserved().empty());
	ReleaseKeysDeferredByLoad();
}

TEST_F(WeaponSwapModTest, Rim_FollowsTheWholeArea_OnTheGridLines)
{
	Mark({ 0, 1, 10, 11, 20, 21 });

	lua::InventoryDrawComplete();

	// 4 sides in one color, and no line between two marked cells.
	EXPECT_EQ(EvalAs<int>("return #rects"), 4);
	EXPECT_TRUE(EvalAs<bool>("return rects[1][5] == rects[2][5] and rects[1][5] == rects[3][5] and rects[1][5] == rects[4][5]").value());
	const char *check = R"lua(
		local x, y = require('devilutionx.render').inventoryCellRect(0)
		local found = {}
		for _, r in ipairs(rects) do found[table.concat({ r[1] - x, r[2] - y, r[3], r[4] }, ',')] = true end
		-- Grid lines around the area: x = -1 and 57, y = -2 and 85.
		local n = 0
		for _, key in ipairs({ '-1,-2,59,1', '-1,85,59,1', '-1,-2,1,88', '57,-2,1,88' }) do
			if found[key] then n = n + 1 end
		end
		return n
	)lua";
	EXPECT_EQ(EvalAs<int>(check), 4);
}

TEST_F(WeaponSwapModTest, Rim_BrighterWhileMarking_NothingWithoutMarks)
{
	lua::InventoryDrawComplete();
	EXPECT_EQ(EvalAs<int>("return #rects"), 0);

	Mark({ 3 });
	lua::InventoryDrawComplete();
	const std::string normal = EvalAs<std::string>("local c = rects[1][5]; for k in pairs(rects) do rects[k] = nil end; return tostring(c)").value();

	KeymapperPress(MarkKey);
	lua::InventoryDrawComplete();
	KeymapperRelease(MarkKey);

	EXPECT_EQ(EvalAs<int>("return #rects"), 4);
	EXPECT_NE(EvalAs<std::string>("return tostring(rects[1][5])").value(), normal);
}

TEST_F(WeaponSwapModTest, Rim_FollowsUnmarking)
{
	Mark({ 3, 4 });
	Mark({ 4 });

	lua::InventoryDrawComplete();

	EXPECT_EQ(EvalAs<int>("return #rects"), 4);
	EXPECT_EQ(EvalAs<int>("return rects[1][3]"), 30);
}

TEST_F(WeaponSwapModTest, Stone_OnePieceOfBareStonePerMarkedCell_DarkenedLessWhileMarking)
{
	ASSERT_TRUE(Eval(R"lua(
		blits = {}
		local render = require('devilutionx.render')
		render.inventoryPanelBlit = function(sx, sy, w, h, x, y, shade) table.insert(blits, { sx, sy, w, h, x, y, shade }) end
	)lua")
	                .valid());
	Mark({ 3, 17 });

	lua::InventoryDrawBackground();

	EXPECT_EQ(EvalAs<int>("return #blits"), 2);
	const char *check = R"lua(
		local render = require('devilutionx.render')
		local cellX = {}
		for _, cell in ipairs({ 3, 17 }) do
			local x, y = render.inventoryCellRect(cell)
			-- The grid art's 3 pixel wide lines stay visible around this.
			cellX[(x + 1) .. ',' .. y] = true
		end
		local ok = 0
		for _, b in ipairs(blits) do
			-- Bare stone: below and right of the frame, left of the silhouette, above the left hand slot.
			if cellX[b[5] .. ',' .. b[6]] and b[1] >= 7 and b[1] + b[3] <= 110 and b[2] >= 11 and b[2] + b[4] <= 72
			    and b[3] == 26 and b[4] == 26 and b[7] > 0 then
				ok = ok + 1
			end
		end
		return ok
	)lua";
	EXPECT_EQ(EvalAs<int>(check), 2);

	const int shade = EvalAs<int>("return blits[1][7]").value();
	KeymapperPress(MarkKey);
	lua::InventoryDrawBackground();
	KeymapperRelease(MarkKey);
	EXPECT_LT(EvalAs<int>("return blits[#blits][7]").value(), shade);
}

TEST_F(WeaponSwapModTest, GameStart_RestoresMarksFromHerostore)
{
	HeroStoreSet("weapon_swap.cells", "2,30");

	lua::GameStart();

	EXPECT_EQ(Reserved(), (Cells { 2, 30 }));
	EXPECT_EQ(Stored(), "2,30");
}

TEST_F(WeaponSwapModTest, GameStart_BadStoredCells_Ignored)
{
	HeroStoreSet("weapon_swap.cells", "2,40,999,x");

	lua::GameStart();

	EXPECT_EQ(Reserved(), (Cells { 2 }));
	Mark({ 7 });
	EXPECT_EQ(Stored(), "2,7") << "Bad cells do not come back";
}

TEST_F(WeaponSwapModTest, OtherHero_NoMarks)
{
	Mark({ 2 });
	ClearHeroStore();

	lua::GameStart();

	EXPECT_EQ(Reserved(), Cells {});
	EXPECT_EQ(Stored(), "<none>");
}

TEST_F(WeaponSwapModTest, GameStart_ResetsStuckMarking)
{
	KeymapperPress(MarkKey);
	lua::GameStart();
	PlaceAt(MakeSword(), 0);

	Click(0);

	EXPECT_EQ(Reserved(), Cells {});
	KeymapperRelease(MarkKey);
}

TEST_F(WeaponSwapModTest, Marks_SurviveSaveAndReload)
{
	constexpr uint32_t SaveNumber = 7;
	gbVanilla = false;
	giNumberOfLevels = 17;
	gSaveNumber = SaveNumber;
	RemoveFile(StrCat(paths::PrefPath(), gbIsSpawn ? "spawn_" : "single_", SaveNumber, ".sv").c_str());
	_uiheroinfo info {};
	info.heroclass = HeroClass::Warrior;
	info.saveNumber = SaveNumber;
	CopyUtf8(info.name, "Hero", sizeof(info.name));
	FreeCursor();
	ASSERT_TRUE(pfile_ui_save_create(&info));
	InitCursor();
	lua::GameStart();
	Mark({ 4, 9 });
	pfile_write_hero();

	ClearHeroStore();
	MyPlayer->reservedInvCells.reset();
	FreeCursor();
	pfile_read_player_from_save(SaveNumber, *MyPlayer);
	InitCursor();
	lua::GameStart();

	EXPECT_EQ(Reserved(), (Cells { 4, 9 }));
	StripPlayer();
	GivePlentyOfStats();
}

TEST_F(WeaponSwapModTest, Mod_OffAndOnAgain_StillWorks)
{
	FindModEntry()->SetValue(false);
	EXPECT_EQ(GetOptions().Keymapper.findAction(static_cast<uint32_t>(SwapKey)), nullptr);
	FindModEntry()->SetValue(true);
	ASSERT_TRUE(Eval(Spies).valid());
	lua::GameStart();
	Item sword = MakeSword();
	PlaceAt(sword, 0);
	Mark({ 0 });

	Swap();

	EXPECT_EQ(Left()._iSeed, sword._iSeed);
}

class WeaponSwapModOffTest : public LuaUITest {
};

TEST_F(WeaponSwapModOffTest, OffByDefault_NoActions_NoMask)
{
	OptionEntryBoolean *mod = FindModEntry();
	ASSERT_NE(mod, nullptr) << "weapon_swap must be listed in Settings";
	EXPECT_FALSE(**mod);
	EXPECT_EQ(GetOptions().Keymapper.findAction(static_cast<uint32_t>(SwapKey)), nullptr);
	EXPECT_EQ(GetOptions().Keymapper.findAction(static_cast<uint32_t>(MarkKey)), nullptr);
}

class WeaponSwapDownloadedTest : public WeaponSwapModTest {
protected:
	static std::filesystem::path ModFolder()
	{
		return std::filesystem::path(paths::PrefPath()) / "mods" / "weapon_swap";
	}

	static void Install(const char *file, std::string_view marker)
	{
		std::ifstream in(StrCat(paths::AssetsPath(), "lua/mods/weapon_swap/", file));
		std::stringstream source;
		source << in.rdbuf();
		std::ofstream out(ModFolder() / "lua" / "mods" / "weapon_swap" / file);
		out << "require('devilutionx.herostore').set('downloaded." << marker << "', 'yes')\n"
		    << source.str();
	}

	static void SetUpTestSuite()
	{
		LuaUITest::SetUpTestSuite();
		if (!HaveMainData())
			return;
		std::filesystem::create_directories(ModFolder() / "lua" / "mods" / "weapon_swap");
		Install("init.lua", "init");
		Install("logic.lua", "logic");
		ClearHeroStore();
		FindModEntry()->SetValue(true);
	}

	static void TearDownTestSuite()
	{
		if (!HaveMainData())
			return;
		WeaponSwapModTest::TearDownTestSuite();
		std::filesystem::remove_all(ModFolder());
	}

	void SetUp() override
	{
		if (!HaveMainData()) {
			WeaponSwapModTest::SetUp();
			return;
		}
		loadedInit_ = HeroStoreGet("downloaded.init").has_value();
		loadedLogic_ = HeroStoreGet("downloaded.logic").has_value();
		WeaponSwapModTest::SetUp();
	}

	bool loadedInit_ = false;
	bool loadedLogic_ = false;
};

TEST_F(WeaponSwapDownloadedTest, DownloadedFolder_LoadsAndSwaps)
{
	EXPECT_TRUE(loadedInit_);
	EXPECT_TRUE(loadedLogic_);
	Item sword = MakeSword();
	PlaceAt(sword, 0);
	Mark({ 0 });

	Swap();

	EXPECT_EQ(Left()._iSeed, sword._iSeed);
}

} // namespace
} // namespace devilution
