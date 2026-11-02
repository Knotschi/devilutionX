/**
 * @file move_items_test.cpp
 *
 * Tests for CheckMoveItems, MoveItems and CanWieldTogether.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <optional>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "ui_test.hpp"

#include "inv.h"
#include "items.h"
#include "msg.h"
#include "player.h"

namespace devilution {
namespace {

ItemLocation Body(inv_body_loc location)
{
	return { ItemLocation::Kind::Body, location };
}

ItemLocation Cell(int cell)
{
	return { ItemLocation::Kind::Cell, cell };
}

class MoveItemsTest : public UITest {
protected:
	void SetUp() override
	{
		UITest::SetUp();
		if (IsSkipped())
			return;

		StripPlayer();
		GivePlentyOfStats();
	}

	void Become(HeroClass heroClass)
	{
		// CreatePlayer() does its own InitCursor()/FreeCursor() cycle, see UITest::SetUp().
		FreeCursor();
		CreatePlayer(*MyPlayer, heroClass);
		InitCursor();
		StripPlayer();
		GivePlentyOfStats();
	}

	static void GivePlentyOfStats(Player &player = *MyPlayer)
	{
		player._pBaseStr = player._pStrength = 250;
		player._pBaseMag = player._pMagic = 250;
		player._pBaseDex = player._pDexterity = 250;
	}

	/** @brief Create the first available item of this kind, with a seed no other test item has. */
	static Item MakeItem(ItemType type, item_equip_type location, std::optional<Size> size = std::nullopt)
	{
		static uint32_t nextSeed = 1;
		for (size_t i = 1; i < AllItemsList.size(); i++) {
			if (AllItemsList[i].itype != type || AllItemsList[i].iLoc != location || !IsItemAvailable(static_cast<int>(i)))
				continue;
			Item item {};
			InitializeItem(item, static_cast<_item_indexes>(i));
			if (size && GetInventorySize(item) != *size)
				continue;
			item._iSeed = nextSeed++;
			item.updateRequiredStatsCacheForPlayer(*MyPlayer);
			return item;
		}
		ADD_FAILURE() << "No such item";
		return {};
	}

	static Item MakeSword() { return MakeItem(ItemType::Sword, ILOC_ONEHAND, Size { 1, 3 }); }
	static Item MakeStaff() { return MakeItem(ItemType::Staff, ILOC_TWOHAND, Size { 2, 3 }); }

	static Item MakeMace() { return MakeItem(ItemType::Mace, ILOC_ONEHAND); }
	static Item MakeTwoHandSword() { return MakeItem(ItemType::Sword, ILOC_TWOHAND); }
	static Item MakeBow() { return MakeItem(ItemType::Bow, ILOC_TWOHAND); }
	static Item MakeRing() { return MakeItem(ItemType::Ring, ILOC_RING); }
	static Item MakeShield() { return MakeItem(ItemType::Shield, ILOC_ONEHAND, Size { 2, 2 }); }

	static Item MakePotion()
	{
		Item item {};
		InitializeItem(item, IDI_HEAL);
		return item;
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

	/** @brief Exactly these cells hold this item. */
	static void ExpectCellsOf(const Item &item, const std::vector<int> &cells)
	{
		std::vector<int> actual;
		for (int cell = 0; cell < InventoryGridCells; cell++) {
			const int id = std::abs(MyPlayer->InvGrid[cell]);
			if (id != 0 && MyPlayer->InvList[id - 1]._iSeed == item._iSeed)
				actual.push_back(cell);
		}
		EXPECT_EQ(actual, cells);
	}

	/** @brief Every listed item covers a rectangle of its size, encoded the way the game does, and no cell refers to anything else. */
	static void ExpectGridMatchesInvList()
	{
		const Player &player = *MyPlayer;
		int coveredCells = 0;
		for (int i = 0; i < player._pNumInv; i++) {
			const int id = i + 1;
			int topLeft = -1;
			int cellCount = 0;
			for (int cell = 0; cell < InventoryGridCells; cell++) {
				if (std::abs(player.InvGrid[cell]) != id)
					continue;
				if (topLeft == -1)
					topLeft = cell;
				cellCount++;
			}
			ASSERT_NE(topLeft, -1) << "item " << i << " is not on the grid";
			const Size size = GetInventorySize(player.InvList[i]);
			EXPECT_EQ(cellCount, size.width * size.height) << "item " << i;
			for (int y = 0; y < size.height; y++) {
				for (int x = 0; x < size.width; x++) {
					const int expected = x == 0 && y == size.height - 1 ? id : -id;
					EXPECT_EQ(player.InvGrid[topLeft + (y * 10) + x], expected) << "item " << i;
				}
			}
			coveredCells += cellCount;
		}
		int occupiedCells = 0;
		for (const int8_t cell : player.InvGrid)
			occupiedCells += cell != 0 ? 1 : 0;
		EXPECT_EQ(occupiedCells, coveredCells) << "a cell refers to no listed item";
	}

	/** @brief Everything a move could touch, as raw bytes. */
	static std::vector<std::byte> Snapshot()
	{
		const Player &player = *MyPlayer;
		std::vector<std::byte> bytes;
		const auto append = [&bytes](const auto &value) {
			const auto *begin = reinterpret_cast<const std::byte *>(&value);
			bytes.insert(bytes.end(), begin, begin + sizeof(value));
		};
		append(player.InvBody);
		append(player.InvList);
		append(player.InvGrid);
		append(player.SpdList);
		append(player.HoldItem);
		append(player._pNumInv);
		append(player._pGold);
		append(player._pIMinDam);
		append(player._pIMaxDam);
		append(player._pIAC);
		append(player._pScrlSpells);
		append(player._pISpells);
		return bytes;
	}

	/** @brief Both entry points reject the moves with this reason, and neither changes anything. */
	static void ExpectRejected(std::span<const ItemMove> moves, MoveItemsError expected)
	{
		const std::vector<std::byte> before = Snapshot();
		EXPECT_EQ(CheckMoveItems(*MyPlayer, moves), expected);
		EXPECT_TRUE(Snapshot() == before) << "CheckMoveItems changed the state";
		EXPECT_EQ(MoveItems(*MyPlayer, moves), expected);
		EXPECT_TRUE(Snapshot() == before) << "MoveItems changed the state";
	}

	/** @brief How many copies of this item the player has, wherever they are. */
	static int Copies(const Item &item)
	{
		const Player &player = *MyPlayer;
		// A cleared item keeps its seed, only its type says it is gone.
		const auto isCopy = [&item](const Item &other) { return !other.isEmpty() && other._iSeed == item._iSeed; };
		int copies = isCopy(player.HoldItem) ? 1 : 0;
		for (const Item &bodyItem : player.InvBody)
			copies += isCopy(bodyItem) ? 1 : 0;
		for (int i = 0; i < player._pNumInv; i++)
			copies += isCopy(player.InvList[i]) ? 1 : 0;
		return copies;
	}
};

