/**
 * @file inventory_reserved_cells_test.cpp
 *
 * Tests for Player::reservedInvCells.
 *
 * A reserved cell is never used by automatic placement, and automatic
 * rearranging never moves an item touching one. Manual placement is unaffected.
 */

#include <algorithm>
#include <array>
#include <cstdlib>
#include <initializer_list>
#include <iterator>

#include <gtest/gtest.h>

#include "ui_test.hpp"

#include "inv.h"
#include "items.h"
#include "levels/gendung.h"
#include "objects.h"
#include "player.h"
#include "qol/autopickup.h"
#include "stores.h"

namespace devilution {
namespace {

constexpr int SalePrice = GOLD_MAX_LIMIT + 1000;

/** ShrineSpiritual in the shrine_type enum, which objects.cpp keeps private. */
constexpr int SpiritualShrineType = 17;

class InvReservedTest : public UITest {
protected:
	void SetUp() override
	{
		UITest::SetUp();
		if (IsSkipped())
			return;

		StripPlayer();
		Stash = {};
		Stash.gold = 0;
		Stash.dirty = false;

		// CheckInvItem() does nothing while inspecting someone else.
		InspectPlayer = MyPlayer;
	}

	/** @brief Create a healing potion (1x1). */
	static Item MakePotion()
	{
		Item item {};
		InitializeItem(item, IDI_HEAL);
		return item;
	}

	/** @brief Create the first weapon or armor with the given inventory size. */
	static Item MakeItemOfSize(Size size)
	{
		for (size_t i = 1; i < AllItemsList.size(); i++) {
			if (AllItemsList[i].iLoc == ILOC_INVALID || AllItemsList[i].iLoc == ILOC_UNEQUIPABLE)
				continue;
			Item item {};
			InitializeItem(item, static_cast<_item_indexes>(i));
			if (GetInventorySize(item) == size)
				return item;
		}
		ADD_FAILURE() << "No item of size " << size.width << "x" << size.height;
		return {};
	}

	/** @brief Put an item at an exact grid position, encoded the way the game does. */
	static void PlaceAt(const Item &item, int topLeftCell)
	{
		Player &player = *MyPlayer;
		const Size size = GetInventorySize(item);
		player.InvList[player._pNumInv] = item;
		player._pNumInv++;
		const auto id = static_cast<int8_t>(player._pNumInv);
		for (int y = 0; y < size.height; y++) {
			for (int x = 0; x < size.width; x++) {
				const bool isMainCell = x == 0 && y == size.height - 1;
				player.InvGrid[topLeftCell + (y * 10) + x] = isMainCell ? id : static_cast<int8_t>(-id);
			}
		}
	}

	/** @brief Fill every empty cell with a full gold pile: no vendor buys it and it has no room for more gold. */
	static void FillFreeCells(std::initializer_list<int> cellsToSkip = {})
	{
		for (int cell = 0; cell < InventoryGridCells; cell++) {
			if (MyPlayer->InvGrid[cell] != 0 || std::find(cellsToSkip.begin(), cellsToSkip.end(), cell) != cellsToSkip.end())
				continue;
			Item gold {};
			MakeGoldStack(gold, GOLD_MAX_LIMIT);
			PlaceAt(gold, cell);
		}
	}

	/** @brief Create a 1x3 weapon that sells for SalePrice, which is more than one gold pile can hold. */
	static Item MakeValuableWeapon()
	{
		Item weapon = MakeItemOfSize({ 1, 3 });
		weapon._ivalue = SalePrice * 4;
		weapon._iIvalue = SalePrice * 4;
		return weapon;
	}

	/** @brief Create a 1x3 weapon that sells for more gold than its own three cells can hold. */
	static Item MakePricelessWeapon()
	{
		Item weapon = MakeItemOfSize({ 1, 3 });
		weapon._ivalue = SalePrice * 16;
		weapon._iIvalue = SalePrice * 16;
		return weapon;
	}

