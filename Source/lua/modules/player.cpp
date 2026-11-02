#include "lua/modules/player.hpp"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <tuple>
#include <vector>

#include <sol/sol.hpp>

#include "effects.h"
#include "engine/backbuffer_state.hpp"
#include "engine/point.hpp"
#include "engine/random.hpp"
#include "inv.h"
#include "items.h"
#include "lua/metadoc.hpp"
#include "player.h"
#include "sound_effect_enums.h"

namespace devilution {
namespace {

/**
 * @brief Turns a Lua value into an index below count.
 * @return The index, or an empty optional for anything but a whole number in range.
 */
std::optional<int> ToIndex(const sol::object &value, int count)
{
	if (value.get_type() != sol::type::number)
		return std::nullopt;

	lua_State *state = value.lua_state();
	value.push(state);
	int isInteger = 0;
	const lua_Integer index = lua_tointegerx(state, -1, &isInteger);
	lua_pop(state, 1);

	if (isInteger == 0 || index < 0 || index >= count)
		return std::nullopt;
	return static_cast<int>(index);
}

/**
 * @brief The inventory methods take self as a plain value, as a call with '.' instead of ':' puts anything there.
 * @return The player, or nullptr if self is not one.
 */
Player *ToPlayer(const sol::object &self)
{
	return self.is<Player>() ? self.as<Player *>() : nullptr;
}

/**
 * @return The item, or nullptr if the value is not one.
 */
const Item *ToItem(const sol::object &value)
{
	return value.is<Item>() ? value.as<const Item *>() : nullptr;
}

/**
 * @brief Reads `{ body = location }` or `{ cell = cell }`.
 *
 * Fields of moves are read raw here and below, so metamethods can't run code or raise an error in the middle of this.
 */
std::optional<ItemLocation> ToItemLocation(const sol::object &value)
{
	if (value.get_type() != sol::type::table)
		return std::nullopt;

	const auto table = value.as<sol::table>();
	const auto body = table.raw_get<sol::object>("body");
	const auto cell = table.raw_get<sol::object>("cell");
	if (body.valid() && cell.valid())
		return std::nullopt;

	const std::optional<int> index = body.valid() ? ToIndex(body, NUM_INVLOC) : ToIndex(cell, InventoryGridCells);
	if (!index.has_value())
		return std::nullopt;
	return ItemLocation { body.valid() ? ItemLocation::Kind::Body : ItemLocation::Kind::Cell, *index };
}

std::optional<ItemMove> ToItemMove(const sol::object &value)
{
	if (value.get_type() != sol::type::table)
		return std::nullopt;

	const auto table = value.as<sol::table>();
	const std::optional<ItemLocation> from = ToItemLocation(table.raw_get<sol::object>("from"));
	const std::optional<ItemLocation> to = ToItemLocation(table.raw_get<sol::object>("to"));
	if (!from.has_value() || !to.has_value())
		return std::nullopt;
	return ItemMove { *from, *to };
}

/**
 * @brief Reads a list of moves. Every entry of the table has to be part of the list.
 *
 * The table is walked rather than read up to its length, as a gap would otherwise drop the moves after it, and only some of the moves would happen.
 */
std::optional<std::vector<ItemMove>> ToItemMoves(const sol::object &value)
{
	if (value.get_type() != sol::type::table)
		return std::nullopt;

	std::vector<ItemMove> moves(MaxItemMoves);
	bool valid = true;
	int count = 0;
	int lastIndex = 0;
	value.as<sol::table>().for_each([&](const sol::object &key, const sol::object &entry) {
		count++;
		const std::optional<int> index = ToIndex(key, static_cast<int>(MaxItemMoves) + 1);
		const std::optional<ItemMove> move = ToItemMove(entry);
		if (!index.has_value() || *index == 0 || !move.has_value()) {
			valid = false;
			return;
		}
		moves[*index - 1] = *move;
		lastIndex = std::max(lastIndex, *index);
	});

	if (!valid || lastIndex != count)
		return std::nullopt;
	moves.resize(count);
	return moves;
}

using MoveItemsResult = std::tuple<bool, std::optional<std::string_view>>;

MoveItemsResult ToMoveItemsResult(std::optional<MoveItemsError> error)
{
	if (!error.has_value())
		return { true, std::nullopt };

	switch (*error) {
	case MoveItemsError::Busy:
		return { false, "busy" };
	case MoveItemsError::Missing:
		return { false, "missing" };
	case MoveItemsError::NoRoom:
		return { false, "noroom" };
	case MoveItemsError::CannotEquip:
		return { false, "cannotequip" };
	case MoveItemsError::Invalid:
		break;
	}
	return { false, "invalid" };
}

void InitPlayerUserType(sol::state_view &lua)
{
	sol::usertype<Player> playerType = lua.new_usertype<Player>(sol::no_constructor);
	LuaSetDocReadonlyProperty(playerType, "name", "string",
	    "Player's name (readonly)",
	    &Player::name);
	LuaSetDocReadonlyProperty(playerType, "id", "integer",
	    "Player's unique ID (readonly)",
	    [](const Player &player) {
		    return static_cast<int>(reinterpret_cast<uintptr_t>(&player));
	    });
	LuaSetDocReadonlyProperty(playerType, "position", "Point",
	    "Player's current position (readonly)",
	    [](const Player &player) -> Point {
		    return Point { player.position.tile };
	    });
	LuaSetDocFn(playerType, "addExperience", "(experience: integer, monsterLevel: integer = nil)",
	    "Adds experience to this player based on the current game mode",
	    [](Player &player, uint32_t experience, std::optional<int> monsterLevel) {
		    if (monsterLevel.has_value()) {
			    player.addExperience(experience, *monsterLevel);
		    } else {
			    player.addExperience(experience);
		    }
	    });
	LuaSetDocProperty(playerType, "characterLevel", "number",
	    "Character level (writeable)",
	    &Player::getCharacterLevel, &Player::setCharacterLevel);
	LuaSetDocFn(playerType, "addItem", "(itemId: integer, count: integer = 1)",
	    "Add an item to the player's inventory",
	    [](Player &player, int itemId, std::optional<int> count) -> bool {
		    const auto itemIndex = static_cast<_item_indexes>(itemId);
		    const int itemCount = count.value_or(1);
		    for (int i = 0; i < itemCount; i++) {
			    Item tempItem {};
			    SetupAllItems(player, tempItem, itemIndex, AdvanceRndSeed(), 1, 1, true, false);
			    if (!AutoPlaceItemInInventory(player, tempItem, true)) {
				    return false;
			    }
		    }
		    CalcPlrInv(player, true);
		    return true;
	    });
	LuaSetDocFn(playerType, "hasItem", "(itemId: integer)",
	    "Check if the player has an item with the given ID",
	    [](const Player &player, int itemId) -> bool {
		    return HasInventoryOrBeltItemWithId(player, static_cast<_item_indexes>(itemId));
	    });
	LuaSetDocFn(playerType, "removeItem", "(itemId: integer, count: integer = 1)",
	    "Remove an item from the player's inventory",
	    [](Player &player, int itemId, std::optional<int> count) -> int {
		    const auto targetId = static_cast<_item_indexes>(itemId);
		    const int itemCount = count.value_or(1);
		    int removed = 0;

		    // Remove from inventory
		    for (int i = player._pNumInv - 1; i >= 0 && removed < itemCount; i--) {
			    if (player.InvList[i].IDidx == targetId) {
				    player.RemoveInvItem(i);
				    removed++;
			    }
		    }

		    // Remove from belt if needed
		    for (int i = MaxBeltItems - 1; i >= 0 && removed < itemCount; i--) {
			    if (!player.SpdList[i].isEmpty() && player.SpdList[i].IDidx == targetId) {
				    player.RemoveSpdBarItem(i);
				    removed++;
			    }
		    }

		    if (removed > 0) {
			    CalcPlrInv(player, true);
		    }

		    return removed;
	    });
	LuaSetDocFn(playerType, "restoreFullLife", "()",
	    "Restore player's HP to maximum",
	    [](Player &player) {
		    player._pHitPoints = player._pMaxHP;
		    player._pHPBase = player._pMaxHPBase;
	    });
	LuaSetDocFn(playerType, "restoreFullMana", "()",
	    "Restore player's mana to maximum",
	    [](Player &player) {
		    player._pMana = player._pMaxMana;
		    player._pManaBase = player._pMaxManaBase;
	    });
	LuaSetDocReadonlyProperty(playerType, "mana", "number",
	    "Current mana (readonly)",
	    [](Player &player) { return player._pMana.whole(); });
	LuaSetDocReadonlyProperty(playerType, "maxMana", "number",
	    "Maximum mana (readonly)",
	    [](Player &player) { return player._pMaxMana.whole(); });
	LuaSetDocFn(playerType, "getBodyItem", "(location: InvBodyLoc) -> Item|nil",
	    "The item in a body slot, or nil if the slot is empty or doesn't exist. A live reference: invalid after any inventory change.",
	    [](const sol::object &self, const sol::object &location) -> Item * {
		    Player *player = ToPlayer(self);
		    const std::optional<int> index = ToIndex(location, NUM_INVLOC);
		    if (player == nullptr || !index.has_value() || player->InvBody[*index].isEmpty())
			    return nullptr;
		    return &player->InvBody[*index];
	    });
	LuaSetDocFn(playerType, "getInventoryItemAt", "(cell: integer) -> Item|nil",
	    "The item covering an inventory cell (`row * InventoryColumns + column`, from 0), or nil if there is none or the cell doesn't exist. A live reference: invalid after any inventory change.",
	    [](const sol::object &self, const sol::object &cell) -> Item * {
		    Player *player = ToPlayer(self);
		    const std::optional<int> index = ToIndex(cell, InventoryGridCells);
		    if (player == nullptr || !index.has_value() || player->InvGrid[*index] == 0)
			    return nullptr;
		    return &player->InvList[std::abs(player->InvGrid[*index]) - 1];
	    });
	LuaSetDocFn(playerType, "getInventoryItemCells", "(cell: integer) -> integer[]|nil",
	    "All cells of the item covering a cell, ascending, so the first is its top-left cell. Nil if there is no item or the cell doesn't exist.",
	    [](const sol::object &self, const sol::object &cell, sol::this_state state) -> std::optional<sol::table> {
		    const Player *player = ToPlayer(self);
		    const std::optional<int> index = ToIndex(cell, InventoryGridCells);
		    if (player == nullptr || !index.has_value() || player->InvGrid[*index] == 0)
			    return std::nullopt;

		    sol::table cells = sol::state_view(state).create_table();
		    const int itemId = std::abs(player->InvGrid[*index]);
		    int count = 0;
		    for (int i = 0; i < InventoryGridCells; i++) {
			    if (std::abs(player->InvGrid[i]) == itemId) {
				    count++;
				    cells[count] = i;
			    }
		    }
		    return cells;
	    });
	LuaSetDocFn(playerType, "getItemLocation", "(item: Item) -> ItemEquipType|nil",
	    "Where this player wears an item. Differs from `item.loc` for classes that hold some two-handed weapons in one hand.",
	    [](const sol::object &self, const sol::object &item) -> std::optional<item_equip_type> {
		    const Player *player = ToPlayer(self);
		    const Item *itemPointer = ToItem(item);
		    if (player == nullptr || itemPointer == nullptr)
			    return std::nullopt;
		    return player->GetItemLocation(*itemPointer);
	    });
	LuaSetDocFn(playerType, "canUseItem", "(item: Item) -> boolean",
	    "Whether this player meets the item's requirements",
	    [](const sol::object &self, const sol::object &item) -> bool {
		    const Player *player = ToPlayer(self);
		    const Item *itemPointer = ToItem(item);
		    return player != nullptr && itemPointer != nullptr && player->CanUseItem(*itemPointer);
	    });
	LuaSetDocFn(playerType, "canWieldTogether", "(a: Item, b: Item) -> boolean",
	    "Whether this player's class can hold both items at once. Ignores what is in hand and the items' requirements.",
	    [](const sol::object &self, const sol::object &a, const sol::object &b) -> bool {
		    const Player *player = ToPlayer(self);
		    const Item *itemA = ToItem(a);
		    const Item *itemB = ToItem(b);
		    return player != nullptr && itemA != nullptr && itemB != nullptr && CanWieldTogether(*player, *itemA, *itemB);
	    });
	LuaSetDocFn(playerType, "canMoveItems", "(moves: { from: { body: InvBodyLoc }|{ cell: integer }, to: { body: InvBodyLoc }|{ cell: integer } }[]) -> ok: boolean, reason: string|nil",
	    "Whether moveItems would succeed with these moves, changing nothing. Reason is as in moveItems.",
	    [](const sol::object &self, const sol::object &moves) -> MoveItemsResult {
		    const Player *player = ToPlayer(self);
		    const std::optional<std::vector<ItemMove>> itemMoves = ToItemMoves(moves);
		    if (player == nullptr || !itemMoves.has_value())
			    return ToMoveItemsResult(MoveItemsError::Invalid);
		    return ToMoveItemsResult(CheckMoveItems(*player, *itemMoves));
	    });
	LuaSetDocFn(playerType, "moveItems", "(moves: { from: { body: InvBodyLoc }|{ cell: integer }, to: { body: InvBodyLoc }|{ cell: integer } }[]) -> ok: boolean, reason: string|nil",
	    "Move up to 16 items between body slots and inventory cells, all or none, seen by other players too. "
	    "A `from` cell is any cell of the item, a `to` cell its new top-left cell, and a target may overlap the source of another move. "
	    "Reason on failure: \"busy\" (not the local player, no game running, a store is open, or a body slot is involved while attacking or casting), \"invalid\" (a malformed list), "
	    "\"missing\" (a source is empty), \"noroom\" (a target is taken or out of bounds) or \"cannotequip\". Item references are invalid afterwards.",
	    [](const sol::object &self, const sol::object &moves) -> MoveItemsResult {
		    Player *player = ToPlayer(self);
		    const std::optional<std::vector<ItemMove>> itemMoves = ToItemMoves(moves);
		    if (player == nullptr || !itemMoves.has_value())
			    return ToMoveItemsResult(MoveItemsError::Invalid);
		    return ToMoveItemsResult(MoveItems(*player, *itemMoves));
	    });
	LuaSetDocFn(playerType, "say", "(speech: HeroSpeech)",
	    "Speak a line, unless it is already being spoken. Does nothing given anything but a HeroSpeech.",
	    [](const sol::object &self, const sol::object &speech) {
		    const Player *player = ToPlayer(self);
		    const std::optional<int> index = ToIndex(speech, static_cast<int>(HeroSpeech::LAST) + 1);
		    if (player != nullptr && index.has_value())
			    player->SaySpecific(static_cast<HeroSpeech>(*index));
	    });
}
} // namespace

sol::table LuaPlayerModule(sol::state_view &lua)
{
	InitPlayerUserType(lua);
	sol::table table = lua.create_table();
	LuaSetDocFn(table, "self", "()",
	    "The current player",
	    []() {
		    return MyPlayer;
	    });
	LuaSetDocFn(table, "walk_to", "(x: integer, y: integer)",
	    "Walk to the given coordinates",
	    [](int x, int y) {
		    NetSendCmdLoc(MyPlayerId, true, CMD_WALKXY, Point { x, y });
	    });
	LuaSetDoc(table, "InventoryColumns", "integer", "Width of the inventory grid in cells", InventorySizeInSlots.width);
	LuaSetDoc(table, "InventoryRows", "integer", "Height of the inventory grid in cells", InventorySizeInSlots.height);

	return table;
}

} // namespace devilution
