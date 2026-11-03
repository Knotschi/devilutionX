/**
 * @file lua_inventory_events_test.cpp
 *
 * Tests for the Lua inventory events and the render functions a mod draws on the inventory with.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "lua_test_fixture.hpp"

#include "control/control.hpp"
#include "engine/dx.h"
#include "engine/light_tables.hpp"
#include "engine/palette.h"
#include "engine/surface.hpp"
#include "game_mode.hpp"
#include "gamemenu.h"
#include "inv.h"
#include "player.h"
#include "tables/misdat.h"
#include "tables/monstdat.h"
#include "tables/objdat.h"
#include "utils/display.h"
#include "utils/paths.h"
#include "utils/str_cat.hpp"
#include "utils/ui_fwd.h"

using ::testing::IsEmpty;
using ::testing::Not;

namespace devilution {
namespace {

/** What a test buffer holds before anything is drawn on it. */
constexpr uint8_t Unpainted = 7;

/**
 * @brief The game's back buffer for one test, in the middle of a surface whose margin must stay clean.
 */
class ScopedBackBuffer {
public:
	static constexpr int Margin = 8;

	ScopedBackBuffer(int width, int height)
	    : previous_(PalSurface)
	    , previousSize_ { gnScreenWidth, gnScreenHeight }
	    , buffer_(width + 2 * Margin, height + 2 * Margin)
	    , allocation_(static_cast<uint8_t *>(buffer_.surface->pixels))
	    , size_ { width, height }
	{
		for (int y = 0; y < buffer_.h(); y++) {
			for (int x = 0; x < buffer_.w(); x++) {
				*buffer_.at(x, y) = Unpainted;
			}
		}
		buffer_.surface->pixels = allocation_ + Margin * buffer_.pitch() + Margin;
		PalSurface = buffer_.surface;
		AdjustToScreenGeometry(size_);
	}

	~ScopedBackBuffer()
	{
		buffer_.surface->pixels = allocation_;
		PalSurface = previous_;
		AdjustToScreenGeometry(previousSize_);
	}

	ScopedBackBuffer(const ScopedBackBuffer &) = delete;
	ScopedBackBuffer &operator=(const ScopedBackBuffer &) = delete;

	/** @param x From -Margin up to the width plus Margin. */
	[[nodiscard]] uint8_t Pixel(int x, int y) const
	{
		return allocation_[(y + Margin) * buffer_.pitch() + x + Margin];
	}

	[[nodiscard]] int CountPaintedOnScreen() const
	{
		return CountPaintedIn(0, 0, size_.width, size_.height);
	}

	/** @brief Painted pixels the game can't see, each of them a write out of bounds. */
	[[nodiscard]] int CountPaintedInMargin() const
	{
		return CountPaintedIn(-Margin, -Margin, size_.width + 2 * Margin, size_.height + 2 * Margin) - CountPaintedOnScreen();
	}

	[[nodiscard]] int CountPainted() const
	{
		return CountPaintedOnScreen() + CountPaintedInMargin();
	}

private:
	[[nodiscard]] int CountPaintedIn(int left, int top, int width, int height) const
	{
		int count = 0;
		for (int y = top; y < top + height; y++) {
			for (int x = left; x < left + width; x++) {
				if (Pixel(x, y) != Unpainted)
					count++;
			}
		}
		return count;
	}

	SDL_Surface *previous_;
	Size previousSize_;
	OwnedSurface buffer_;
	uint8_t *allocation_;
	Size size_;
};

/** Shared by all tests of a suite, so a test has to drop what it registers. */
constexpr char Prelude[] = R"lua(
events = require('devilutionx.events')
render = require('devilutionx.render')
clicks = {}
registered = {}

function on(name, handler)
  events[name].add(handler)
  table.insert(registered, { name, handler })
end

function dropHandlers()
  for _, entry in ipairs(registered) do
    events[entry[1]].remove(entry[2])
  end
end

function recordClick(cell)
  table.insert(clicks, cell)
end
)lua";

class LuaInventoryEventsTest : public LuaUITest {
protected:
	void SetUp() override
	{
		LuaUITest::SetUp();
		if (IsSkipped())
			return;

		StripPlayer();
		GivePlentyOfStats();
		AdjustToScreenGeometry({ 640, 480 });
		ASSERT_TRUE(Eval(Prelude).valid());
	}

	void TearDown() override
	{
		if (!IsSkipped())
			Eval("dropHandlers()");
		FreeInvGFX();
		LuaUITest::TearDown();
	}

	/** @brief Move the mouse to the middle of a slot of the inventory panel, or of the belt. */
	static void HoverSlot(int slot)
	{
		const Point panel = slot >= SLOTXY_BELT_FIRST ? GetMainPanel().position : GetRightPanel().position;
		const Point center = InvRect[slot].Center();
		MousePosition = panel + Displacement { center.x, center.y };
	}

	static void HoverCell(int cell)
	{
		HoverSlot(SLOTXY_INV_FIRST + cell);
	}

	static int ClickCount()
	{
		return EvalAs<int>("return #clicks").value_or(-1);
	}
};

TEST_F(LuaInventoryEventsTest, PlacementCheck_RefusedSpot_GameTriesNext)
{
	ASSERT_TRUE(Eval("on('InventoryPlacementCheck', function(cell, w, h) return cell == 30 and w == 1 and h == 1 end)").valid());

	EXPECT_TRUE(AutoPlaceAcquiredItemInInventory(*MyPlayer, MakePotion()));

	EXPECT_EQ(MyPlayer->InvGrid[30], 0);
	EXPECT_NE(MyPlayer->InvGrid[31], 0);
}

