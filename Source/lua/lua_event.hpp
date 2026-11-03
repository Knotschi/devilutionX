#pragma once

#include <cstdint>
#include <string_view>

namespace devilution {

struct Player;
struct Monster;

namespace lua {

void MonsterDataLoaded();
void UniqueMonsterDataLoaded();
void ItemDataLoaded();
void UniqueItemDataLoaded();

void StoreOpened(std::string_view name);

void OnMonsterTakeDamage(const Monster *monster, int damage, int damageType);

void OnPlayerGainExperience(const Player *player, uint32_t exp);
void OnPlayerTakeDamage(const Player *player, int damage, int damageType);

void LoadModsComplete();
void GameDrawComplete();
void GameStart();
/**
 * @param cell The cell of the inventory grid.
 * @return Whether a handler took the click, so the game has to ignore it.
 */
bool InventoryCellClicked(int cell);
/** @brief Offers the top-left cell the game picked for an item being acquired. Returns whether a mod refused it. */
bool InventoryPlacementRefused(int cell, int width, int height);
void InventoryDrawBackground();
void InventoryDrawComplete();

} // namespace lua

} // namespace devilution