	/** @brief Put a scroll that sells for less than one gold pile holds (50 gold) in the first belt slot. */
	static void PutCheapScrollInBelt()
	{
		Item scroll {};
		InitializeItem(scroll, IDI_PORTAL);
		MyPlayer->SpdList[0] = scroll;
	}

	/** @brief Sell the item covering a grid cell to Griswold through the visual store. */
	static void SellInVisualStore(int cell)
	{
		VisualStore.vendor = VisualStoreVendor::Smith;
		SellItemToVisualStore(std::abs(MyPlayer->InvGrid[cell]) - 1);
	}

	/** @brief Sell the item on the cursor to Griswold by dropping it on the visual store. */
	static void PasteInVisualStore()
	{
		VisualStore.vendor = VisualStoreVendor::Smith;
		CheckVisualStorePaste(MousePosition);
	}

	/** @brief Use a Spiritual shrine on dungeon level type 1, where each pile it creates holds 5 to 14 gold. */
	static void UseSpiritualShrine()
	{
		leveltype = DTYPE_CATHEDRAL;
		Object shrine {};
		shrine._otype = OBJ_SHRINEL;
		shrine._oVar1 = SpiritualShrineType;
		shrine.selectionRegion = SelectionRegion::Middle;

		OperateObject(*MyPlayer, shrine);
	}

	/**
	 * @brief Lay out a 2x2 item, a 1x3 item and a 1x1 item so that sorting can't succeed.
	 * Only the cells in use stay unreserved. Sorting takes 0, 10, 20 for the 1x3 item, leaving no 2x2 block.
	 */
	static void LayOutImpossibleSort(const Item &square, const Item &tall, const Item &single)
	{
		PlaceAt(square, 0);
		PlaceAt(tall, 12);
		PlaceAt(single, 20);
		MyPlayer->reservedInvCells.set();
		for (const int cell : { 0, 1, 10, 11, 12, 20, 22, 32 })
			MyPlayer->reservedInvCells.reset(cell);
	}

	/** @brief Create a Town Portal scroll the player can read and put it in the readied spell slot. */
	static Item MakeReadiedScroll()
	{
		Item scroll {};
		InitializeItem(scroll, IDI_PORTAL);
		scroll._iStatFlag = true;
		MyPlayer->_pRSpell = SpellID::TownPortal;
		MyPlayer->_pRSplType = SpellType::Scroll;
		return scroll;
	}

	/** @brief Open a sell list and pick its first entry, the only one expected there. */
	static void PickOnlyItemToSell(TalkID sellStore)
	{
		StartStore(sellStore);
		ASSERT_EQ(ActiveStore, sellStore);
		ASSERT_EQ(CurrentItemIndex, 1);
		ScrollPos = 0;
		CurrentTextLine = 5;
		StoreEnter();
	}

	/** @brief The item covering a grid cell, or nullptr for an empty cell. */
	static const Item *ItemAt(int cell)
	{
		const int id = std::abs(MyPlayer->InvGrid[cell]);
		return id == 0 ? nullptr : &MyPlayer->InvList[id - 1];
	}

	/** @brief Run autopickup with a gold pile next to the player. @return Whether it asked to pick up the gold. */
	static bool AutoPickupRequestsGold()
	{
		leveltype = DTYPE_CATHEDRAL;
		GetOptions().Gameplay.autoGoldPickup.SetValue(true);
		MyPlayer->position.tile = { 50, 50 };
		const Point tile = MyPlayer->position.tile + PathDirs[0];

		Item &gold = Items[0];
		gold = {};
		MakeGoldStack(gold, 100);
		gold.position = tile;
		dItem[tile.x][tile.y] = 1;

		AutoPickup(*MyPlayer);

		dItem[tile.x][tile.y] = 0;
		return gold._iRequest;
	}

