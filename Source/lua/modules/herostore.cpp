#include "lua/modules/herostore.hpp"

#include <optional>
#include <string>
#include <string_view>

#include <sol/sol.hpp>

#include "hero_store.hpp"
#include "lua/metadoc.hpp"

namespace devilution {

namespace {

[[nodiscard]] bool IsString(const sol::object &object)
{
	return object.get_type() == sol::type::string;
}

std::optional<std::string> LuaGet(const sol::object &key)
{
	if (!IsString(key))
		return std::nullopt;
	return HeroStoreGet(key.as<std::string_view>());
}

bool LuaSet(const sol::object &key, const sol::object &value)
{
	if (!IsString(key))
		return false;
	if (value == sol::lua_nil)
		return HeroStoreSet(key.as<std::string_view>(), std::nullopt);
	if (!IsString(value))
		return false;
	return HeroStoreSet(key.as<std::string_view>(), value.as<std::string_view>());
}

} // namespace

sol::table LuaHeroStoreModule(sol::state_view &lua)
{
	sol::table table = lua.create_table();
	LuaSetDocFn(table, "get", "(key: string) -> string|nil",
	    "Returns the value the current hero keeps under `key`, or nil if there is none or `key` is not a string.",
	    LuaGet);
	LuaSetDocFn(table, "set", "(key: string, value: string|nil) -> boolean",
	    "Keeps `value` under `key` in the current hero's save, or deletes `key` if `value` is nil. Name keys \"<mod>.<key>\".\n"
	    "Returns false and changes nothing if an argument is not a string, `key` is empty or over 64 bytes, `value` is over 4096 bytes, or the hero already has 256 keys.",
	    LuaSet);
	return table;
}

} // namespace devilution