/**
 * Replays what MoveItems sends on a second player through the real message handlers, as if it
 * had arrived from another client, and compares that replica with the local player.
 */
class MoveItemsReplicaTest : public MoveItemsTest {
protected:
	void SetUp() override
	{
		MoveItemsTest::SetUp();
		if (IsSkipped())
			return;

		Players.resize(2);
		MyPlayer = &Players[0];
		// CreatePlayer() does its own InitCursor()/FreeCursor() cycle, see UITest::SetUp().
		FreeCursor();
		CreatePlayer(Players[1], HeroClass::Warrior);
		InitCursor();
		GivePlentyOfStats(Players[1]);
	}

	/** @brief Run moves that must succeed on a replica that starts out identical. @return The commands sent, in order. */
	static std::vector<_cmd_id> MoveAndReplay(std::span<const ItemMove> moves)
	{
		Player &local = Players[0];
		Player &replica = Players[1];
		std::copy(std::begin(local.InvBody), std::end(local.InvBody), std::begin(replica.InvBody));
		std::copy(std::begin(local.InvList), std::end(local.InvList), std::begin(replica.InvList));
		std::copy(std::begin(local.InvGrid), std::end(local.InvGrid), std::begin(replica.InvGrid));
		replica._pNumInv = local._pNumInv;
		ReplayMessages(/*onReplica=*/false);

		EXPECT_EQ(MoveItems(local, moves), std::nullopt);

		return ReplayMessages(/*onReplica=*/true);
	}

	/** @brief Empty the loopback queue. @return The commands it held, in order. */
	static std::vector<_cmd_id> ReplayMessages(bool onReplica)
	{
		std::vector<_cmd_id> commands;
		uint8_t sender;
		void *data;
		size_t size;
		while (SNetReceiveMessage(&sender, &data, &size)) {
			const auto *command = reinterpret_cast<const TCmd *>(static_cast<const std::byte *>(data) + sizeof(TPktHdr));
			commands.push_back(command->bCmd);
			if (onReplica)
				ParseCmd(1, command, size - sizeof(TPktHdr));
		}
		return commands;
	}

	static std::optional<uint32_t> SeedInCell(const Player &player, int cell)
	{
		const int id = std::abs(player.InvGrid[cell]);
		if (id == 0)
			return std::nullopt;
		return player.InvList[id - 1]._iSeed;
	}

	static std::optional<uint32_t> SeedOnBody(const Player &player, int location)
	{
		if (player.InvBody[location].isEmpty())
			return std::nullopt;
		return player.InvBody[location]._iSeed;
	}

	/** @brief The replica holds the same items in the same places as the local player, though InvList order may differ. */
	static void ExpectReplicaMatches()
	{
		const Player &local = Players[0];
		const Player &replica = Players[1];
		EXPECT_EQ(replica._pNumInv, local._pNumInv);
		for (int location = 0; location < NUM_INVLOC; location++)
			EXPECT_EQ(SeedOnBody(replica, location), SeedOnBody(local, location)) << "body location " << location;
		for (int cell = 0; cell < InventoryGridCells; cell++) {
			EXPECT_EQ(SeedInCell(replica, cell), SeedInCell(local, cell)) << "cell " << cell;
			EXPECT_EQ(replica.InvGrid[cell] > 0, local.InvGrid[cell] > 0) << "main cell marker of cell " << cell;
		}
	}
};

TEST_F(MoveItemsReplicaTest, CellToBody)
{
	PlaceAt(MakeSword(), 5);
	const ItemMove moves[] = { { Cell(5), Body(INVLOC_HAND_LEFT) } };

	const std::vector<_cmd_id> commands = MoveAndReplay(moves);

	EXPECT_EQ(commands, (std::vector<_cmd_id> { CMD_DELINVITEMS, CMD_CHANGEPLRITEMS }));
	EXPECT_FALSE(Players[1].InvBody[INVLOC_HAND_LEFT].isEmpty());
	ExpectReplicaMatches();
}