TEST_F(LuaInventoryEventsTest, PlacementCheck_AllRefused_ItemDoesNotFit)
{
	ASSERT_TRUE(Eval("on('InventoryPlacementCheck', function() return true end)").valid());

	EXPECT_FALSE(CanFitAcquiredItemInInventory(*MyPlayer, MakePotion()));
	EXPECT_FALSE(AutoPlaceAcquiredItemInInventory(*MyPlayer, MakePotion()));
	EXPECT_EQ(MyPlayer->_pNumInv, 0);
}

TEST_F(LuaInventoryEventsTest, PlacementCheck_NotAskedForOwnGear)
{
	ASSERT_TRUE(Eval("on('InventoryPlacementCheck', function() return true end)").valid());

	EXPECT_TRUE(AutoPlaceItemInInventory(*MyPlayer, MakePotion()));
	EXPECT_NE(MyPlayer->InvGrid[30], 0);
}

TEST_F(LuaInventoryEventsTest, PlacementCheck_Gold_SkipsRefusedCells)
{
	ASSERT_TRUE(Eval("on('InventoryPlacementCheck', function(cell, w, h) return cell == 39 and w == 1 and h == 1 end)").valid());

	EXPECT_EQ(RoomForGold(), 39 * MaxGold);
	EXPECT_EQ(AddGoldToInventory(*MyPlayer, 10), 0);

	EXPECT_EQ(MyPlayer->InvGrid[39], 0);
	EXPECT_NE(MyPlayer->InvGrid[38], 0);
}

TEST_F(LuaInventoryEventsTest, PlacementCheck_Gold_SoldItemCountsOnlyWhereAllowed)
{
	PlaceAt(MakeSword(), 0); // Cells 0, 10 and 20.
	ASSERT_TRUE(Eval("on('InventoryPlacementCheck', function(cell) return cell == 0 end)").valid());

	EXPECT_EQ(RoomForGold(), 37 * MaxGold);
	EXPECT_EQ(RoomForGold(0), 39 * MaxGold);
}

TEST_F(LuaInventoryEventsTest, Click_FiresWithCell)
{
	ASSERT_TRUE(Eval("on('InventoryCellClicked', recordClick)").valid());

	HoverCell(23);
	CheckInvItem();

	ASSERT_EQ(ClickCount(), 1);
	EXPECT_EQ(EvalAs<int>("return clicks[1]"), 23);
}

TEST_F(LuaInventoryEventsTest, Click_Consumed_SkipsCut)
{
	PlaceAt(MakePotion(), 5);
	ASSERT_TRUE(Eval("on('InventoryCellClicked', function() return true end)").valid());

	HoverCell(5);
	CheckInvItem();

	EXPECT_TRUE(MyPlayer->HoldItem.isEmpty());
	EXPECT_EQ(MyPlayer->InvGrid[5], 1);
	EXPECT_EQ(MyPlayer->_pNumInv, 1);
}

TEST_F(LuaInventoryEventsTest, Click_NotConsumed_CutsNormally)
{
	PlaceAt(MakePotion(), 5);
	ASSERT_TRUE(Eval("on('InventoryCellClicked', function() return false end)").valid());

	HoverCell(5);
	CheckInvItem();

	EXPECT_FALSE(MyPlayer->HoldItem.isEmpty());
	EXPECT_EQ(MyPlayer->InvGrid[5], 0);
	EXPECT_EQ(MyPlayer->_pNumInv, 0);
}

TEST_F(LuaInventoryEventsTest, Click_Consumed_HoldItemNotPasted)
{
	MyPlayer->HoldItem = MakePotion();
	ASSERT_TRUE(Eval("on('InventoryCellClicked', function() return true end)").valid());

	HoverCell(5);
	CheckInvItem();

	EXPECT_FALSE(MyPlayer->HoldItem.isEmpty());
	EXPECT_EQ(MyPlayer->InvGrid[5], 0);
	EXPECT_EQ(MyPlayer->_pNumInv, 0);
}

TEST_F(LuaInventoryEventsTest, Click_Consumed_SkipsStashTransfer)
{
	PlaceAt(MakePotion(), 5);
	ASSERT_TRUE(Eval("on('InventoryCellClicked', function() return true end)").valid());
	IsStashOpen = true;
	pcursinvitem = INVITEM_INV_FIRST;

	HoverCell(5);
	CheckInvItem(/*isShiftHeld=*/false, /*isCtrlHeld=*/true);

	EXPECT_EQ(MyPlayer->_pNumInv, 1);
}

TEST_F(LuaInventoryEventsTest, Click_StopsAtFirstTruthyHandler)
{
	ASSERT_TRUE(Eval(R"lua(
		on('InventoryCellClicked', function(cell) table.insert(clicks, 'first') end)
		on('InventoryCellClicked', function(cell) table.insert(clicks, 'second') return true end)
		on('InventoryCellClicked', function(cell) table.insert(clicks, 'third') return false end)
	)lua")
	                .valid());
	PlaceAt(MakePotion(), 5);

	HoverCell(5);
	CheckInvItem();

	EXPECT_EQ(EvalAs<std::string>("return table.concat(clicks, ' ')"), "first second");
	EXPECT_TRUE(MyPlayer->HoldItem.isEmpty()) << "A later handler must not take the click back";
}