	/** @brief Move the mouse over an inventory panel slot (body or grid cell). */
	static void HoverSlot(int slot)
	{
		MousePosition = GetRightPanel().position + Displacement { InvRect[slot].Center().x, InvRect[slot].Center().y };
	}
};

TEST_F(InvReservedTest, AutoPlace_SkipsReservedCell)
{
	// A 1x1 item first tries cell 30 on an empty grid.
	MyPlayer->reservedInvCells.set(30);

	ASSERT_TRUE(AutoPlaceItemInInventory(*MyPlayer, MakePotion()));

	EXPECT_EQ(MyPlayer->InvGrid[30], 0);
	EXPECT_EQ(MyPlayer->InvGrid[31], 1);
}

TEST_F(InvReservedTest, AutoPlace_NoRoomOutsideReserved)
{
	for (int cell = 0; cell < InventoryGridCells; cell++) {
		if (cell < 5)
			MyPlayer->reservedInvCells.set(cell);
		else
			PlaceAt(MakePotion(), cell);
	}

	EXPECT_FALSE(CanFitItemInInventory(*MyPlayer, MakePotion()));
	EXPECT_FALSE(AutoPlaceItemInInventory(*MyPlayer, MakePotion()));
	EXPECT_EQ(MyPlayer->_pNumInv, 35);
}

TEST_F(InvReservedTest, AutoPlace_2x3_DoesNotOverlapReserved)
{
	// Cell 11 rules out top-left cells 0 and 1, the two first choices of a 2x3 item.
	MyPlayer->reservedInvCells.set(11);

	ASSERT_TRUE(AutoPlaceItemInInventory(*MyPlayer, MakeItemOfSize({ 2, 3 })));

	EXPECT_EQ(MyPlayer->InvGrid[0], 0);
	EXPECT_EQ(MyPlayer->InvGrid[1], 0);
	EXPECT_EQ(MyPlayer->InvGrid[11], 0);
	EXPECT_EQ(MyPlayer->InvGrid[2], -1);
	EXPECT_EQ(MyPlayer->InvGrid[22], 1);
}

TEST_F(InvReservedTest, ManualPaste_IgnoresReservation)
{
	MyPlayer->reservedInvCells.set(12);
	MyPlayer->HoldItem = MakePotion();

	HoverSlot(SLOTXY_INV_FIRST + 12);
	CheckInvItem();

	EXPECT_TRUE(MyPlayer->HoldItem.isEmpty());
	EXPECT_EQ(MyPlayer->InvGrid[12], 1);
}

TEST_F(InvReservedTest, Gold_NotPlacedInReserved)
{
	// A new gold pile first tries cell 39.
	MyPlayer->reservedInvCells.set(39);

	EXPECT_EQ(AddGoldToInventory(*MyPlayer, 100), 0);

	EXPECT_EQ(MyPlayer->InvGrid[39], 0);
	EXPECT_EQ(MyPlayer->InvGrid[38], 1);
}

TEST_F(InvReservedTest, Gold_LeftOverWhenOnlyReservedFree)
{
	MyPlayer->reservedInvCells.set();

	EXPECT_EQ(AddGoldToInventory(*MyPlayer, 100), 100);

	EXPECT_EQ(MyPlayer->_pNumInv, 0);
}

TEST_F(InvReservedTest, RoomForGold_ExcludesReservedEmpty)
{
	MyPlayer->reservedInvCells.set(0);
	MyPlayer->reservedInvCells.set(1);
	MyPlayer->reservedInvCells.set(2);

	EXPECT_EQ(RoomForGold(), 37 * GOLD_MAX_LIMIT);
}

TEST_F(InvReservedTest, RoomForGold_ReservedPileStillCounts)
{
	for (int cell = 0; cell < InventoryGridCells; cell++) {
		if (cell == 5) {
			Item gold {};
			MakeGoldStack(gold, 1000);
			PlaceAt(gold, cell);
		} else {
			PlaceAt(MakePotion(), cell);
		}
	}
	MyPlayer->reservedInvCells.set(5);

	EXPECT_EQ(RoomForGold(), GOLD_MAX_LIMIT - 1000);

	EXPECT_EQ(AddGoldToInventory(*MyPlayer, 500), 0);
	EXPECT_EQ(MyPlayer->InvList[5]._ivalue, 1500);
}

TEST_F(InvReservedTest, AutoPickup_TakesGoldWhenCellFree)
{
	EXPECT_TRUE(AutoPickupRequestsGold());
}

TEST_F(InvReservedTest, AutoPickup_LeavesGoldWhenOnlyReservedFree)
{
	MyPlayer->reservedInvCells.set();

	EXPECT_FALSE(AutoPickupRequestsGold());
}

TEST_F(InvReservedTest, Reorganize_NoReservation_Unchanged)
{
	const Item tall = MakeItemOfSize({ 1, 3 });
	const Item square = MakeItemOfSize({ 2, 2 });
	PlaceAt(MakePotion(), 0);
	PlaceAt(square, 12);
	PlaceAt(tall, 19);

	ReorganizeInventory(*MyPlayer);

	EXPECT_EQ(MyPlayer->_pNumInv, 3);
	// Tallest first, each at the first spot FindSlotForItem() tries for its size.
	ASSERT_NE(ItemAt(0), nullptr);
	EXPECT_EQ(ItemAt(0)->IDidx, tall.IDidx);
	EXPECT_GT(MyPlayer->InvGrid[20], 0);
	ASSERT_NE(ItemAt(8), nullptr);
	EXPECT_EQ(ItemAt(8)->IDidx, square.IDidx);
	EXPECT_GT(MyPlayer->InvGrid[18], 0);
	ASSERT_NE(ItemAt(30), nullptr);
	EXPECT_EQ(ItemAt(30)->IDidx, IDI_HEAL);
	EXPECT_EQ(MyPlayer->InvGrid[12], 0);
}

TEST_F(InvReservedTest, Reorganize_PinsReservedItems)
{
	const Item tall = MakeItemOfSize({ 1, 3 });
	const Item square = MakeItemOfSize({ 2, 2 });
	PlaceAt(MakePotion(), 0);
	PlaceAt(square, 12);
	PlaceAt(tall, 19);
	// Only the bottom right cell of the 2x2 item, which is not the cell InvGrid marks as its main one.
	MyPlayer->reservedInvCells.set(23);

	ReorganizeInventory(*MyPlayer);

	EXPECT_EQ(MyPlayer->_pNumInv, 3);
	ASSERT_NE(ItemAt(12), nullptr);
	EXPECT_EQ(ItemAt(12)->IDidx, square.IDidx);
	const int8_t id = MyPlayer->InvGrid[22];
	EXPECT_GT(id, 0);
	EXPECT_EQ(MyPlayer->InvGrid[12], -id);
	EXPECT_EQ(MyPlayer->InvGrid[13], -id);
	EXPECT_EQ(MyPlayer->InvGrid[23], -id);

	// The others are still sorted around it.
	ASSERT_NE(ItemAt(0), nullptr);
	EXPECT_EQ(ItemAt(0)->IDidx, tall.IDidx);
	ASSERT_NE(ItemAt(30), nullptr);
	EXPECT_EQ(ItemAt(30)->IDidx, IDI_HEAL);
	EXPECT_EQ(MyPlayer->InvGrid[19], 0);
}

TEST_F(InvReservedTest, Reorganize_ImpossibleSort_KeepsEverything)
{
	const Item tall = MakeItemOfSize({ 1, 3 });
	const Item square = MakeItemOfSize({ 2, 2 });
	LayOutImpossibleSort(square, tall, MakePotion());

	std::array<int8_t, InventoryGridCells> gridBefore;
	std::copy(std::begin(MyPlayer->InvGrid), std::end(MyPlayer->InvGrid), gridBefore.begin());

	ReorganizeInventory(*MyPlayer);

	EXPECT_EQ(MyPlayer->_pNumInv, 3);
	EXPECT_TRUE(std::equal(gridBefore.begin(), gridBefore.end(), std::begin(MyPlayer->InvGrid)));
	ASSERT_NE(ItemAt(0), nullptr);
	EXPECT_EQ(ItemAt(0)->IDidx, square.IDidx);
	ASSERT_NE(ItemAt(12), nullptr);
	EXPECT_EQ(ItemAt(12)->IDidx, tall.IDidx);
	ASSERT_NE(ItemAt(20), nullptr);
	EXPECT_EQ(ItemAt(20)->IDidx, IDI_HEAL);
}

TEST_F(InvReservedTest, Reorganize_ImpossibleSort_KeepsScrollSpells)
{
	LayOutImpossibleSort(MakeItemOfSize({ 2, 2 }), MakeItemOfSize({ 1, 3 }), MakeReadiedScroll());
	MyPlayer->CalcScrolls();
	const uint64_t scrollSpells = MyPlayer->_pScrlSpells;
	ASSERT_NE(scrollSpells, 0U);

	ReorganizeInventory(*MyPlayer);

	EXPECT_EQ(MyPlayer->_pScrlSpells, scrollSpells);
	EXPECT_EQ(MyPlayer->_pRSpell, SpellID::TownPortal);
	EXPECT_EQ(MyPlayer->_pRSplType, SpellType::Scroll);
}

TEST_F(InvReservedTest, Reorganize_NoReservation_SortStillClearsReadiedScrollSpell)
{
	// Vanilla behaviour: the 1x3 item is placed first, and the check after it finds no scroll yet.
	PlaceAt(MakeReadiedScroll(), 0);
	PlaceAt(MakeItemOfSize({ 1, 3 }), 12);
	MyPlayer->CalcScrolls();
	const uint64_t scrollSpells = MyPlayer->_pScrlSpells;
	ASSERT_NE(scrollSpells, 0U);

	ReorganizeInventory(*MyPlayer);

	EXPECT_EQ(MyPlayer->_pScrlSpells, scrollSpells);
	EXPECT_EQ(MyPlayer->_pRSpell, SpellID::Invalid);
	EXPECT_EQ(MyPlayer->_pRSplType, SpellType::Invalid);
}

TEST_F(InvReservedTest, StoreGoldFit_ReservedSoldItemNotCounted)
{
	PlaceAt(MakeValuableWeapon(), 0);
	FillFreeCells();
	MyPlayer->reservedInvCells.set(0);
	MyPlayer->reservedInvCells.set(10);
	MyPlayer->reservedInvCells.set(20);

	PickOnlyItemToSell(TalkID::SmithSell);

	EXPECT_EQ(ActiveStore, TalkID::NoRoom);
}

TEST_F(InvReservedTest, StoreGoldFit_PartlyReservedSoldItemCountsFreeCellsOnly)
{
	PlaceAt(MakeValuableWeapon(), 0);
	FillFreeCells();
	MyPlayer->reservedInvCells.set(10);
	MyPlayer->reservedInvCells.set(20);

	// One unreserved cell holds a single pile, less than the sale price.
	PickOnlyItemToSell(TalkID::SmithSell);

	EXPECT_EQ(ActiveStore, TalkID::NoRoom);
}

TEST_F(InvReservedTest, StoreGoldFit_SaleKeepsAllGoldWhenFreeCellsSuffice)
{
	PlaceAt(MakeValuableWeapon(), 0);
	FillFreeCells();
	// Cell 20 is the sold item's cell that new gold tries first, and the sale needs just two of its three cells.
	MyPlayer->reservedInvCells.set(20);
	const int goldBefore = CalculateGold(*MyPlayer);

	PickOnlyItemToSell(TalkID::SmithSell);
	ASSERT_EQ(ActiveStore, TalkID::Confirm);
	CurrentTextLine = 18;
	StoreEnter();

	EXPECT_EQ(CalculateGold(*MyPlayer), goldBefore + SalePrice);
	EXPECT_EQ(MyPlayer->InvGrid[20], 0);
}

TEST_F(InvReservedTest, StoreGoldFit_BeltItemCountsItsSizeAndFreeCells)
{
	Item scroll {};
	InitializeItem(scroll, IDI_PORTAL);
	scroll._ivalue = SalePrice * 4;
	scroll._iIvalue = SalePrice * 4;
	MyPlayer->SpdList[0] = scroll;
	FillFreeCells({ 39 });

	PickOnlyItemToSell(TalkID::WitchSell);
	EXPECT_EQ(ActiveStore, TalkID::Confirm);

	MyPlayer->reservedInvCells.set(39);
	PickOnlyItemToSell(TalkID::WitchSell);
	EXPECT_EQ(ActiveStore, TalkID::NoRoom);
}

TEST_F(InvReservedTest, StoreGoldFit_CheapBeltItem_RefusedWhenOnlyReservedFree)
{
	PutCheapScrollInBelt();
	FillFreeCells({ 39 });
	MyPlayer->reservedInvCells.set(39);

	PickOnlyItemToSell(TalkID::WitchSell);

	EXPECT_EQ(ActiveStore, TalkID::NoRoom);
}

TEST_F(InvReservedTest, StoreGoldFit_NoReservation_BeltItemStillCountsItsSize)
{
	// Vanilla behaviour: the sale of a belt item is offered on a full inventory, even though its gold is lost.
	PutCheapScrollInBelt();
	FillFreeCells();

	PickOnlyItemToSell(TalkID::WitchSell);

	EXPECT_EQ(ActiveStore, TalkID::Confirm);
}

TEST_F(InvReservedTest, StoreGoldFit_BeltSlotZero_EmptyReservedCellsAreNotItsCells)
{
	// Belt slot 0 is listed as item index -1, which is also what an empty grid cell decodes to.
	PutCheapScrollInBelt();
	FillFreeCells({ 30, 31, 32, 33, 34, 35, 36, 37, 38, 39 });
	for (int cell = 31; cell < 40; cell++)
		MyPlayer->reservedInvCells.set(cell);
	const int goldBefore = CalculateGold(*MyPlayer);

	PickOnlyItemToSell(TalkID::WitchSell);
	ASSERT_EQ(ActiveStore, TalkID::Confirm);
	CurrentTextLine = 18;
	StoreEnter();

	EXPECT_EQ(CalculateGold(*MyPlayer), goldBefore + 50);
	EXPECT_NE(MyPlayer->InvGrid[30], 0);
	EXPECT_TRUE(MyPlayer->SpdList[0].isEmpty());
}

TEST_F(InvReservedTest, VisualSale_OnlySoldItemCellsUsable_KeepsAllGold)
{
	PlaceAt(MakeValuableWeapon(), 1);
	FillFreeCells({ 0, 10, 20 });
	MyPlayer->reservedInvCells.set(0);
	MyPlayer->reservedInvCells.set(10);
	MyPlayer->reservedInvCells.set(20);
	const int goldBefore = CalculateGold(*MyPlayer);

	SellInVisualStore(1);

	EXPECT_EQ(CalculateGold(*MyPlayer), goldBefore + SalePrice);
	EXPECT_NE(ItemAt(21), nullptr);
	EXPECT_EQ(MyPlayer->InvGrid[0], 0);
	EXPECT_EQ(MyPlayer->InvGrid[10], 0);
	EXPECT_EQ(MyPlayer->InvGrid[20], 0);
}

TEST_F(InvReservedTest, VisualSale_ReservedSoldItemNotCounted)
{
	PlaceAt(MakeValuableWeapon(), 0);
	FillFreeCells();
	MyPlayer->reservedInvCells.set(0);
	MyPlayer->reservedInvCells.set(10);
	MyPlayer->reservedInvCells.set(20);
	const int goldBefore = CalculateGold(*MyPlayer);
	const int itemsBefore = MyPlayer->_pNumInv;

	SellInVisualStore(0);

	EXPECT_EQ(CalculateGold(*MyPlayer), goldBefore);
	EXPECT_EQ(MyPlayer->_pNumInv, itemsBefore);
	ASSERT_NE(ItemAt(0), nullptr);
	EXPECT_NE(ItemAt(0)->_itype, ItemType::Gold);
}

TEST_F(InvReservedTest, VisualPaste_OnlyReservedFree_Refused)
{
	FillFreeCells({ 0 });
	MyPlayer->reservedInvCells.set(0);
	MyPlayer->HoldItem = MakeValuableWeapon();
	const int goldBefore = CalculateGold(*MyPlayer);

	PasteInVisualStore();

	EXPECT_FALSE(MyPlayer->HoldItem.isEmpty());
	EXPECT_EQ(CalculateGold(*MyPlayer), goldBefore);
	EXPECT_EQ(MyPlayer->InvGrid[0], 0);
}

TEST_F(InvReservedTest, VisualSale_PartlyReservedSoldItemCountsFreeCellsOnly)
{
	PlaceAt(MakeValuableWeapon(), 0);
	FillFreeCells();
	MyPlayer->reservedInvCells.set(10);
	MyPlayer->reservedInvCells.set(20);
	const int itemsBefore = MyPlayer->_pNumInv;

	// One unreserved cell holds a single pile, less than the sale price.
	SellInVisualStore(0);

	EXPECT_EQ(MyPlayer->_pNumInv, itemsBefore);
	ASSERT_NE(ItemAt(0), nullptr);
	EXPECT_NE(ItemAt(0)->_itype, ItemType::Gold);
}

TEST_F(InvReservedTest, VisualSale_GoldSpreadsOverFreeCellAndSoldItemCells)
{
	PlaceAt(MakeValuableWeapon(), 1);
	FillFreeCells({ 0, 39 });
	MyPlayer->reservedInvCells.set(0);
	const int goldBefore = CalculateGold(*MyPlayer);

	SellInVisualStore(1);

	EXPECT_EQ(CalculateGold(*MyPlayer), goldBefore + SalePrice);
	EXPECT_NE(MyPlayer->InvGrid[39], 0);
	EXPECT_EQ(MyPlayer->InvGrid[0], 0);
}

TEST_F(InvReservedTest, VisualPaste_RoomOutsideReserved_Sells)
{
	FillFreeCells({ 0, 1, 2 });
	MyPlayer->reservedInvCells.set(0);
	MyPlayer->HoldItem = MakeValuableWeapon();
	const int goldBefore = CalculateGold(*MyPlayer);

	PasteInVisualStore();

	EXPECT_TRUE(MyPlayer->HoldItem.isEmpty());
	EXPECT_EQ(CalculateGold(*MyPlayer), goldBefore + SalePrice);
	EXPECT_EQ(MyPlayer->InvGrid[0], 0);
}

TEST_F(InvReservedTest, VisualSale_NoReservation_GoesThroughOnFullInventory)
{
	// Vanilla behaviour: no room check, and gold that does not fit before the item has left is lost.
	PlaceAt(MakePricelessWeapon(), 0);
	FillFreeCells();
	const int goldBefore = CalculateGold(*MyPlayer);
	const int itemsBefore = MyPlayer->_pNumInv;

	SellInVisualStore(0);

	EXPECT_EQ(MyPlayer->_pNumInv, itemsBefore - 1);
	EXPECT_EQ(CalculateGold(*MyPlayer), goldBefore);
	EXPECT_EQ(MyPlayer->InvGrid[0], 0);
}

TEST_F(InvReservedTest, VisualSale_NoReservation_GoldIsPlacedBeforeItemLeaves)
{
	// Vanilla behaviour: cell 39, where new gold goes first, is not free yet. One pile fits in 38, the rest is lost.
	PlaceAt(MakeValuableWeapon(), 19);
	FillFreeCells({ 38 });
	const int goldBefore = CalculateGold(*MyPlayer);

	SellInVisualStore(19);

	EXPECT_EQ(CalculateGold(*MyPlayer), goldBefore + GOLD_MAX_LIMIT);
	EXPECT_NE(MyPlayer->InvGrid[38], 0);
	EXPECT_EQ(MyPlayer->InvGrid[39], 0);
}

TEST_F(InvReservedTest, VisualPaste_NoReservation_GoesThroughOnFullInventory)
{
	// Vanilla behaviour: no room check, the gold is lost.
	FillFreeCells();
	MyPlayer->HoldItem = MakeValuableWeapon();
	const int goldBefore = CalculateGold(*MyPlayer);

	PasteInVisualStore();

	EXPECT_TRUE(MyPlayer->HoldItem.isEmpty());
	EXPECT_EQ(CalculateGold(*MyPlayer), goldBefore);
}

TEST_F(InvReservedTest, SpiritualShrine_LeavesReservedCellsEmpty)
{
	MyPlayer->reservedInvCells.set(5);
	MyPlayer->reservedInvCells.set(12);

	UseSpiritualShrine();

	EXPECT_EQ(MyPlayer->InvGrid[5], 0);
	EXPECT_EQ(MyPlayer->InvGrid[12], 0);
	EXPECT_EQ(MyPlayer->_pNumInv, InventoryGridCells - 2);
	EXPECT_GT(MyPlayer->InvGrid[39], 0);
}

TEST_F(InvReservedTest, SpiritualShrine_NoReservation_FillsEveryEmptyCell)
{
	PlaceAt(MakePotion(), 0);

	UseSpiritualShrine();

	EXPECT_EQ(MyPlayer->_pNumInv, InventoryGridCells);
	for (int cell = 1; cell < InventoryGridCells; cell++) {
		ASSERT_GT(MyPlayer->InvGrid[cell], 0);
		const Item *pile = ItemAt(cell);
		EXPECT_EQ(pile->_itype, ItemType::Gold);
		EXPECT_GE(pile->_ivalue, 5);
		EXPECT_LE(pile->_ivalue, 14);
	}
	EXPECT_EQ(MyPlayer->_pGold, CalculateGold(*MyPlayer));
}

TEST_F(InvReservedTest, StashTransfer_SkipsReserved)
{
	MyPlayer->reservedInvCells.set(30);
	Stash.SetPage(0);
	ASSERT_TRUE(AutoPlaceItemInStash(MakePotion(), true));

	TransferItemToInventory(*MyPlayer, 0);

	EXPECT_TRUE(Stash.stashList.empty());
	EXPECT_EQ(MyPlayer->InvGrid[30], 0);
	EXPECT_EQ(MyPlayer->InvGrid[31], 1);
}

TEST_F(InvReservedTest, StashTransfer_StaysInStashWhenOnlyReservedFree)
{
	MyPlayer->reservedInvCells.set();
	Stash.SetPage(0);
	ASSERT_TRUE(AutoPlaceItemInStash(MakePotion(), true));

	TransferItemToInventory(*MyPlayer, 0);

	EXPECT_EQ(Stash.stashList.size(), 1u);
	EXPECT_EQ(MyPlayer->_pNumInv, 0);
}

TEST_F(InvReservedTest, ShiftClickUnequip_SkipsReserved)
{
	Item weapon = MakeItemOfSize({ 1, 3 });
	MyPlayer->InvBody[INVLOC_HAND_LEFT] = weapon;
	MyPlayer->reservedInvCells.set(0);

	HoverSlot(SLOTXY_HAND_LEFT);
	CheckInvItem(/*isShiftHeld=*/true);

	EXPECT_TRUE(MyPlayer->InvBody[INVLOC_HAND_LEFT].isEmpty());
	EXPECT_EQ(MyPlayer->InvGrid[0], 0);
	EXPECT_EQ(MyPlayer->InvGrid[1], -1);
	EXPECT_EQ(MyPlayer->InvGrid[21], 1);
}

} // namespace
} // namespace devilution