TEST_F(MoveItemsReplicaTest, BodyToCell)
{
	MyPlayer->InvBody[INVLOC_HAND_LEFT] = MakeSword();
	const ItemMove moves[] = { { Body(INVLOC_HAND_LEFT), Cell(5) } };

	const std::vector<_cmd_id> commands = MoveAndReplay(moves);

	EXPECT_EQ(commands, (std::vector<_cmd_id> { CMD_DELPLRITEMS, CMD_CHANGEINVITEMS }));
	EXPECT_TRUE(Players[1].InvBody[INVLOC_HAND_LEFT].isEmpty());
	ExpectReplicaMatches();
}

TEST_F(MoveItemsReplicaTest, CellToCell)
{
	PlaceAt(MakeSword(), 5);
	PlaceAt(MakeRing(), 39);
	const ItemMove moves[] = { { Cell(15), Cell(0) } };

	const std::vector<_cmd_id> commands = MoveAndReplay(moves);

	EXPECT_EQ(commands, (std::vector<_cmd_id> { CMD_DELINVITEMS, CMD_CHANGEINVITEMS }));
	ExpectReplicaMatches();
}

TEST_F(MoveItemsReplicaTest, Swap_StaffForSwordAndShield)
{
	MyPlayer->InvBody[INVLOC_HAND_LEFT] = MakeSword();
	MyPlayer->InvBody[INVLOC_HAND_RIGHT] = MakeShield();
	PlaceAt(MakeRing(), 39);
	PlaceAt(MakeStaff(), 0);
	PlaceAt(MakeRing(), 38);
	const ItemMove moves[] = {
		{ Body(INVLOC_HAND_LEFT), Cell(0) },
		{ Body(INVLOC_HAND_RIGHT), Cell(1) },
		{ Cell(10), Body(INVLOC_HAND_LEFT) },
	};

	const std::vector<_cmd_id> commands = MoveAndReplay(moves);

	EXPECT_EQ(commands, (std::vector<_cmd_id> { CMD_DELPLRITEMS, CMD_DELPLRITEMS, CMD_DELINVITEMS, CMD_CHANGEINVITEMS, CMD_CHANGEINVITEMS, CMD_CHANGEPLRITEMS }));
	ExpectReplicaMatches();
}

TEST_F(MoveItemsReplicaTest, SeveralInventorySources_ShiftIndices)
{
	PlaceAt(MakeRing(), 0);
	PlaceAt(MakeRing(), 1);
	PlaceAt(MakeRing(), 2);
	PlaceAt(MakeRing(), 3);
	const ItemMove moves[] = {
		{ Cell(0), Body(INVLOC_RING_LEFT) },
		{ Cell(1), Body(INVLOC_RING_RIGHT) },
		{ Cell(3), Cell(20) },
	};

	const std::vector<_cmd_id> commands = MoveAndReplay(moves);

	EXPECT_EQ(commands, (std::vector<_cmd_id> { CMD_DELINVITEMS, CMD_DELINVITEMS, CMD_DELINVITEMS, CMD_CHANGEINVITEMS, CMD_CHANGEPLRITEMS, CMD_CHANGEPLRITEMS }));
	ExpectReplicaMatches();
}

TEST_F(MoveItemsReplicaTest, Rejected_SendsNothing)
{
	MyPlayer->InvBody[INVLOC_HAND_LEFT] = MakeSword();
	PlaceAt(MakeRing(), 0);
	ReplayMessages(/*onReplica=*/false);
	const ItemMove moves[] = { { Body(INVLOC_HAND_LEFT), Cell(0) } };

	ASSERT_EQ(MoveItems(*MyPlayer, moves), MoveItemsError::NoRoom);

	EXPECT_TRUE(ReplayMessages(/*onReplica=*/false).empty());
}

TEST_F(MoveItemsTest, CellToBody_EquipsAndClearsGrid)
{
	const Item sword = MakeSword();
	PlaceAt(sword, 5);
	const ItemMove moves[] = { { Cell(5), Body(INVLOC_HAND_LEFT) } };

	EXPECT_EQ(MoveItems(*MyPlayer, moves), std::nullopt);

	EXPECT_EQ(MyPlayer->InvBody[INVLOC_HAND_LEFT]._iSeed, sword._iSeed);
	EXPECT_EQ(MyPlayer->_pNumInv, 0);
	for (int cell = 0; cell < InventoryGridCells; cell++)
		EXPECT_EQ(MyPlayer->InvGrid[cell], 0) << "cell " << cell;
	EXPECT_EQ(Copies(sword), 1);
}

TEST_F(MoveItemsTest, BodyToCell_Unequips)
{
	const Item sword = MakeSword();
	MyPlayer->InvBody[INVLOC_HAND_LEFT] = sword;
	const ItemMove moves[] = { { Body(INVLOC_HAND_LEFT), Cell(5) } };

	EXPECT_EQ(MoveItems(*MyPlayer, moves), std::nullopt);

	EXPECT_TRUE(MyPlayer->InvBody[INVLOC_HAND_LEFT].isEmpty());
	ASSERT_EQ(MyPlayer->_pNumInv, 1);
	EXPECT_EQ(MyPlayer->InvList[0]._iSeed, sword._iSeed);
	EXPECT_EQ(MyPlayer->InvGrid[5], -1);
	EXPECT_EQ(MyPlayer->InvGrid[15], -1);
	EXPECT_EQ(MyPlayer->InvGrid[25], 1);
	EXPECT_EQ(Copies(sword), 1);
}