TEST_F(LuaInventoryEventsTest, Click_TruthyValueOtherThanTrueConsumes)
{
	ASSERT_TRUE(Eval(R"lua(
		on('InventoryCellClicked', function() return 'taken' end)
		on('InventoryCellClicked', recordClick)
	)lua")
	                .valid());
	PlaceAt(MakePotion(), 5);

	HoverCell(5);
	CheckInvItem();

	EXPECT_EQ(ClickCount(), 0);
	EXPECT_TRUE(MyPlayer->HoldItem.isEmpty());
}

TEST_F(LuaInventoryEventsTest, Trigger_OtherEventsStillRunAllHandlers)
{
	ASSERT_TRUE(Eval(R"lua(
		events.registerCustom('TestEvent')
		on('TestEvent', function() table.insert(clicks, 'first') return true end)
		on('TestEvent', function() table.insert(clicks, 'second') return 'last' end)
	)lua")
	                .valid());

	EXPECT_EQ(EvalAs<std::string>("return events.TestEvent.trigger()"), "last");
	EXPECT_EQ(EvalAs<std::string>("return table.concat(clicks, ' ')"), "first second");
}

TEST_F(LuaInventoryEventsTest, Click_NoEventOnBodySlot)
{
	ASSERT_TRUE(Eval("on('InventoryCellClicked', recordClick)").valid());

	HoverSlot(SLOTXY_HAND_LEFT);
	CheckInvItem();
	HoverSlot(SLOTXY_INV_FIRST - 1);
	CheckInvItem();

	EXPECT_EQ(ClickCount(), 0);
}

TEST_F(LuaInventoryEventsTest, Click_NoEventOnBelt)
{
	ASSERT_TRUE(Eval("on('InventoryCellClicked', recordClick)").valid());

	HoverSlot(SLOTXY_BELT_FIRST);
	CheckInvItem();
	HoverSlot(SLOTXY_BELT_LAST);
	CheckInvItem();

	EXPECT_EQ(ClickCount(), 0);
}

TEST_F(LuaInventoryEventsTest, Click_NoEventOutsideAnySlot)
{
	ASSERT_TRUE(Eval("on('InventoryCellClicked', recordClick)").valid());

	MousePosition = GetRightPanel().position;
	CheckInvItem();

	EXPECT_EQ(ClickCount(), 0);
}

TEST_F(LuaInventoryEventsTest, Click_FirstAndLastCell)
{
	ASSERT_TRUE(Eval("on('InventoryCellClicked', recordClick)").valid());

	HoverCell(0);
	CheckInvItem();
	HoverCell(InventoryGridCells - 1);
	CheckInvItem();

	EXPECT_EQ(EvalAs<std::string>("return table.concat(clicks, ' ')"), "0 39");
}

TEST_F(LuaInventoryEventsTest, Click_NoEventWhenInspecting)
{
	ASSERT_TRUE(Eval("on('InventoryCellClicked', recordClick)").valid());
	Player other {};
	InspectPlayer = &other;

	HoverCell(5);
	CheckInvItem();

	InspectPlayer = MyPlayer;
	EXPECT_EQ(ClickCount(), 0);
}

TEST_F(LuaInventoryEventsTest, Click_HandlerError_FallsThrough)
{
	PlaceAt(MakePotion(), 5);
	ASSERT_TRUE(Eval("on('InventoryCellClicked', function() error('broken mod') end)").valid());

	HoverCell(5);
	CheckInvItem();

	EXPECT_FALSE(MyPlayer->HoldItem.isEmpty());
	EXPECT_EQ(MyPlayer->InvGrid[5], 0);
}

TEST_F(LuaInventoryEventsTest, Click_HandlerError_NonStringError_FallsThrough)
{
	MyPlayer->HoldItem = MakePotion();
	ASSERT_TRUE(Eval("on('InventoryCellClicked', function() error({ code = 1 }) end)").valid());

	HoverCell(5);
	CheckInvItem();

	EXPECT_TRUE(MyPlayer->HoldItem.isEmpty());
	EXPECT_NE(MyPlayer->InvGrid[5], 0);
}

TEST_F(LuaInventoryEventsTest, Click_HandlerErrorsWithTrue_NotConsumed)
{
	PlaceAt(MakePotion(), 5);
	ASSERT_TRUE(Eval("on('InventoryCellClicked', function() error(true) end)").valid());

	HoverCell(5);
	CheckInvItem();

	EXPECT_FALSE(MyPlayer->HoldItem.isEmpty());
}

TEST_F(LuaInventoryEventsTest, Click_HandlerError_LuaStateStaysUsable)
{
	ASSERT_TRUE(Eval(R"lua(
		broken = function() error('broken mod') end
		on('InventoryCellClicked', broken)
	)lua")
	                .valid());
	HoverCell(5);
	CheckInvItem();
	CheckInvItem();
	ASSERT_TRUE(Eval("events.InventoryCellClicked.remove(broken) on('InventoryCellClicked', recordClick)").valid());

	CheckInvItem();

	EXPECT_EQ(ClickCount(), 1);
}

