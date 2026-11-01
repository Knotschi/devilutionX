#pragma once

#include <sol/sol.hpp>

namespace devilution {

sol::table LuaInputModule(sol::state_view &lua);
/** @brief Removes all actions added from Lua. Their callbacks must not outlive the Lua state. */
void ClearLuaActions();

} // namespace devilution