TEST_F(MoveItemsTest, Swap_StaffForSword_SameCells)
{
	const Item staff = MakeStaff();
	const Item sword = MakeSword();
	MyPlayer->InvBody[INVLOC_HAND_LEFT] = staff;
	PlaceAt(sword, 0);
	const ItemMove moves[] = {
		{ Body(INVLOC_HAND_LEFT), Cell(0) },
		{ Cell(0), Body(INVLOC_HAND_LEFT) },
	};

	EXPECT_EQ(MoveItems(*MyPlayer, moves), std::nullopt);

	EXPECT_EQ(MyPlayer->InvBody[INVLOC_HAND_LEFT]._iSeed, sword._iSeed);
	ASSERT_EQ(MyPlayer->_pNumInv, 1);
	EXPECT_EQ(MyPlayer->InvList[0]._iSeed, staff._iSeed);
	EXPECT_EQ(Copies(staff), 1);
	EXPECT_EQ(Copies(sword), 1);
	ExpectGridMatchesInvList();
	ExpectCellsOf(staff, { 0, 1, 10, 11, 20, 21 });
}

TEST_F(MoveItemsTest, Rejects_NonMovingItem)
{
	const Item sword = MakeSword();
	const Item potion = MakePotion();
	MyPlayer->InvBody[INVLOC_HAND_LEFT] = sword;
	PlaceAt(potion, 20);
	const ItemMove moves[] = { { Body(INVLOC_HAND_LEFT), Cell(0) } };

	ExpectRejected(moves, MoveItemsError::NoRoom);
}

TEST_F(MoveItemsTest, Rejects_OutOfBounds_PastRightEdge)
{
	MyPlayer->InvBody[INVLOC_HAND_LEFT] = MakeStaff();
	const ItemMove moves[] = { { Body(INVLOC_HAND_LEFT), Cell(9) } };

	ExpectRejected(moves, MoveItemsError::NoRoom);
}

TEST_F(MoveItemsTest, Rejects_OutOfBounds_PastBottomEdge)
{
	MyPlayer->InvBody[INVLOC_HAND_LEFT] = MakeStaff();
	const ItemMove moves[] = { { Body(INVLOC_HAND_LEFT), Cell(30) } };

	ExpectRejected(moves, MoveItemsError::NoRoom);
}

TEST_F(MoveItemsTest, Rejects_OccupiedBodySlot_WouldLoseItsItem)
{
	const Item worn = MakeRing();
	MyPlayer->InvBody[INVLOC_RING_LEFT] = worn;
	PlaceAt(MakeRing(), 0);
	const ItemMove moves[] = { { Cell(0), Body(INVLOC_RING_LEFT) } };

	ExpectRejected(moves, MoveItemsError::NoRoom);
	EXPECT_EQ(Copies(worn), 1);
}

TEST_F(MoveItemsTest, Rejects_NonEquipment)
{
	PlaceAt(MakePotion(), 0);
	const ItemMove moves[] = { { Cell(0), Body(INVLOC_HAND_LEFT) } };

	ExpectRejected(moves, MoveItemsError::CannotEquip);
}

TEST_F(MoveItemsTest, Rejects_StatsTooLow)
{
	Item sword = MakeSword();
	sword._iMinStr = 100;
	MyPlayer->_pStrength = 99;
	PlaceAt(sword, 0);
	const ItemMove moves[] = { { Cell(0), Body(INVLOC_HAND_LEFT) } };

	ExpectRejected(moves, MoveItemsError::CannotEquip);
}

TEST_F(MoveItemsTest, Rejects_WrongSlotType)
{
	PlaceAt(MakeRing(), 0);
	const ItemMove moves[] = { { Cell(0), Body(INVLOC_HEAD) } };

	ExpectRejected(moves, MoveItemsError::CannotEquip);
}

TEST_F(MoveItemsTest, Rejects_TwoHandWithOffhand)
{
	MyPlayer->InvBody[INVLOC_HAND_RIGHT] = MakeShield();
	PlaceAt(MakeStaff(), 0);
	const ItemMove moves[] = { { Cell(0), Body(INVLOC_HAND_LEFT) } };

	ExpectRejected(moves, MoveItemsError::CannotEquip);
}

TEST_F(MoveItemsTest, Rejects_TwoHandInRightHand)
{
	PlaceAt(MakeStaff(), 0);
	const ItemMove moves[] = { { Cell(0), Body(INVLOC_HAND_RIGHT) } };

	ExpectRejected(moves, MoveItemsError::CannotEquip);
}

TEST_F(MoveItemsTest, Rejects_TwoShields)
{
	MyPlayer->InvBody[INVLOC_HAND_LEFT] = MakeShield();
	PlaceAt(MakeShield(), 0);
	const ItemMove moves[] = { { Cell(0), Body(INVLOC_HAND_RIGHT) } };

	ExpectRejected(moves, MoveItemsError::CannotEquip);
}

TEST_F(MoveItemsTest, Rejects_BothTargetsLegalAloneButNotTogether)
{
	PlaceAt(MakeStaff(), 0);
	PlaceAt(MakeShield(), 2);
	const ItemMove moves[] = {
		{ Cell(0), Body(INVLOC_HAND_LEFT) },
		{ Cell(2), Body(INVLOC_HAND_RIGHT) },
	};

	ExpectRejected(moves, MoveItemsError::CannotEquip);
}