TEST_F(LuaInventoryEventsTest, Click_HandlerHandsBackNothing_NotConsumed)
{
	PlaceAt(MakePotion(), 5);
	ASSERT_TRUE(Eval("on('InventoryCellClicked', function() end)").valid());

	HoverCell(5);
	CheckInvItem();

	EXPECT_FALSE(MyPlayer->HoldItem.isEmpty());
}

TEST_F(LuaInventoryEventsTest, DrawInv_FiresEvent)
{
	InitInv();
	const OwnedSurface out { 640, 480 };
	ASSERT_TRUE(Eval("on('InventoryDrawComplete', function() table.insert(clicks, 'drawn') end)").valid());

	DrawInv(out);

	EXPECT_EQ(ClickCount(), 1);
}

TEST_F(LuaInventoryEventsTest, DrawInv_NoEventWhenInspecting)
{
	InitInv();
	const OwnedSurface out { 640, 480 };
	ASSERT_TRUE(Eval("on('InventoryDrawComplete', function() table.insert(clicks, 'drawn') end)").valid());
	Player other {};
	InspectPlayer = &other;

	DrawInv(out);

	InspectPlayer = MyPlayer;
	EXPECT_EQ(ClickCount(), 0);
}

TEST_F(LuaInventoryEventsTest, DrawInv_HandlerError_DrawStillCompletes)
{
	InitInv();
	const OwnedSurface out { 640, 480 };
	ASSERT_TRUE(Eval("on('InventoryDrawComplete', function() error('broken mod') end)").valid());

	DrawInv(out);
	DrawInv(out);
}

TEST_F(LuaInventoryEventsTest, Rect_DrawsOutlineOnly)
{
	const ScopedBackBuffer buffer { 20, 10 };

	ASSERT_TRUE(Eval("render.rect(2, 3, 5, 4, 100)").valid());

	// x runs 2..6, y runs 3..6.
	const std::string expected = "...................."
	                             "...................."
	                             "...................."
	                             "..#####............."
	                             "..#...#............."
	                             "..#...#............."
	                             "..#####............."
	                             "...................."
	                             "...................."
	                             "....................";
	std::string painted;
	for (int y = 0; y < 10; y++) {
		for (int x = 0; x < 20; x++) {
			painted += buffer.Pixel(x, y) == 100 ? '#' : (buffer.Pixel(x, y) == Unpainted ? '.' : '?');
		}
	}
	EXPECT_EQ(painted, expected);
	EXPECT_EQ(buffer.CountPaintedInMargin(), 0);
}

TEST_F(LuaInventoryEventsTest, Rect_OneByOne_PaintsOnePixel)
{
	const ScopedBackBuffer buffer { 20, 10 };

	ASSERT_TRUE(Eval("render.rect(4, 5, 1, 1, 100)").valid());

	EXPECT_EQ(buffer.CountPainted(), 1);
	EXPECT_EQ(buffer.Pixel(4, 5), 100);
}

TEST_F(LuaInventoryEventsTest, Rect_ClipsAtRightAndBottom)
{
	const ScopedBackBuffer buffer { 20, 10 };

	ASSERT_TRUE(Eval("render.rect(18, 8, 6, 6, 100)").valid());

	// Only the top and left edge are on screen: (18,8), (19,8), (18,9).
	EXPECT_EQ(buffer.CountPainted(), 3);
	EXPECT_EQ(buffer.Pixel(18, 8), 100);
	EXPECT_EQ(buffer.Pixel(19, 8), 100);
	EXPECT_EQ(buffer.Pixel(18, 9), 100);
}

TEST_F(LuaInventoryEventsTest, Rect_ClipsAtLeftAndTop)
{
	const ScopedBackBuffer buffer { 20, 10 };

	ASSERT_TRUE(Eval("render.rect(-3, -2, 6, 5, 100)").valid());

	// Only the right edge (x = 2) and bottom edge (y = 2) are on screen.
	EXPECT_EQ(buffer.CountPainted(), 5);
	EXPECT_EQ(buffer.CountPaintedInMargin(), 0);
	EXPECT_EQ(buffer.Pixel(2, 0), 100);
	EXPECT_EQ(buffer.Pixel(2, 1), 100);
	EXPECT_EQ(buffer.Pixel(2, 2), 100);
	EXPECT_EQ(buffer.Pixel(0, 2), 100);
	EXPECT_EQ(buffer.Pixel(1, 2), 100);
}

TEST_F(LuaInventoryEventsTest, Rect_LargerThanScreen_PaintsNothing)
{
	const ScopedBackBuffer buffer { 20, 10 };

	ASSERT_TRUE(Eval("render.rect(-5, -5, 30, 20, 100)").valid());

	EXPECT_EQ(buffer.CountPainted(), 0);
}

TEST_F(LuaInventoryEventsTest, Rect_LargerThanScreen_ClipsTheEdgesItCrosses)
{
	const ScopedBackBuffer buffer { 20, 10 };

	ASSERT_TRUE(Eval("render.rect(5, 2, 100, 100, 100)").valid());

	EXPECT_EQ(buffer.CountPainted(), 15 + 7);
	EXPECT_EQ(buffer.CountPaintedInMargin(), 0);
}

TEST_F(LuaInventoryEventsTest, Rect_OffScreen_PaintsNothing)
{
	const ScopedBackBuffer buffer { 20, 10 };

	for (const char *const code : {
	         "render.rect(20, 0, 5, 5, 100)",
	         "render.rect(0, 10, 5, 5, 100)",
	         "render.rect(-5, 0, 5, 5, 100)",
	         "render.rect(0, -5, 5, 5, 100)",
	     }) {
		ASSERT_TRUE(Eval(code).valid()) << code;
		EXPECT_EQ(buffer.CountPainted(), 0) << code;
	}
}

