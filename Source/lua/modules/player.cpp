#include "lua/modules/player.hpp"

#include <cstdlib>
#include <optional>

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
	LuaSetDocFn(playerType, "setInventoryCellReserved", "(cell: integer, reserved: boolean)",
	    "Reserve an inventory cell: automatic placement won't use it and sorting won't move items touching it. Placing items by hand is unaffected. Lasts until the hero is loaded again. Does nothing given a bad cell, or anything but a boolean.",
	    [](const sol::object &self, const sol::object &cell, const sol::object &reserved) {
		    Player *player = ToPlayer(self);
		    const std::optional<int> index = ToIndex(cell, InventoryGridCells);
		    if (player == nullptr || !index.has_value() || reserved.get_type() != sol::type::boolean)
			    return;
		    player->reservedInvCells.set(*index, reserved.as<bool>());
	    });
	LuaSetDocFn(playerType, "isInventoryCellReserved", "(cell: integer) -> boolean",
	    "Whether an inventory cell is reserved. False if the cell doesn't exist.",
	    [](const sol::object &self, const sol::object &cell) -> bool {
		    const Player *player = ToPlayer(self);
		    const std::optional<int> index = ToIndex(cell, InventoryGridCells);
		    return player != nullptr && index.has_value() && player->reservedInvCells.test(*index);
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