TEST_F(MoveItemsTest, Bard_DualSwordMace_Ok)
{
	Become(HeroClass::Bard);
	const Item sword = MakeSword();
	const Item mace = MakeMace();
	PlaceAt(sword, 0);
	PlaceAt(mace, 2);
	const ItemMove moves[] = {
		{ Cell(0), Body(INVLOC_HAND_LEFT) },
		{ Cell(2), Body(INVLOC_HAND_RIGHT) },
	};

	EXPECT_EQ(MoveItems(*MyPlayer, moves), std::nullopt);

	EXPECT_EQ(MyPlayer->InvBody[INVLOC_HAND_LEFT]._iSeed, sword._iSeed);
	EXPECT_EQ(MyPlayer->InvBody[INVLOC_HAND_RIGHT]._iSeed, mace._iSeed);
}

TEST_F(MoveItemsTest, Warrior_DualSword_Rejected)
{
	PlaceAt(MakeSword(), 0);
	PlaceAt(MakeSword(), 1);
	const ItemMove moves[] = {
		{ Cell(0), Body(INVLOC_HAND_LEFT) },
		{ Cell(1), Body(INVLOC_HAND_RIGHT) },
	};

	ExpectRejected(moves, MoveItemsError::CannotEquip);
}

TEST_F(MoveItemsTest, Barbarian_2HSwordPlusShield_Ok)
{
	Become(HeroClass::Barbarian);
	const Item greatSword = MakeTwoHandSword();
	const Item shield = MakeShield();
	PlaceAt(greatSword, 0);
	PlaceAt(shield, 2);
	const ItemMove moves[] = {
		{ Cell(0), Body(INVLOC_HAND_LEFT) },
		{ Cell(2), Body(INVLOC_HAND_RIGHT) },
	};

	EXPECT_EQ(MoveItems(*MyPlayer, moves), std::nullopt);

	EXPECT_EQ(MyPlayer->InvBody[INVLOC_HAND_LEFT]._iSeed, greatSword._iSeed);
	EXPECT_EQ(MyPlayer->InvBody[INVLOC_HAND_RIGHT]._iSeed, shield._iSeed);
}

TEST_F(MoveItemsTest, Barbarian_2HSwordInRightHand_Ok)
{
	Become(HeroClass::Barbarian);
	const Item greatSword = MakeTwoHandSword();
	PlaceAt(greatSword, 0);
	const ItemMove moves[] = { { Cell(0), Body(INVLOC_HAND_RIGHT) } };

	EXPECT_EQ(MoveItems(*MyPlayer, moves), std::nullopt);

	EXPECT_EQ(MyPlayer->InvBody[INVLOC_HAND_RIGHT]._iSeed, greatSword._iSeed);
}

TEST_F(MoveItemsTest, Barbarian_Bow_Plus_Shield_Rejected)
{
	Become(HeroClass::Barbarian);
	PlaceAt(MakeBow(), 0);
	PlaceAt(MakeShield(), 2);
	const ItemMove moves[] = {
		{ Cell(0), Body(INVLOC_HAND_LEFT) },
		{ Cell(2), Body(INVLOC_HAND_RIGHT) },
	};

	ExpectRejected(moves, MoveItemsError::CannotEquip);
}

TEST_F(MoveItemsTest, Busy_WhenAttacking)
{
	MyPlayer->InvBody[INVLOC_HAND_LEFT] = MakeSword();
	MyPlayer->_pmode = PM_ATTACK;
	const ItemMove moves[] = { { Body(INVLOC_HAND_LEFT), Cell(0) } };

	ExpectRejected(moves, MoveItemsError::Busy);
}

TEST_F(MoveItemsTest, Busy_BodyTargetWhenAttacking)
{
	PlaceAt(MakeSword(), 0);
	MyPlayer->_pmode = PM_ATTACK;
	const ItemMove moves[] = { { Cell(0), Body(INVLOC_HAND_LEFT) } };

	ExpectRejected(moves, MoveItemsError::Busy);
}

TEST_F(MoveItemsTest, CellToCell_AllowedWhenAttacking)
{
	const Item sword = MakeSword();
	PlaceAt(sword, 0);
	MyPlayer->_pmode = PM_ATTACK;
	const ItemMove moves[] = { { Cell(0), Cell(9) } };

	EXPECT_EQ(MoveItems(*MyPlayer, moves), std::nullopt);

	ExpectCellsOf(sword, { 9, 19, 29 });
}

TEST_F(MoveItemsTest, Busy_InStore)
{
	PlaceAt(MakeSword(), 0);
	ActiveStore = TalkID::Smith;
	const ItemMove moves[] = { { Cell(0), Cell(9) } };

	ExpectRejected(moves, MoveItemsError::Busy);
}

TEST_F(MoveItemsTest, Busy_RemotePlayer)
{
	Players.resize(2);
	MyPlayer = &Players[0];
	const Item sword = MakeSword();
	Players[1].InvList[0] = sword;
	Players[1]._pNumInv = 1;
	Players[1].InvGrid[0] = -1;
	Players[1].InvGrid[10] = -1;
	Players[1].InvGrid[20] = 1;
	const ItemMove moves[] = { { Cell(0), Cell(9) } };

	EXPECT_EQ(CheckMoveItems(Players[1], moves), MoveItemsError::Busy);
	EXPECT_EQ(MoveItems(Players[1], moves), MoveItemsError::Busy);

	EXPECT_EQ(Players[1].InvGrid[20], 1);
	EXPECT_EQ(Players[1].InvGrid[29], 0);
}