TEST_F(LuaInventoryEventsTest, Rect_EmptySize_PaintsNothing)
{
	const ScopedBackBuffer buffer { 20, 10 };

	for (const char *const code : {
	         "render.rect(5, 5, 0, 4, 100)",
	         "render.rect(5, 5, 4, 0, 100)",
	         "render.rect(5, 5, -4, 4, 100)",
	         "render.rect(5, 5, 4, -4, 100)",
	         "render.rect(-2147483648, 5, -2147483647, 4, 100)",
	         "render.rect(5, -2147483648, 4, -2147483647, 100)",
	     }) {
		ASSERT_TRUE(Eval(code).valid()) << code;
		EXPECT_EQ(buffer.CountPainted(), 0) << code;
	}
}

TEST_F(LuaInventoryEventsTest, Rect_ValuesThatOverflowAnInt_PaintNoStray)
{
	const ScopedBackBuffer buffer { 20, 10 };

	ASSERT_TRUE(Eval("render.rect(2147483647, 2147483647, 2147483647, 2147483647, 100)").valid());
	ASSERT_TRUE(Eval("render.rect(-2147483648, -2147483648, 2147483647, 2147483647, 100)").valid());

	EXPECT_EQ(buffer.CountPainted(), 0);
}

TEST_F(LuaInventoryEventsTest, Rect_LargestSize_ClipsTheEdgesItCrosses)
{
	const ScopedBackBuffer buffer { 20, 10 };

	ASSERT_TRUE(Eval("render.rect(5, 2, 2147483647, 2147483647, 100)").valid());

	EXPECT_EQ(buffer.CountPainted(), 15 + 7);
	EXPECT_EQ(buffer.CountPaintedInMargin(), 0);
}

TEST_F(LuaInventoryEventsTest, Rect_BadArguments_PaintNothing_NoLuaError)
{
	const ScopedBackBuffer buffer { 20, 10 };
	const std::string good[] = { "1", "2", "3", "4", "100" };

	for (size_t badIndex = 0; badIndex < std::size(good); badIndex++) {
		for (const char *const bad : { "1.5", "0/0", "1/0", "-1/0", "2^53", "4294967298", "math.maxinteger", "math.mininteger", "'3'", "true", "nil", "{}", "print" }) {
			std::string code = "render.rect(";
			for (size_t i = 0; i < std::size(good); i++) {
				if (i != 0)
					code += ", ";
				code += i == badIndex ? bad : good[i];
			}
			code += ")";
			const sol::protected_function_result result = Eval(code);
			EXPECT_TRUE(result.valid()) << code;
			EXPECT_EQ(buffer.CountPainted(), 0) << code;
		}
	}
}

TEST_F(LuaInventoryEventsTest, Rect_MissingArguments_PaintNothing_NoLuaError)
{
	const ScopedBackBuffer buffer { 20, 10 };

	for (const char *const code : { "render.rect()", "render.rect(1)", "render.rect(1, 2, 3, 4)" }) {
		const sol::protected_function_result result = Eval(code);
		EXPECT_TRUE(result.valid()) << code;
		EXPECT_EQ(buffer.CountPainted(), 0) << code;
	}
}

TEST_F(LuaInventoryEventsTest, Rect_ColorOutOfRange_PaintsNothing)
{
	const ScopedBackBuffer buffer { 20, 10 };

	for (const char *const code : { "render.rect(1, 2, 3, 4, -1)", "render.rect(1, 2, 3, 4, 256)" }) {
		ASSERT_TRUE(Eval(code).valid()) << code;
		EXPECT_EQ(buffer.CountPainted(), 0) << code;
	}
}

TEST_F(LuaInventoryEventsTest, Rect_ColorBounds_Painted)
{
	const ScopedBackBuffer buffer { 20, 10 };

	ASSERT_TRUE(Eval("render.rect(1, 1, 1, 1, 0)").valid());
	ASSERT_TRUE(Eval("render.rect(2, 1, 1, 1, 255)").valid());

	EXPECT_EQ(buffer.Pixel(1, 1), 0);
	EXPECT_EQ(buffer.Pixel(2, 1), 255);
}

TEST_F(LuaInventoryEventsTest, Rect_FloatWithWholeValue_Painted)
{
	const ScopedBackBuffer buffer { 20, 10 };

	ASSERT_TRUE(Eval("render.rect(8 / 2, 10 / 2, 1.0, 1.0, 100.0)").valid());

	EXPECT_EQ(buffer.Pixel(4, 5), 100);
}

TEST_F(LuaInventoryEventsTest, Rect_NoBackBuffer_NoCrash)
{
	ASSERT_EQ(PalSurface, nullptr);
	AdjustToScreenGeometry({ 640, 480 });

	EXPECT_TRUE(Eval("render.rect(1, 2, 3, 4, 100)").valid());
}

TEST_F(LuaInventoryEventsTest, InventoryCellRect_FollowsThePanel)
{
	AdjustToScreenGeometry({ 1280, 720 });

	// The right panel is at (960, 120) on this screen.
	ASSERT_TRUE(Eval(R"lua(
		first = { render.inventoryCellRect(0) }
		last = { render.inventoryCellRect(39) }
	)lua")
	                .valid());

	EXPECT_EQ(EvalAs<std::string>("return table.concat(first, ' ')"), "977 343 28 28");
	EXPECT_EQ(EvalAs<std::string>("return table.concat(last, ' ')"), "1238 430 28 28");
}

