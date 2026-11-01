/**
 * @file lua_inventory_api_test.cpp
 *
 * Tests for the Lua inventory API: the Player methods, the Item fields and the enums.
 *
 * Mods are not trusted: bad input has to give nil, false or "invalid", never a Lua error.
 */

#include <array>
#include <optional>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "lua_test_fixture.hpp"

#include "cursor.h"
#include "engine/assets.hpp"
#include "inv.h"
#include "items.h"
#include "player.h"
#include "sound_effect_enums.h"
#include "utils/paths.h"
#include "utils/str_cat.hpp"

namespace devilution {
namespace {

constexpr char Prelude[] = R"lua(
local player = require('devilutionx.player')
local items = require('devilutionx.items')
local audio = require('devilutionx.audio')
local p = player.self()
)lua";

/** Everything that isn't an inventory cell. 4294967296 is cell 0 if cut down to 32 bits. */
const auto BadCells = { "-1", "40", "1.5", "2^53", "1e308", "0/0", "1/0", "4294967296", "'0'", "true", "nil", "{}", "print", "p", "" };

/** Everything that isn't an item, as an only argument. */
const auto BadItems = { "0", "'x'", "true", "nil", "{}", "print", "p", "" };

/** A first argument a call with '.' instead of ':' can give, then the rest. */
const auto WrongSelves = { "", "0", "'p', 0", "{}, 0", "nil, 0", "print, 0" };

class LuaInventoryApiTest : public LuaUITest {
protected:
	void SetUp() override
	{
		LuaUITest::SetUp();
		if (IsSkipped())
			return;

		StripPlayer();
		GivePlentyOfStats();
	}