TEST_F(MoveItemsTest, AllowedWhileWalking)
{
	const Item sword = MakeSword();
	PlaceAt(sword, 0);
	MyPlayer->_pmode = PM_WALK_SIDEWAYS;
	const ItemMove moves[] = { { Cell(0), Body(INVLOC_HAND_LEFT) } };

	EXPECT_EQ(MoveItems(*MyPlayer, moves), std::nullopt);

	EXPECT_EQ(MyPlayer->InvBody[INVLOC_HAND_LEFT]._iSeed, sword._iSeed);
}

TEST_F(MoveItemsTest, Invalid_EmptyList)
{
	ExpectRejected({}, MoveItemsError::Invalid);
}

TEST_F(MoveItemsTest, Invalid_TooManyMoves)
{
	std::vector<ItemMove> moves;
	for (int cell = 0; cell < 17; cell++) {
		PlaceAt(MakePotion(), cell);
		moves.push_back({ Cell(cell), Cell(cell + 20) });
	}

	ExpectRejected(moves, MoveItemsError::Invalid);
}

TEST_F(MoveItemsTest, SixteenMoves_Done)
{
	std::vector<ItemMove> moves;
	for (int cell = 0; cell < 16; cell++) {
		PlaceAt(MakePotion(), cell);
		moves.push_back({ Cell(cell), Cell(cell + 20) });
	}

	EXPECT_EQ(MoveItems(*MyPlayer, moves), std::nullopt);

	EXPECT_EQ(MyPlayer->_pNumInv, 16);
	EXPECT_EQ(MyPlayer->InvGrid[0], 0);
	EXPECT_NE(MyPlayer->InvGrid[35], 0);
	ExpectGridMatchesInvList();
}

TEST_F(MoveItemsTest, Invalid_LocationOutOfRange)
{
	const Item sword = MakeSword();
	MyPlayer->InvBody[INVLOC_HAND_LEFT] = sword;
	PlaceAt(MakePotion(), 0);
	const ItemLocation outOfRange[] = {
		Cell(-1),
		Cell(InventoryGridCells),
		{ ItemLocation::Kind::Body, -1 },
		{ ItemLocation::Kind::Body, NUM_INVLOC },
	};

	for (const ItemLocation location : outOfRange) {
		const ItemMove asSource[] = { { location, Cell(5) } };
		ExpectRejected(asSource, MoveItemsError::Invalid);
		const ItemMove asTarget[] = { { Cell(0), location } };
		ExpectRejected(asTarget, MoveItemsError::Invalid);
	}
}

TEST_F(MoveItemsTest, Invalid_DuplicateSource)
{
	const Item potion = MakePotion();
	PlaceAt(potion, 0);
	MyPlayer->InvBody[INVLOC_RING_LEFT] = MakeRing();
	const ItemMove sameCell[] = {
		{ Cell(0), Cell(1) },
		{ Cell(0), Cell(2) },
	};
	const ItemMove sameBodySlot[] = {
		{ Body(INVLOC_RING_LEFT), Cell(1) },
		{ Body(INVLOC_RING_LEFT), Cell(2) },
	};

	ExpectRejected(sameCell, MoveItemsError::Invalid);
	ExpectRejected(sameBodySlot, MoveItemsError::Invalid);
}

TEST_F(MoveItemsTest, Invalid_DuplicateTarget)
{
	PlaceAt(MakePotion(), 0);
	PlaceAt(MakePotion(), 1);
	PlaceAt(MakeRing(), 2);
	PlaceAt(MakeRing(), 3);
	const ItemMove sameCell[] = {
		{ Cell(0), Cell(5) },
		{ Cell(1), Cell(5) },
	};
	const ItemMove sameBodySlot[] = {
		{ Cell(2), Body(INVLOC_RING_LEFT) },
		{ Cell(3), Body(INVLOC_RING_LEFT) },
	};

	ExpectRejected(sameCell, MoveItemsError::Invalid);
	ExpectRejected(sameBodySlot, MoveItemsError::Invalid);
}

TEST_F(MoveItemsTest, Missing_EmptySource)
{
	const ItemMove emptyCell[] = { { Cell(0), Cell(1) } };
	const ItemMove emptyBodySlot[] = { { Body(INVLOC_HAND_LEFT), Cell(1) } };

	ExpectRejected(emptyCell, MoveItemsError::Missing);
	ExpectRejected(emptyBodySlot, MoveItemsError::Missing);
}

TEST_F(MoveItemsTest, Invalid_TwoCellsOfOneItem_WouldDuplicateIt)
{
	const Item sword = MakeSword();
	PlaceAt(sword, 0);
	const ItemMove moves[] = {
		{ Cell(0), Cell(1) },
		{ Cell(10), Cell(2) },
	};

	ExpectRejected(moves, MoveItemsError::Invalid);
	EXPECT_EQ(Copies(sword), 1);
}

TEST_F(MoveItemsTest, FromAnyCellOfItem_Resolves)
{
	const Item staff = MakeStaff();
	PlaceAt(staff, 0);
	const ItemMove moves[] = { { Cell(21), Cell(5) } };

	EXPECT_EQ(MoveItems(*MyPlayer, moves), std::nullopt);

	ExpectCellsOf(staff, { 5, 6, 15, 16, 25, 26 });
	ExpectGridMatchesInvList();
}

