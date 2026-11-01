#include "lua/modules/input.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sol/sol.hpp>

#include "controls/controller_buttons.h"
#include "controls/padmapper.hpp"
#include "diablo.h"
#include "lua/lua_global.hpp"
#include "lua/metadoc.hpp"
#include "options.h"
#include "utils/str_cat.hpp"

namespace devilution {

namespace {

/** @brief The actions added from Lua, each one in both mappers. */
std::vector<std::string> LuaActionIds;
/** @brief The number of action callbacks on the call stack. */
int RunningCallbacks = 0;

/** @brief The id is a key in diablo.ini, where most other characters mean something. */
bool IsValidActionId(std::string_view id)
{
	return !id.empty() && std::all_of(id.begin(), id.end(), [](char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
	});
}

bool IsString(const sol::object &value)
{
	return value.get_type() == sol::type::string;
}

bool IsOptionalFunction(const sol::object &value)
{
	return value.get_type() == sol::type::function || value.get_type() == sol::type::lua_nil;
}

void RunCallback(const sol::main_protected_function &callback)
{
	RunningCallbacks++;
	SafeCallResult(callback(), /*optional=*/true);
	RunningCallbacks--;
}

std::function<void()> MakePressCallback(const sol::object &function)
{
	if (function.get_type() != sol::type::function)
		return nullptr;
	return [callback = function.as<sol::main_protected_function>()] {
		if (CanPlayerTakeAction())
			RunCallback(callback);
	};
}

/** @brief Not gated like the press, or a mod that tracks a hold would never see its end. */
std::function<void()> MakeReleaseCallback(const sol::object &function)
{
	if (function.get_type() != sol::type::function)
		return nullptr;
	return [callback = function.as<sol::main_protected_function>()] {
		RunCallback(callback);
	};
}

void RemoveLuaAction(const std::string &id)
{
	GetOptions().Keymapper.RemoveRuntimeAction(id);
	PadmapperRemoveRuntimeAction(id);
}

/** @return The reason this call cannot add the action, or nothing if it did. */
std::optional<std::string> AddAction(lua_State *state)
{
	// Replacing the action would destroy the callback while it runs.
	if (RunningCallbacks > 0)
		return "cannot be used inside an action callback";
	if (lua_type(state, 1) != LUA_TTABLE)
		return "the argument must be a table";

	const sol::table spec(state, 1);
	const sol::object idValue = spec.raw_get<sol::object>("id");
	const sol::object nameValue = spec.raw_get<sol::object>("name");
	const sol::object descriptionValue = spec.raw_get<sol::object>("description");
	const sol::object defaultKeyValue = spec.raw_get<sol::object>("defaultKey");
	const sol::object onPress = spec.raw_get<sol::object>("onPress");
	const sol::object onRelease = spec.raw_get<sol::object>("onRelease");

	if (!IsString(idValue) || !IsValidActionId(idValue.as<std::string_view>()))
		return "id must be a string of letters, digits, '_', '.' and '-'";
	if (!IsString(nameValue) || nameValue.as<std::string_view>().empty())
		return "name must be a non-empty string";
	if (!IsString(descriptionValue))
		return "description must be a string";
	if (!IsOptionalFunction(onPress))
		return "onPress must be a function";
	if (!IsOptionalFunction(onRelease))
		return "onRelease must be a function";

	KeymapperOptions &keymapper = GetOptions().Keymapper;
	PadmapperOptions &padmapper = GetOptions().Padmapper;

	uint32_t defaultKey = SDLK_UNKNOWN;
	if (defaultKeyValue.get_type() != sol::type::lua_nil) {
		if (!IsString(defaultKeyValue))
			return "defaultKey must be a string";
		const std::optional<uint32_t> key = keymapper.KeyFromName(defaultKeyValue.as<std::string_view>());
		if (!key.has_value())
			return StrCat("unknown defaultKey \"", defaultKeyValue.as<std::string_view>(), "\"");
		defaultKey = *key;
	}

	const std::string id = idValue.as<std::string>();
	const bool isReplacement = std::find(LuaActionIds.begin(), LuaActionIds.end(), id) != LuaActionIds.end();
	if (!isReplacement && (keymapper.HasAction(id) || padmapper.HasAction(id)))
		return StrCat("id \"", id, "\" belongs to another action");

	if (isReplacement)
		RemoveLuaAction(id);
	else
		LuaActionIds.push_back(id);

	const std::string_view name = nameValue.as<std::string_view>();
	const std::string_view description = descriptionValue.as<std::string_view>();
	keymapper.AddRuntimeAction(id, name, description, defaultKey, MakePressCallback(onPress), MakeReleaseCallback(onRelease));
	padmapper.AddRuntimeAction(id, name, description, ControllerButton_NONE, MakePressCallback(onPress), MakeReleaseCallback(onRelease));
	return std::nullopt;
}

/** @return Whether adding the action failed, with the message for lua_error() on the Lua stack. */
bool PushAddActionError(lua_State *state)
{
	const std::optional<std::string> error = AddAction(state);
	if (!error.has_value())
		return false;
	const std::string message = StrCat("addAction: ", *error);
	luaL_where(state, 1);
	lua_pushlstring(state, message.data(), message.size());
	lua_concat(state, 2);
	return true;
}

/** @brief lua_error() does not unwind C++ frames, so all of them must be gone when it runs. */
int LuaAddAction(lua_State *state)
{
	if (PushAddActionError(state))
		return lua_error(state);
	return 0;
}

} // namespace

sol::table LuaInputModule(sol::state_view &lua)
{
	sol::table table = lua.create_table();
	LuaSetDocFn(table, "addAction",
	    "(spec: { id: string, name: string, description: string, defaultKey: string|nil, onPress: function|nil, onRelease: function|nil })",
	    "Adds a key and a gamepad action, shown in the settings, with their bindings kept in diablo.ini.\n"
	    "`id` may hold letters, digits, '_', '.' and '-', for example \"mymod.action\". The same id again replaces the action and keeps its binding. An id used by anything else is an error.\n"
	    "`defaultKey` is a key name as in diablo.ini, like \"W\", \"F9\" or \"LALT\". If another action has it, this one starts unbound. The gamepad starts unbound.\n"
	    "`onPress` runs only if the player can act, and again on keyboard auto-repeat while a key is held. `onRelease` is not gated like `onPress`, so expect a release without a press. A release during a level load runs after the load, at the next input. A release while no game runs is lost: clear held state on `GameStart`.\n"
	    "Raises an error for a bad spec, and when called inside `onPress` or `onRelease`.",
	    LuaAddAction);
	return table;
}

void ClearLuaActions()
{
	for (const std::string &id : LuaActionIds)
		RemoveLuaAction(id);
	LuaActionIds.clear();
}

} // namespace devilution