TEST_F(LuaInventoryEventsTest, InventoryCellRect_IsWhereTheCellIsDrawn)
{
	InitInv();
	const OwnedSurface empty { 640, 480 };
	const OwnedSurface filled { 640, 480 };
	DrawInv(empty);
	PlaceAt(MakePotion(), 12);
	DrawInv(filled);

	int left = 640;
	int top = 480;
	int right = -1;
	int bottom = -1;
	for (int y = 0; y < 480; y++) {
		for (int x = 0; x < 640; x++) {
			if (*empty.at(x, y) == *filled.at(x, y))
				continue;
			left = std::min(left, x);
			top = std::min(top, y);
			right = std::max(right, x);
			bottom = std::max(bottom, y);
		}
	}

	ASSERT_TRUE(Eval("rect = { render.inventoryCellRect(12) }").valid());
	EXPECT_EQ(EvalAs<std::string>("return table.concat(rect, ' ')"), StrCat(left, " ", top, " ", right - left + 1, " ", bottom - top + 1));
}

TEST_F(LuaInventoryEventsTest, InventoryCellRect_CornersClickTheCell)
{
	ASSERT_TRUE(Eval("on('InventoryCellClicked', recordClick)").valid());

	for (int cell = 0; cell < InventoryGridCells; cell++) {
		ASSERT_TRUE(Eval(StrCat("rect = { render.inventoryCellRect(", cell, ") }")).valid());
		const int x = *EvalAs<int>("return rect[1]");
		const int y = *EvalAs<int>("return rect[2]");
		const int width = *EvalAs<int>("return rect[3]");
		const int height = *EvalAs<int>("return rect[4]");
		for (const Point corner : { Point { x, y }, Point { x + width - 1, y + height - 1 } }) {
			ASSERT_TRUE(Eval("clicks = {}").valid());
			MousePosition = corner;
			CheckInvItem();
			EXPECT_EQ(EvalAs<int>("return clicks[1]"), cell) << corner.x << "," << corner.y;
		}
	}
}

TEST_F(LuaInventoryEventsTest, InventoryCellRect_BadCell_GivesNil_NoLuaError)
{
	for (const char *const bad : { "-1", "40", "1.5", "2^53", "1e308", "0/0", "1/0", "4294967296", "math.mininteger", "'0'", "true", "nil", "{}", "print", "" }) {
		const std::string code = StrCat("return select('#', render.inventoryCellRect(", bad, ")), (render.inventoryCellRect(", bad, "))");
		const sol::protected_function_result result = Eval(code);
		ASSERT_TRUE(result.valid()) << code;
		EXPECT_EQ(result.get<int>(0), 1) << code;
		EXPECT_EQ(result.get<sol::object>(1).get_type(), sol::type::lua_nil) << code;
	}
}

TEST_F(LuaInventoryEventsTest, PaletteColor)
{
	EXPECT_EQ(EvalAs<int>("return render.PaletteColor.Beige"), 160);
	EXPECT_EQ(EvalAs<int>("return render.PaletteColor.Blue"), 176);
	EXPECT_EQ(EvalAs<int>("return render.PaletteColor.Yellow"), 192);
	EXPECT_EQ(EvalAs<int>("return render.PaletteColor.Orange"), 208);
	EXPECT_EQ(EvalAs<int>("return render.PaletteColor.Red"), 224);
	EXPECT_EQ(EvalAs<int>("return render.PaletteColor.Gray"), 240);
}

TEST_F(LuaInventoryEventsTest, PaletteColor_ShadeIsAValidColor)
{
	const ScopedBackBuffer buffer { 20, 10 };

	ASSERT_TRUE(Eval("render.rect(4, 5, 1, 1, render.PaletteColor.Gray + 15)").valid());

	EXPECT_EQ(buffer.Pixel(4, 5), 255);
}

TEST_F(LuaInventoryEventsTest, NewNamesAreDocumented)
{
	for (const char *const code : {
	         "return render.__doc_rect",
	         "return render.__sig_rect",
	         "return render.__doc_inventoryCellRect",
	         "return render.__sig_inventoryCellRect",
	         "return render.__doc_PaletteColor",
	         "return render.__sig_PaletteColor",
	         "return events.__doc_InventoryCellClicked",
	         "return events.__doc_InventoryDrawComplete",
	         "return events.__doc_InventoryDrawBackground",
	     }) {
		EXPECT_THAT(EvalAs<std::string>(code).value_or(""), Not(IsEmpty())) << code;
	}
}

TEST_F(LuaInventoryEventsTest, DrawInv_RectFromHandler_IsOnTopOfPanelAndItems)
{
	const ScopedBackBuffer buffer { 640, 480 };
	InitInv();
	PlaceAt(MakePotion(), 12);
	ASSERT_TRUE(Eval(R"lua(
		on('InventoryDrawComplete', function()
		  local x, y, width, height = render.inventoryCellRect(12)
		  render.rect(x, y, width, height, render.PaletteColor.Orange)
		end)
	)lua")
	                .valid());

	DrawInv(GlobalBackBuffer());

	// Cell 12 is drawn from (395, 252) to (422, 279).
	for (int x = 395; x <= 422; x++) {
		EXPECT_EQ(buffer.Pixel(x, 252), PAL16_ORANGE) << x;
		EXPECT_EQ(buffer.Pixel(x, 279), PAL16_ORANGE) << x;
	}
	for (int y = 252; y <= 279; y++) {
		EXPECT_EQ(buffer.Pixel(395, y), PAL16_ORANGE) << y;
		EXPECT_EQ(buffer.Pixel(422, y), PAL16_ORANGE) << y;
	}
	EXPECT_EQ(buffer.CountPaintedInMargin(), 0);
}