TEST_F(MoveItemsTest, MultipleInvSources_IndexShiftSafe)
{
	const Item first = MakeSword();
	const Item stays = MakePotion();
	const Item last = MakeRing();
	const Item alsoStays = MakeBow();
	PlaceAt(first, 0);
	PlaceAt(stays, 1);
	PlaceAt(last, 2);
	PlaceAt(alsoStays, 3);
	const ItemMove moves[] = {
		{ Cell(0), Body(INVLOC_HAND_LEFT) },
		{ Cell(2), Body(INVLOC_RING_LEFT) },
	};

	EXPECT_EQ(MoveItems(*MyPlayer, moves), std::nullopt);

	EXPECT_EQ(MyPlayer->InvBody[INVLOC_HAND_LEFT]._iSeed, first._iSeed);
	EXPECT_EQ(MyPlayer->InvBody[INVLOC_RING_LEFT]._iSeed, last._iSeed);
	EXPECT_EQ(MyPlayer->_pNumInv, 2);
	ExpectCellsOf(stays, { 1 });
	EXPECT_EQ(Copies(alsoStays), 1);
	ExpectGridMatchesInvList();
}

TEST_F(MoveItemsTest, CalcPlrInv_Applied)
{
	Item sword = MakeSword();
	sword._iMinDam = 40;
	sword._iMaxDam = 77;
	PlaceAt(sword, 0);
	const ItemMove moves[] = { { Cell(0), Body(INVLOC_HAND_LEFT) } };

	EXPECT_EQ(MoveItems(*MyPlayer, moves), std::nullopt);

	EXPECT_EQ(MyPlayer->_pIMaxDam, 77);
}

TEST_F(MoveItemsTest, ClosesGoldDrop)
{
	PlaceAt(MakePotion(), 0);
	DropGoldFlag = true;
	const ItemMove moves[] = { { Cell(0), Cell(5) } };

	EXPECT_EQ(MoveItems(*MyPlayer, moves), std::nullopt);

	EXPECT_FALSE(DropGoldFlag);
}

TEST_F(MoveItemsTest, Rejected_LeavesGoldDropOpen)
{
	DropGoldFlag = true;
	const ItemMove moves[] = { { Cell(0), Cell(5) } };

	EXPECT_EQ(MoveItems(*MyPlayer, moves), MoveItemsError::Missing);

	EXPECT_TRUE(DropGoldFlag);
	CloseGoldDrop();
}

TEST_F(MoveItemsTest, CheckMoveItems_DoesNotMutate)
{
	const Item sword = MakeSword();
	const Item ring = MakeRing();
	PlaceAt(sword, 0);
	MyPlayer->InvBody[INVLOC_RING_LEFT] = ring;
	const ItemMove moves[] = {
		{ Cell(0), Body(INVLOC_HAND_LEFT) },
		{ Body(INVLOC_RING_LEFT), Cell(1) },
	};
	const std::vector<std::byte> before = Snapshot();

	EXPECT_EQ(CheckMoveItems(*MyPlayer, moves), std::nullopt);

	EXPECT_TRUE(Snapshot() == before);
}

TEST_F(MoveItemsTest, BodyToBody_SwapsRings)
{
	const Item left = MakeRing();
	const Item right = MakeRing();
	MyPlayer->InvBody[INVLOC_RING_LEFT] = left;
	MyPlayer->InvBody[INVLOC_RING_RIGHT] = right;
	const ItemMove moves[] = {
		{ Body(INVLOC_RING_LEFT), Body(INVLOC_RING_RIGHT) },
		{ Body(INVLOC_RING_RIGHT), Body(INVLOC_RING_LEFT) },
	};

	EXPECT_EQ(MoveItems(*MyPlayer, moves), std::nullopt);

	EXPECT_EQ(MyPlayer->InvBody[INVLOC_RING_LEFT]._iSeed, right._iSeed);
	EXPECT_EQ(MyPlayer->InvBody[INVLOC_RING_RIGHT]._iSeed, left._iSeed);
}

TEST_F(MoveItemsTest, SwordAndShieldForStaff_ThreeItemsNoLoss)
{
	const Item sword = MakeSword();
	const Item shield = MakeShield();
	const Item staff = MakeStaff();
	MyPlayer->InvBody[INVLOC_HAND_LEFT] = sword;
	MyPlayer->InvBody[INVLOC_HAND_RIGHT] = shield;
	PlaceAt(staff, 0);
	const ItemMove moves[] = {
		{ Body(INVLOC_HAND_LEFT), Cell(0) },
		{ Body(INVLOC_HAND_RIGHT), Cell(1) },
		{ Cell(0), Body(INVLOC_HAND_LEFT) },
	};

	EXPECT_EQ(MoveItems(*MyPlayer, moves), std::nullopt);

	EXPECT_EQ(MyPlayer->InvBody[INVLOC_HAND_LEFT]._iSeed, staff._iSeed);
	EXPECT_TRUE(MyPlayer->InvBody[INVLOC_HAND_RIGHT].isEmpty());
	ExpectCellsOf(sword, { 0, 10, 20 });
	ExpectCellsOf(shield, { 1, 2, 11, 12 });
	EXPECT_EQ(Copies(sword), 1);
	EXPECT_EQ(Copies(shield), 1);
	EXPECT_EQ(Copies(staff), 1);
	ExpectGridMatchesInvList();
}

