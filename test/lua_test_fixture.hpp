/**
 * @file lua_test_fixture.hpp
 *
 * Shared test fixture for tests that drive the game through the Lua API.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>

#include <gtest/gtest.h>
#include <sol/sol.hpp>

#include "ui_test.hpp"

#include "diablo.h"
#include "engine/assets.hpp"
#include "inv.h"
#include "items.h"
#include "lua/lua_global.hpp"
#include "options.h"
#include "player.h"
#include "tables/itemdat.h"
#include "utils/paths.h"
#include "utils/str_cat.hpp"

namespace devilution {

/**
 * @brief UITest with a running Lua state and one mod sandbox.
 *
 * Options and mods come from a temporary folder, never from the user's diablo.ini.
 */
class LuaUITest : public UITest {
protected:
	static void SetUpTestSuite()
	{
		UITest::SetUpTestSuite();
		if (!HaveMainData())
			return;

		prefDirectory_ = std::filesystem::temp_directory_path() / StrCat("devilutionx_lua_test_", std::random_device {}());
		std::filesystem::create_directories(prefDirectory_);
		paths::SetPrefPath(prefDirectory_.string());
		paths::SetConfigPath(prefDirectory_.string());

		// These only add to their lists, and a binary can hold several suites.
		static const bool ActionsInitialized = [] {
			InitKeymapActions();
			InitPadmapActions();
			return true;
		}();
		(void)ActionsInitialized;

		LoadOptions();
		LuaInitialize();
		sandbox_ = CreateLuaSandbox();
	}

	static void TearDownTestSuite()
	{
		if (!sandbox_.has_value())
			return;
		sandbox_ = std::nullopt;
		LuaShutdown();
		std::error_code ignored;
		std::filesystem::remove_all(prefDirectory_, ignored);
	}

	void SetUp() override
	{
		UITest::SetUp();
		if (IsSkipped())
			return;

		// CheckInvItem() does nothing while inspecting someone else.
		InspectPlayer = MyPlayer;
	}

	/** @brief Shut Lua down, run `whileDown`, start it again with a new sandbox. Drop all Lua objects first. */
	template <typename Fn>
	static void RestartLua(Fn &&whileDown)
	{
		sandbox_ = std::nullopt;
		LuaShutdown();
		whileDown();
		LuaInitialize();
		sandbox_ = CreateLuaSandbox();
	}

	/** @brief Run a chunk in the sandbox. Use the result before running anything else. */
	static sol::protected_function_result Eval(std::string_view code)
	{
		return GetLuaState().safe_script(code, *sandbox_, sol::script_pass_on_error);
	}

	/** @brief Run a chunk in the sandbox and get its result, which fails the test if the chunk raises an error. */
	template <typename T>
	static std::optional<T> EvalAs(std::string_view code)
	{
		const sol::protected_function_result result = Eval(code);
		if (!result.valid()) {
			ADD_FAILURE() << "Lua error: " << result.get<sol::error>().what() << "\nin: " << code;
			return std::nullopt;
		}
		return result.get<std::optional<T>>();
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

	static void GivePlentyOfStats()
	{
		Player &player = *MyPlayer;
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
				player.InvGrid[topLeftCell + (y * InventorySizeInSlots.width) + x] = isMainCell ? id : static_cast<int8_t>(-id);
			}
		}
	}

private:
	inline static std::filesystem::path prefDirectory_;
	inline static std::optional<sol::environment> sandbox_;
};

} // namespace devilution