	/** @brief Run code that sees `player`, `items` and `audio`, and `p`, the local player. */
	template <typename T>
	static std::optional<T> Run(const std::string &code)
	{
		return EvalAs<T>(Prelude + code);
	}
};

TEST_F(LuaInventoryApiTest, Fixture_DoesNotUseTheUsersConfig)
{
	EXPECT_NE(paths::ConfigPath().find("devilutionx_lua_test_"), std::string::npos);
	EXPECT_NE(paths::PrefPath().find("devilutionx_lua_test_"), std::string::npos);
}

TEST_F(LuaInventoryApiTest, InvBodyLoc_MatchesTheBodySlots)
{
	EXPECT_EQ(Run<int>("return items.InvBodyLoc.Head"), INVLOC_HEAD);
	EXPECT_EQ(Run<int>("return items.InvBodyLoc.RingLeft"), INVLOC_RING_LEFT);
	EXPECT_EQ(Run<int>("return items.InvBodyLoc.RingRight"), INVLOC_RING_RIGHT);
	EXPECT_EQ(Run<int>("return items.InvBodyLoc.Amulet"), INVLOC_AMULET);
	EXPECT_EQ(Run<int>("return items.InvBodyLoc.HandLeft"), INVLOC_HAND_LEFT);
	EXPECT_EQ(Run<int>("return items.InvBodyLoc.HandRight"), INVLOC_HAND_RIGHT);
	EXPECT_EQ(Run<int>("return items.InvBodyLoc.Chest"), INVLOC_CHEST);
}

TEST_F(LuaInventoryApiTest, GetBodyItem_ReturnsTheEquippedItem)
{
	Item sword = MakeSword();
	sword._iSeed = 1234;
	MyPlayer->InvBody[INVLOC_HAND_LEFT] = sword;

	EXPECT_EQ(Run<int>("return p:getBodyItem(items.InvBodyLoc.HandLeft).seed"), 1234);
}

TEST_F(LuaInventoryApiTest, GetBodyItem_IsALiveReference)
{
	MyPlayer->InvBody[INVLOC_HAND_LEFT] = MakeSword();

	Run<int>("p:getBodyItem(items.InvBodyLoc.HandLeft).seed = 77 return 0");

	EXPECT_EQ(MyPlayer->InvBody[INVLOC_HAND_LEFT]._iSeed, 77);
}

TEST_F(LuaInventoryApiTest, GetBodyItem_EmptySlot_ReturnsNil)
{
	EXPECT_EQ(Run<bool>("return p:getBodyItem(items.InvBodyLoc.HandLeft) == nil"), true);
}

TEST_F(LuaInventoryApiTest, GetBodyItem_BadLocation_ReturnsNil)
{
	MyPlayer->InvBody[INVLOC_HEAD] = MakeSword();

	for (const char *location : { "-1", "7", "1.5", "2^53", "1e308", "0/0", "1/0", "4294967296", "'0'", "true", "nil", "{}", "print", "p", "" }) {
		SCOPED_TRACE(location);
		EXPECT_EQ(Run<bool>(StrCat("return p:getBodyItem(", location, ") == nil")), true);
	}
}

TEST_F(LuaInventoryApiTest, GetBodyItem_IntegerValuedFloat_IsAccepted)
{
	MyPlayer->InvBody[INVLOC_HEAD] = MakeSword();

	EXPECT_EQ(Run<bool>("return p:getBodyItem(0.0) ~= nil"), true);
}

TEST_F(LuaInventoryApiTest, GetBodyItem_WrongSelf_ReturnsNil)
{
	for (const char *args : WrongSelves) {
		SCOPED_TRACE(args);
		EXPECT_EQ(Run<bool>(StrCat("return p.getBodyItem(", args, ") == nil")), true);
	}
}

TEST_F(LuaInventoryApiTest, GetInventoryItemAt_AnyCellOfTheItem_ReturnsIt)
{
	Item staff = MakeStaff();
	staff._iSeed = 1234;
	PlaceAt(staff, 12);

	for (const int cell : { 12, 13, 22, 23, 32, 33 }) {
		SCOPED_TRACE(cell);
		EXPECT_EQ(Run<int>(StrCat("return p:getInventoryItemAt(", cell, ").seed")), 1234);
	}
}

TEST_F(LuaInventoryApiTest, GetInventoryItemAt_EmptyCell_ReturnsNil)
{
	PlaceAt(MakeStaff(), 12);

	EXPECT_EQ(Run<bool>("return p:getInventoryItemAt(14) == nil"), true);
}

TEST_F(LuaInventoryApiTest, GetInventoryItemAt_BadCell_ReturnsNil)
{
	PlaceAt(MakeSword(), 0);

	for (const char *cell : BadCells) {
		SCOPED_TRACE(cell);
		EXPECT_EQ(Run<bool>(StrCat("return p:getInventoryItemAt(", cell, ") == nil")), true);
	}
}

TEST_F(LuaInventoryApiTest, GetInventoryItemAt_WrongSelf_ReturnsNil)
{
	for (const char *args : WrongSelves) {
		SCOPED_TRACE(args);
		EXPECT_EQ(Run<bool>(StrCat("return p.getInventoryItemAt(", args, ") == nil")), true);
	}
}

TEST_F(LuaInventoryApiTest, GetInventoryItemCells_ListsAllCellsAscending_FromAnyCell)
{
	PlaceAt(MakeSword(), 11);
	PlaceAt(MakeStaff(), 12);
	PlaceAt(MakeShield(), 14);

	for (const int cell : { 12, 13, 22, 23, 32, 33 }) {
		SCOPED_TRACE(cell);
		EXPECT_EQ(Run<std::string>(StrCat("return table.concat(p:getInventoryItemCells(", cell, "), ',')")), "12,13,22,23,32,33");
	}
}

TEST_F(LuaInventoryApiTest, GetInventoryItemCells_IsAPlainLuaSequence)
{
	PlaceAt(MakeShield(), 0);

	EXPECT_EQ(Run<std::string>(R"lua(
		local cells = p:getInventoryItemCells(0)
		return type(cells) .. ' ' .. #cells .. ' ' .. math.type(cells[1]) .. ' ' .. cells[1]
	)lua"),
	    "table 4 integer 0");
}

TEST_F(LuaInventoryApiTest, GetInventoryItemCells_EmptyCell_ReturnsNil)
{
	PlaceAt(MakeShield(), 0);

	EXPECT_EQ(Run<bool>("return p:getInventoryItemCells(2) == nil"), true);
}

TEST_F(LuaInventoryApiTest, GetInventoryItemCells_BadCell_ReturnsNil)
{
	PlaceAt(MakeSword(), 0);

	for (const char *cell : BadCells) {
		SCOPED_TRACE(cell);
		EXPECT_EQ(Run<bool>(StrCat("return p:getInventoryItemCells(", cell, ") == nil")), true);
	}
}

TEST_F(LuaInventoryApiTest, GetInventoryItemCells_WrongSelf_ReturnsNil)
{
	for (const char *args : WrongSelves) {
		SCOPED_TRACE(args);
		EXPECT_EQ(Run<bool>(StrCat("return p.getInventoryItemCells(", args, ") == nil")), true);
	}
}

TEST_F(LuaInventoryApiTest, GetItemLocation_TwoHandSword_DependsOnTheClass)
{
	PlaceAt(MakeItem(ItemType::Sword, ILOC_TWOHAND), 0);
	EXPECT_EQ(Run<bool>("return p:getItemLocation(p:getInventoryItemAt(0)) == items.ItemEquipType.TwoHand"), true);

	Become(HeroClass::Barbarian);
	PlaceAt(MakeItem(ItemType::Sword, ILOC_TWOHAND), 0);
	EXPECT_EQ(Run<bool>("return p:getItemLocation(p:getInventoryItemAt(0)) == items.ItemEquipType.OneHand"), true);
}

TEST_F(LuaInventoryApiTest, GetItemLocation_Barbarian_BowStaysTwoHanded)
{
	Become(HeroClass::Barbarian);
	PlaceAt(MakeItem(ItemType::Bow, ILOC_TWOHAND), 0);

	EXPECT_EQ(Run<bool>("return p:getItemLocation(p:getInventoryItemAt(0)) == items.ItemEquipType.TwoHand"), true);
}

TEST_F(LuaInventoryApiTest, GetItemLocation_BadItem_ReturnsNil)
{
	for (const char *item : BadItems) {
		SCOPED_TRACE(item);
		EXPECT_EQ(Run<bool>(StrCat("return p:getItemLocation(", item, ") == nil")), true);
	}
}

TEST_F(LuaInventoryApiTest, GetItemLocation_WrongSelf_ReturnsNil)
{
	PlaceAt(MakeSword(), 0);

	for (const char *self : { "0", "nil", "'p'", "p:getInventoryItemAt(0)" }) {
		SCOPED_TRACE(self);
		EXPECT_EQ(Run<bool>(StrCat("return p.getItemLocation(", self, ", p:getInventoryItemAt(0)) == nil")), true);
	}
}

TEST_F(LuaInventoryApiTest, CanUseItem_FollowsThePlayersStats)
{
	Item sword = MakeSword();
	sword._iMinStr = 100;
	PlaceAt(sword, 0);

	EXPECT_EQ(Run<bool>("return p:canUseItem(p:getInventoryItemAt(0))"), true);

	MyPlayer->_pStrength = 99;
	EXPECT_EQ(Run<bool>("return p:canUseItem(p:getInventoryItemAt(0))"), false);
}

TEST_F(LuaInventoryApiTest, CanUseItem_BadItem_ReturnsFalse)
{
	for (const char *item : BadItems) {
		SCOPED_TRACE(item);
		EXPECT_EQ(Run<bool>(StrCat("return p:canUseItem(", item, ")")), false);
	}
}

TEST_F(LuaInventoryApiTest, CanUseItem_WrongSelf_ReturnsFalse)
{
	PlaceAt(MakeSword(), 0);

	for (const char *self : { "0", "nil", "'p'", "p:getInventoryItemAt(0)" }) {
		SCOPED_TRACE(self);
		EXPECT_EQ(Run<bool>(StrCat("return p.canUseItem(", self, ", p:getInventoryItemAt(0))")), false);
	}
}

TEST_F(LuaInventoryApiTest, SetInventoryCellReserved_AffectsAutomaticPlacement)
{
	for (int cell = 1; cell < InventoryGridCells; cell++)
		PlaceAt(MakePotion(), cell);
	const Item potion = MakePotion();
	ASSERT_TRUE(CanFitItemInInventory(*MyPlayer, potion));

	Eval(StrCat(Prelude, "p:setInventoryCellReserved(0, true)"));
	EXPECT_FALSE(CanFitItemInInventory(*MyPlayer, potion));

	Eval(StrCat(Prelude, "p:setInventoryCellReserved(0, false)"));
	EXPECT_TRUE(CanFitItemInInventory(*MyPlayer, potion));
}

TEST_F(LuaInventoryApiTest, SetInventoryCellReserved_LastCell)
{
	Eval(StrCat(Prelude, "p:setInventoryCellReserved(39, true)"));

	EXPECT_EQ(MyPlayer->reservedInvCells.count(), 1);
	EXPECT_TRUE(MyPlayer->reservedInvCells.test(39));
}

TEST_F(LuaInventoryApiTest, SetInventoryCellReserved_BadCell_ChangesNothing)
{
	for (const char *cell : BadCells) {
		SCOPED_TRACE(cell);
		EXPECT_EQ(Run<bool>(StrCat("return p:setInventoryCellReserved(", cell, cell[0] == '\0' ? "" : ",", " true) == nil")), true);
	}

	EXPECT_TRUE(MyPlayer->reservedInvCells.none());
}

TEST_F(LuaInventoryApiTest, SetInventoryCellReserved_BadFlag_ChangesNothing)
{
	MyPlayer->reservedInvCells.set(3);

	for (const char *flag : { "0", "1", "'false'", "nil", "{}", "print", "p" }) {
		SCOPED_TRACE(flag);
		EXPECT_EQ(Run<bool>(StrCat("return p:setInventoryCellReserved(3, ", flag, ") == nil")), true);
		EXPECT_EQ(Run<bool>(StrCat("return p:setInventoryCellReserved(4, ", flag, ") == nil")), true);
	}
	EXPECT_EQ(Run<bool>("return p:setInventoryCellReserved(3) == nil"), true);

	EXPECT_EQ(MyPlayer->reservedInvCells.count(), 1);
	EXPECT_TRUE(MyPlayer->reservedInvCells.test(3));
}

TEST_F(LuaInventoryApiTest, SetInventoryCellReserved_WrongSelf_ChangesNothing)
{
	for (const char *self : { "0", "nil", "'p'", "{}" }) {
		SCOPED_TRACE(self);
		EXPECT_EQ(Run<bool>(StrCat("return p.setInventoryCellReserved(", self, ", 3, true) == nil")), true);
	}

	EXPECT_TRUE(MyPlayer->reservedInvCells.none());
}

TEST_F(LuaInventoryApiTest, IsInventoryCellReserved_ReadsTheFlag)
{
	MyPlayer->reservedInvCells.set(3);
	MyPlayer->reservedInvCells.set(39);

	EXPECT_EQ(Run<bool>("return p:isInventoryCellReserved(3)"), true);
	EXPECT_EQ(Run<bool>("return p:isInventoryCellReserved(39)"), true);
	EXPECT_EQ(Run<bool>("return p:isInventoryCellReserved(4)"), false);
}

TEST_F(LuaInventoryApiTest, IsInventoryCellReserved_BadCell_ReturnsFalse)
{
	MyPlayer->reservedInvCells.set();

	for (const char *cell : BadCells) {
		SCOPED_TRACE(cell);
		EXPECT_EQ(Run<bool>(StrCat("return p:isInventoryCellReserved(", cell, ")")), false);
	}
}

TEST_F(LuaInventoryApiTest, IsInventoryCellReserved_WrongSelf_ReturnsFalse)
{
	MyPlayer->reservedInvCells.set();

	for (const char *args : WrongSelves) {
		SCOPED_TRACE(args);
		EXPECT_EQ(Run<bool>(StrCat("return p.isInventoryCellReserved(", args, ")")), false);
	}
}

TEST_F(LuaInventoryApiTest, InventorySize_IsExposed)
{
	EXPECT_EQ(Run<int>("return player.InventoryColumns"), 10);
	EXPECT_EQ(Run<int>("return player.InventoryRows"), 4);
}

TEST_F(LuaInventoryApiTest, HeroSpeech_HasEveryLine)
{
	EXPECT_EQ(Run<int>("return audio.HeroSpeech.ChamberOfBoneLore"), 0);
	EXPECT_EQ(Run<int>("return audio.HeroSpeech.ICantDoThat"), static_cast<int>(HeroSpeech::ICantDoThat));
	EXPECT_EQ(Run<int>("return audio.HeroSpeech.AuughUh"), static_cast<int>(HeroSpeech::AuughUh));
	EXPECT_EQ(Run<int>("local count = 0 for _ in pairs(audio.HeroSpeech) do count = count + 1 end return count"), static_cast<int>(HeroSpeech::LAST) + 1);
}

TEST_F(LuaInventoryApiTest, Say_TakesAHeroSpeech)
{
	EXPECT_EQ(Run<bool>("return p:say(audio.HeroSpeech.ICantDoThat) == nil"), true);
}

TEST_F(LuaInventoryApiTest, Say_BadSpeech_DoesNothing)
{
	for (const char *speech : { "-1", "1.5", "256", "4294967296", "0/0", "'0'", "true", "nil", "{}", "print", "p", "" }) {
		SCOPED_TRACE(speech);
		EXPECT_EQ(Run<bool>(StrCat("return p:say(", speech, ") == nil")), true);
	}
}

TEST_F(LuaInventoryApiTest, Say_WrongSelf_DoesNothing)
{
	for (const char *args : WrongSelves) {
		SCOPED_TRACE(args);
		EXPECT_EQ(Run<bool>(StrCat("return p.say(", args, ") == nil")), true);
	}
}

TEST_F(LuaInventoryApiTest, ItemSize_IsItsSizeInCells)
{
	PlaceAt(MakeStaff(), 0);
	PlaceAt(MakeSword(), 2);
	PlaceAt(MakeShield(), 3);
	PlaceAt(MakePotion(), 5);

	EXPECT_EQ(Run<std::string>("local i = p:getInventoryItemAt(0) return i.invWidth .. 'x' .. i.invHeight"), "2x3");
	EXPECT_EQ(Run<std::string>("local i = p:getInventoryItemAt(2) return i.invWidth .. 'x' .. i.invHeight"), "1x3");
	EXPECT_EQ(Run<std::string>("local i = p:getInventoryItemAt(3) return i.invWidth .. 'x' .. i.invHeight"), "2x2");
	EXPECT_EQ(Run<std::string>("local i = p:getInventoryItemAt(5) return i.invWidth .. 'x' .. i.invHeight"), "1x1");
}

TEST_F(LuaInventoryApiTest, ItemSfx_IsTheSoundOfPlacingIt_AndPlaysInAudio)
{
	PlaceAt(MakeStaff(), 0);
	PlaceAt(MakeSword(), 2);
	PlaceAt(MakeShield(), 3);
	PlaceAt(MakePotion(), 5);

	EXPECT_EQ(Run<bool>("return p:getInventoryItemAt(0).invSfx == audio.SfxID.ItemStaff"), true);
	EXPECT_EQ(Run<bool>("return p:getInventoryItemAt(2).invSfx == audio.SfxID.ItemSword"), true);
	EXPECT_EQ(Run<bool>("return p:getInventoryItemAt(3).invSfx == audio.SfxID.ItemShield"), true);
	EXPECT_EQ(Run<bool>("return p:getInventoryItemAt(5).invSfx == audio.SfxID.ItemPotion"), true);
	EXPECT_EQ(Run<bool>("return audio.playSfx(p:getInventoryItemAt(0).invSfx) == nil"), true);
}

TEST_F(LuaInventoryApiTest, ItemFields_CursorWithoutGraphic_AreEmpty)
{
	PlaceAt(MakePotion(), 0);

	// Past the end of the cursor tables, up to ICURS_DEFAULT.
	for (const int cursor : { 227, 228, 229, 254, 255 }) {
		SCOPED_TRACE(cursor);
		MyPlayer->InvList[0]._iCurs = cursor;
		EXPECT_EQ(Run<bool>("return p:getInventoryItemAt(0).invSfx == audio.SfxID.None"), true);
	}

	for (const int cursor : { 254, 255 }) {
		SCOPED_TRACE(cursor);
		MyPlayer->InvList[0]._iCurs = cursor;
		EXPECT_EQ(Run<bool>("return p:getInventoryItemAt(0).invWidth == nil"), true);
		EXPECT_EQ(Run<bool>("return p:getInventoryItemAt(0).invHeight == nil"), true);
	}
}

TEST_F(LuaInventoryApiTest, ItemSfx_LastCursor_HasASound)
{
	PlaceAt(MakePotion(), 0);
	MyPlayer->InvList[0]._iCurs = ICURS_BOVINE;

	EXPECT_EQ(Run<bool>("return p:getInventoryItemAt(0).invSfx ~= audio.SfxID.None"), true);
}

TEST_F(LuaInventoryApiTest, HasInvItemSprite_OnlyForIndicesStartingAtOne)
{
	EXPECT_FALSE(HasInvItemSprite(-1));
	EXPECT_FALSE(HasInvItemSprite(0));
	EXPECT_TRUE(HasInvItemSprite(1));
}

TEST_F(LuaInventoryApiTest, ItemSize_HellfireCursorWithoutHellfire_IsNil)
{
	if (HaveHellfire())
		GTEST_SKIP() << "Needs a game without hellfire.mpq";
	PlaceAt(MakePotion(), 0);
	MyPlayer->InvList[0]._iCurs = 200;

	EXPECT_EQ(Run<bool>("return p:getInventoryItemAt(0).invWidth == nil"), true);
	EXPECT_EQ(Run<bool>("return p:getInventoryItemAt(0).invHeight == nil"), true);
}

TEST_F(LuaInventoryApiTest, ItemSize_SpritesNotLoaded_IsNil)
{
	PlaceAt(MakePotion(), 0);

	FreeCursor();
	const std::optional<bool> width = Run<bool>("return p:getInventoryItemAt(0).invWidth == nil");
	const std::optional<bool> height = Run<bool>("return p:getInventoryItemAt(0).invHeight == nil");
	InitCursor();

	EXPECT_EQ(width, true);
	EXPECT_EQ(height, true);
}

TEST_F(LuaInventoryApiTest, EveryAddition_IsDocumented)
{
	for (const char *key : { "getBodyItem", "getInventoryItemAt", "getInventoryItemCells", "getItemLocation", "canUseItem",
	         "setInventoryCellReserved", "isInventoryCellReserved", "say" }) {
		SCOPED_TRACE(key);
		EXPECT_EQ(Run<bool>(StrCat("return #p.__doc_", key, " > 0 and #p.__sig_", key, " > 0")), true);
	}

	PlaceAt(MakePotion(), 0);
	for (const char *key : { "invWidth", "invHeight", "invSfx" }) {
		SCOPED_TRACE(key);
		EXPECT_EQ(Run<bool>(StrCat("local i = p:getInventoryItemAt(0) return #i.__doc_", key, " > 0 and #i.__sig_", key, " > 0")), true);
	}

	EXPECT_EQ(Run<bool>("return #player.__doc_InventoryColumns > 0"), true);
	EXPECT_EQ(Run<bool>("return #player.__doc_InventoryRows > 0"), true);
	EXPECT_EQ(Run<bool>("return #items.__doc_InvBodyLoc > 0"), true);
	EXPECT_EQ(Run<bool>("return #audio.__doc_HeroSpeech > 0"), true);
}

} // namespace
} // namespace devilution