TEST_F(MoveItemsTest, Gold_CellToCell_KeepsTotal)
{
	Item gold {};
	MakeGoldStack(gold, 1234);
	PlaceAt(gold, 0);
	MyPlayer->_pGold = 1234;
	const ItemMove moves[] = { { Cell(0), Cell(5) } };

	EXPECT_EQ(MoveItems(*MyPlayer, moves), std::nullopt);

	EXPECT_EQ(MyPlayer->_pGold, 1234);
	EXPECT_EQ(MyPlayer->InvGrid[0], 0);
	EXPECT_EQ(MyPlayer->InvGrid[5], 1);
	EXPECT_EQ(MyPlayer->InvList[0]._ivalue, 1234);
}

TEST_F(MoveItemsTest, ItemOnCursor_Untouched)
{
	const Item held = MakeBow();
	const Item sword = MakeSword();
	MyPlayer->HoldItem = held;
	PlaceAt(sword, 0);
	const ItemMove moves[] = { { Cell(0), Body(INVLOC_HAND_LEFT) } };

	EXPECT_EQ(MoveItems(*MyPlayer, moves), std::nullopt);

	EXPECT_FALSE(MyPlayer->HoldItem.isEmpty());
	EXPECT_EQ(MyPlayer->HoldItem._iSeed, held._iSeed);
}

TEST_F(MoveItemsTest, UnequippedItem_StatFlagRefreshed)
{
	Item sword = MakeSword();
	sword._iMinStr = 100;
	sword._iStatFlag = true;
	MyPlayer->InvBody[INVLOC_HAND_LEFT] = sword;
	MyPlayer->_pBaseStr = 50;
	MyPlayer->_pStrength = 50;
	const ItemMove moves[] = { { Body(INVLOC_HAND_LEFT), Cell(0) } };

	EXPECT_EQ(MoveItems(*MyPlayer, moves), std::nullopt);

	ASSERT_EQ(MyPlayer->_pNumInv, 1);
	EXPECT_FALSE(MyPlayer->InvList[0]._iStatFlag);
}

TEST_F(MoveItemsTest, CanWieldTogether_SwordAndShield)
{
	EXPECT_TRUE(CanWieldTogether(*MyPlayer, MakeSword(), MakeShield()));
	EXPECT_TRUE(CanWieldTogether(*MyPlayer, MakeShield(), MakeSword()));
}

TEST_F(MoveItemsTest, CanWieldTogether_Nothing_False)
{
	EXPECT_FALSE(CanWieldTogether(*MyPlayer, MakeSword(), Item {}));
	EXPECT_FALSE(CanWieldTogether(*MyPlayer, Item {}, MakeSword()));
}

TEST_F(MoveItemsTest, CanWieldTogether_TwoSwords_Warrior_False)
{
	EXPECT_FALSE(CanWieldTogether(*MyPlayer, MakeSword(), MakeSword()));
}

TEST_F(MoveItemsTest, CanWieldTogether_SwordAndMace_Bard_True)
{
	Become(HeroClass::Bard);

	EXPECT_TRUE(CanWieldTogether(*MyPlayer, MakeSword(), MakeMace()));
}

TEST_F(MoveItemsTest, CanWieldTogether_TwoHandSwordAndSword_Bard_False)
{
	Become(HeroClass::Bard);

	EXPECT_FALSE(CanWieldTogether(*MyPlayer, MakeTwoHandSword(), MakeSword()));
	EXPECT_FALSE(CanWieldTogether(*MyPlayer, MakeSword(), MakeTwoHandSword()));
}

TEST_F(MoveItemsTest, CanWieldTogether_TwoHandSwordAndShield_ByClass)
{
	EXPECT_FALSE(CanWieldTogether(*MyPlayer, MakeTwoHandSword(), MakeShield()));

	Become(HeroClass::Barbarian);

	EXPECT_TRUE(CanWieldTogether(*MyPlayer, MakeTwoHandSword(), MakeShield()));
	EXPECT_FALSE(CanWieldTogether(*MyPlayer, MakeBow(), MakeShield()));
}

TEST_F(MoveItemsTest, CanWieldTogether_NotAHandItem_False)
{
	EXPECT_FALSE(CanWieldTogether(*MyPlayer, MakeSword(), MakeRing()));
	EXPECT_FALSE(CanWieldTogether(*MyPlayer, MakeRing(), MakeSword()));
}

TEST_F(MoveItemsTest, CanWieldTogether_IgnoresHeldItems)
{
	MyPlayer->InvBody[INVLOC_HAND_LEFT] = MakeStaff();

	EXPECT_TRUE(CanWieldTogether(*MyPlayer, MakeSword(), MakeShield()));
}

TEST_F(MoveItemsTest, CanWieldTogether_IgnoresStats)
{
	Item sword = MakeSword();
	sword._iMinStr = 100;
	sword._iStatFlag = false;
	MyPlayer->_pStrength = 99;

	EXPECT_TRUE(CanWieldTogether(*MyPlayer, sword, MakeShield()));
}

TEST_F(MoveItemsTest, Rejects_TargetsOverlapEachOther)
{
	MyPlayer->InvBody[INVLOC_HAND_LEFT] = MakeSword();
	MyPlayer->InvBody[INVLOC_HAND_RIGHT] = MakeSword();
	const ItemMove moves[] = {
		{ Body(INVLOC_HAND_LEFT), Cell(0) },
		{ Body(INVLOC_HAND_RIGHT), Cell(10) },
	};

	ExpectRejected(moves, MoveItemsError::NoRoom);
}

} // namespace
} // namespace devilution