TEST_F(LuaInventoryEventsTest, DrawInv_BackgroundEvent_IsAbovePanelAndBelowItems)
{
	const ScopedBackBuffer buffer { 640, 480 };
	InitInv();
	PlaceAt(MakePotion(), 12);
	const Point itemPixel { 395 + 14, 252 + 14 };

	DrawInv(GlobalBackBuffer());
	const uint8_t itemColor = buffer.Pixel(itemPixel.x, itemPixel.y);
	ASSERT_NE(itemColor, PAL16_ORANGE);

	ASSERT_TRUE(Eval(R"lua(
		on('InventoryDrawBackground', function()
		  table.insert(clicks, 'background')
		  for y = 240, 300 do render.rect(380, y, 80, 1, render.PaletteColor.Orange) end
		end)
	)lua")
	                .valid());
	DrawInv(GlobalBackBuffer());

	EXPECT_EQ(EvalAs<int>("return #clicks"), 1);
	EXPECT_EQ(buffer.Pixel(itemPixel.x, itemPixel.y), itemColor);
	EXPECT_EQ(buffer.Pixel(450, 290), PAL16_ORANGE); // Empty grid, the panel lies below the event.
}

TEST_F(LuaInventoryEventsTest, DrawInv_BackgroundEvent_NotCalledWhileInspecting)
{
	const ScopedBackBuffer buffer { 640, 480 };
	InitInv();
	ASSERT_TRUE(Eval("on('InventoryDrawBackground', function() table.insert(clicks, 'background') end)").valid());
	Player other {};
	InspectPlayer = &other;

	DrawInv(GlobalBackBuffer());

	InspectPlayer = MyPlayer;
	EXPECT_EQ(EvalAs<int>("return #clicks"), 0);
}

class LuaPanelBlitTest : public LuaInventoryEventsTest {
protected:
	static constexpr Point Panel { 320, 0 };

	void SetUp() override
	{
		LuaInventoryEventsTest::SetUp();
		if (IsSkipped())
			return;
		InitInv();
		MakeLightTable();
	}

	/** @brief The pixels of the panel art, as DrawInv paints them on a clean inventory. */
	static std::vector<uint8_t> PanelArt(const ScopedBackBuffer &buffer)
	{
		DrawInv(GlobalBackBuffer());
		std::vector<uint8_t> art;
		for (int y = 0; y < 352; y++)
			for (int x = 0; x < 320; x++)
				art.push_back(buffer.Pixel(Panel.x + x, y));
		return art;
	}

	static uint8_t Art(const std::vector<uint8_t> &art, int x, int y) { return art[y * 320 + x]; }
};

TEST_F(LuaPanelBlitTest, ShadeZero_CopiesTheRegion)
{
	const ScopedBackBuffer buffer { 640, 480 };
	const std::vector<uint8_t> art = PanelArt(buffer);

	ASSERT_TRUE(Eval("render.inventoryPanelBlit(10, 12, 20, 6, 100, 200, 0)").valid());

	for (int y = 0; y < 6; y++)
		for (int x = 0; x < 20; x++)
			EXPECT_EQ(buffer.Pixel(100 + x, 200 + y), Art(art, 10 + x, 12 + y)) << x << "," << y;
	EXPECT_EQ(buffer.Pixel(99, 200), Unpainted);
	EXPECT_EQ(buffer.Pixel(120, 200), Unpainted);
	EXPECT_EQ(buffer.Pixel(100, 199), Unpainted);
	EXPECT_EQ(buffer.Pixel(100, 206), Unpainted);
}

TEST_F(LuaPanelBlitTest, Shade_DarkensThroughTheLightTable)
{
	const ScopedBackBuffer buffer { 640, 480 };
	const std::vector<uint8_t> art = PanelArt(buffer);

	ASSERT_TRUE(Eval("render.inventoryPanelBlit(10, 12, 20, 6, 100, 200, 3)").valid());

	int changed = 0;
	for (int y = 0; y < 6; y++) {
		for (int x = 0; x < 20; x++) {
			const uint8_t original = Art(art, 10 + x, 12 + y);
			EXPECT_EQ(buffer.Pixel(100 + x, 200 + y), LightTables[3][original]);
			changed += LightTables[3][original] != original ? 1 : 0;
		}
	}
	EXPECT_GT(changed, 0);
}

TEST_F(LuaPanelBlitTest, ClipsAtEveryScreenEdge_WithoutShiftingTheArt)
{
	const ScopedBackBuffer buffer { 640, 480 };
	const std::vector<uint8_t> art = PanelArt(buffer);

	ASSERT_TRUE(Eval("render.inventoryPanelBlit(10, 12, 20, 6, -5, -2, 0)").valid());
	ASSERT_TRUE(Eval("render.inventoryPanelBlit(10, 12, 20, 6, 630, 476, 0)").valid());

	EXPECT_EQ(buffer.Pixel(0, 0), Art(art, 15, 14));
	EXPECT_EQ(buffer.Pixel(639, 479), Art(art, 19, 15));
	EXPECT_EQ(buffer.CountPaintedInMargin(), 0);
}

TEST_F(LuaPanelBlitTest, PartOutsideThePanel_DrawsOnlyThePanelPart)
{
	const ScopedBackBuffer buffer { 640, 480 };
	const std::vector<uint8_t> art = PanelArt(buffer);

	ASSERT_TRUE(Eval("render.inventoryPanelBlit(-4, 345, 10, 20, 50, 300, 0)").valid());

	EXPECT_EQ(buffer.Pixel(49 + 4, 300), Unpainted);
	EXPECT_EQ(buffer.Pixel(54, 300), Art(art, 0, 345));
	EXPECT_EQ(buffer.Pixel(54, 306), Art(art, 0, 351));
	EXPECT_EQ(buffer.Pixel(54, 307), Unpainted);
}

TEST_F(LuaPanelBlitTest, BadArguments_DrawNothing)
{
	const ScopedBackBuffer buffer { 640, 480 };
	for (const char *call : {
	         "render.inventoryPanelBlit()",
	         "render.inventoryPanelBlit('a', 0, 5, 5, 0, 0, 0)",
	         "render.inventoryPanelBlit(0, 0, 5.5, 5, 0, 0, 0)",
	         "render.inventoryPanelBlit(0, 0, 5, 5, 0, 0, -1)",
	         "render.inventoryPanelBlit(0, 0, 5, 5, 0, 0, 16)",
	         "render.inventoryPanelBlit(0, 0, 0, 5, 0, 0, 0)",
	         "render.inventoryPanelBlit(0, 0, -5, 5, 0, 0, 0)",
	         "render.inventoryPanelBlit(0, 0, 5, 5, 2147483647, 2147483647, 0)",
	         "render.inventoryPanelBlit(2147483647, -2147483648, 5, 5, 0, 0, 0)",
	         "render.inventoryPanelBlit(-2147483648, 2147483647, 2147483647, 2147483647, -2147483648, -2147483648, 0)",
	         "render.inventoryPanelBlit(5000, 5000, 5, 5, 0, 0, 0)",
	     }) {
		EXPECT_TRUE(Eval(call).valid()) << call;
	}
	for (int y = 0; y < 480; y += 7)
		for (int x = 0; x < 640; x += 7)
			EXPECT_EQ(buffer.Pixel(x, y), Unpainted) << x << "," << y;
}

TEST_F(LuaPanelBlitTest, NoPanelArtOrNoScreen_DrawsNothing)
{
	EXPECT_TRUE(Eval("render.inventoryPanelBlit(0, 0, 5, 5, 0, 0, 0)").valid()); // No back buffer.

	const ScopedBackBuffer buffer { 640, 480 };
	FreeInvGFX();
	EXPECT_TRUE(Eval("render.inventoryPanelBlit(0, 0, 5, 5, 0, 0, 0)").valid());
	EXPECT_EQ(buffer.Pixel(0, 0), Unpainted);
}

TEST_F(LuaInventoryEventsTest, Click_NoHandlers_CutsNormally)
{
	PlaceAt(MakePotion(), 5);

	HoverCell(5);
	CheckInvItem();

	EXPECT_FALSE(MyPlayer->HoldItem.isEmpty());
	EXPECT_EQ(MyPlayer->InvGrid[5], 0);
}

TEST_F(LuaInventoryEventsTest, Click_NoHandlers_PastesNormally)
{
	MyPlayer->HoldItem = MakePotion();

	HoverCell(5);
	CheckInvItem();

	EXPECT_TRUE(MyPlayer->HoldItem.isEmpty());
	EXPECT_NE(MyPlayer->InvGrid[5], 0);
}

/**
 * Runs last: a loaded game leaves the level, and everything else in the save, behind.
 */
class LuaGameLoadTest : public LuaInventoryEventsTest {
};

TEST_F(LuaGameLoadTest, GameLoad_FiresGameStart_AfterTheHeroIsLoaded)
{
	ASSERT_TRUE(Eval(R"lua(
		local player = require('devilutionx.player')
		on('GameStart', function() table.insert(clicks, player.self().name) end)
	)lua")
	                .valid());
	std::error_code error;
	std::filesystem::copy_file(paths::BasePath() + "test/fixtures/timedemo/WarriorLevel1to2/spawn_0.sv", paths::PrefPath() + "spawn_0.sv", std::filesystem::copy_options::overwrite_existing, error);
	ASSERT_FALSE(error) << error.message();
	gbIsSpawn = true;
	LoadMissileData();
	LoadMonsterData();
	LoadObjectData();
	ASSERT_EQ(MyPlayer->getCharacterLevel(), 25) << "The save has to hold another hero";
	const std::string nameBefore = MyPlayer->_pName;

	gamemenu_load_game(false);

	ASSERT_NE(MyPlayer->getCharacterLevel(), 25);
	ASSERT_NE(nameBefore, MyPlayer->_pName);
	ASSERT_EQ(ClickCount(), 1);
	EXPECT_EQ(EvalAs<std::string>("return clicks[1]"), MyPlayer->_pName);
	gbIsSpawn = false;
}

} // namespace
} // namespace devilution
