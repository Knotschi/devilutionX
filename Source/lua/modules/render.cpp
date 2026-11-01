#include "lua/modules/render.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <tuple>

#include <sol/sol.hpp>

#include "DiabloUI/ui_flags.hpp"
#include "control/control.hpp"
#include "engine/dx.h"
#include "engine/light_tables.hpp"
#include "engine/palette.h"
#include "engine/render/primitive_render.hpp"
#include "engine/render/text_render.hpp"
#include "inv.h"
#include "lua/metadoc.hpp"
#include "utils/display.h"

namespace devilution {

namespace {

/**
 * @return The value, or an empty optional for anything but a whole number that fits an int.
 */
std::optional<int> ToInt(const sol::object &value)
{
	if (value.get_type() != sol::type::number)
		return std::nullopt;

	lua_State *state = value.lua_state();
	value.push(state);
	int isInteger = 0;
	const lua_Integer number = lua_tointegerx(state, -1, &isInteger);
	lua_pop(state, 1);

	if (isInteger == 0 || number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max())
		return std::nullopt;
	return static_cast<int>(number);
}

/**
 * Clips here, not in the line functions: `x + width` may not fit an int, and DrawVerticalLine runs past the bottom of the buffer.
 */
void DrawRectOutline(const Surface &out, int x, int y, int width, int height, uint8_t color)
{
	if (width <= 0 || height <= 0)
		return;

	const int64_t right = static_cast<int64_t>(x) + width - 1;
	const int64_t bottom = static_cast<int64_t>(y) + height - 1;
	const int visibleLeft = std::max(x, 0);
	const int visibleTop = std::max(y, 0);
	const int visibleRight = static_cast<int>(std::min<int64_t>(right, out.w() - 1));
	const int visibleBottom = static_cast<int>(std::min<int64_t>(bottom, out.h() - 1));
	if (visibleLeft > visibleRight || visibleTop > visibleBottom)
		return;

	const int visibleWidth = visibleRight - visibleLeft + 1;
	const int visibleHeight = visibleBottom - visibleTop + 1;
	if (y >= 0)
		UnsafeDrawHorizontalLine(out, { visibleLeft, y }, visibleWidth, color);
	if (bottom < out.h())
		UnsafeDrawHorizontalLine(out, { visibleLeft, static_cast<int>(bottom) }, visibleWidth, color);
	if (x >= 0)
		UnsafeDrawVerticalLine(out, { x, visibleTop }, visibleHeight, color);
	if (right < out.w())
		UnsafeDrawVerticalLine(out, { static_cast<int>(right), visibleTop }, visibleHeight, color);
}

} // namespace

sol::table LuaRenderModule(sol::state_view &lua)
{
	sol::table table = lua.create_table();
	LuaSetDocFn(table, "string", "(text: string, x: integer, y: integer)",
	    "Renders a string at the given coordinates",
	    [](std::string_view text, int x, int y) { DrawString(GlobalBackBuffer(), text, { x, y }); });
	LuaSetDocFn(table, "rect", "(x: integer, y: integer, width: integer, height: integer, color: integer)",
	    "Draws a 1 pixel wide outline, clipped to the screen. color is a palette index from 0 to 255, see PaletteColor. Draws nothing if an argument is bad.",
	    [](const sol::object &x, const sol::object &y, const sol::object &width, const sol::object &height, const sol::object &color) {
		    const std::optional<int> left = ToInt(x);
		    const std::optional<int> top = ToInt(y);
		    const std::optional<int> outerWidth = ToInt(width);
		    const std::optional<int> outerHeight = ToInt(height);
		    const std::optional<int> paletteIndex = ToInt(color);
		    if (PalSurface == nullptr || !left || !top || !outerWidth || !outerHeight || !paletteIndex || *paletteIndex < 0 || *paletteIndex > 255)
			    return;
		    DrawRectOutline(GlobalBackBuffer(), *left, *top, *outerWidth, *outerHeight, static_cast<uint8_t>(*paletteIndex));
	    });
	LuaSetDocFn(table, "inventoryPanelBlit", "(srcX: integer, srcY: integer, width: integer, height: integer, x: integer, y: integer, shade: integer)",
	    "Copies a region of the inventory panel art to the screen at x, y. srcX and srcY count from the top left of the art. shade darkens it, from 0 (unchanged) to 15 (black). Clipped to the screen and the art. Draws nothing if an argument is bad. To end up below the items, call it from InventoryDrawBackground.",
	    [](const sol::object &srcX, const sol::object &srcY, const sol::object &width, const sol::object &height, const sol::object &x, const sol::object &y, const sol::object &shade) {
		    const std::optional<int> left = ToInt(srcX);
		    const std::optional<int> top = ToInt(srcY);
		    const std::optional<int> regionWidth = ToInt(width);
		    const std::optional<int> regionHeight = ToInt(height);
		    const std::optional<int> targetX = ToInt(x);
		    const std::optional<int> targetY = ToInt(y);
		    const std::optional<int> darkness = ToInt(shade);
		    if (PalSurface == nullptr || !left || !top || !regionWidth || !regionHeight || !targetX || !targetY || !darkness
		        || *regionWidth <= 0 || *regionHeight <= 0 || *darkness < 0 || *darkness >= static_cast<int>(NumLightingLevels))
			    return;
		    DrawInventoryPanelRegion(GlobalBackBuffer(), { { *left, *top }, { *regionWidth, *regionHeight } }, { *targetX, *targetY }, *darkness);
	    });
	LuaSetDocFn(table, "inventoryCellRect", "(cell: integer) -> x: integer, y: integer, width: integer, height: integer",
	    "Returns the rectangle on the screen a cell of the inventory grid is drawn in. Cells are 0-based, row by row. Returns nil if there is no such cell.",
	    [](const sol::object &cell) -> std::optional<std::tuple<int, int, int, int>> {
		    const std::optional<int> index = ToInt(cell);
		    if (!index || *index < 0 || *index >= InventoryGridCells)
			    return std::nullopt;
		    // InvRect is the area that takes clicks. The cell itself starts 1 pixel lower and is 1 pixel smaller.
		    const Point position = GetPanelPosition(UiPanels::Inventory, InvRect[*index + SLOTXY_INV_FIRST].position);
		    return std::make_tuple(position.x, position.y + 1, InventorySlotSizeInPixels.width, InventorySlotSizeInPixels.height);
	    });
	LuaSetDocFn(table, "screen_width", "()",
	    "Returns the screen width", []() { return gnScreenWidth; });
	LuaSetDocFn(table, "screen_height", "()",
	    "Returns the screen height", []() { return gnScreenHeight; });

	auto uiFlags = lua.create_table();
	uiFlags["None"] = UiFlags::None;

	uiFlags["FontSize12"] = UiFlags::FontSize12;
	uiFlags["FontSize24"] = UiFlags::FontSize24;
	uiFlags["FontSize30"] = UiFlags::FontSize30;
	uiFlags["FontSize42"] = UiFlags::FontSize42;
	uiFlags["FontSize46"] = UiFlags::FontSize46;
	uiFlags["FontSizeDialog"] = UiFlags::FontSizeDialog;

	uiFlags["ColorUiGold"] = UiFlags::ColorUiGold;
	uiFlags["ColorUiSilver"] = UiFlags::ColorUiSilver;
	uiFlags["ColorUiGoldDark"] = UiFlags::ColorUiGoldDark;
	uiFlags["ColorUiSilverDark"] = UiFlags::ColorUiSilverDark;
	uiFlags["ColorDialogWhite"] = UiFlags::ColorDialogWhite;
	uiFlags["ColorDialogYellow"] = UiFlags::ColorDialogYellow;
	uiFlags["ColorDialogRed"] = UiFlags::ColorDialogRed;
	uiFlags["ColorYellow"] = UiFlags::ColorYellow;
	uiFlags["ColorGold"] = UiFlags::ColorGold;
	uiFlags["ColorBlack"] = UiFlags::ColorBlack;
	uiFlags["ColorWhite"] = UiFlags::ColorWhite;
	uiFlags["ColorWhitegold"] = UiFlags::ColorWhitegold;
	uiFlags["ColorRed"] = UiFlags::ColorRed;
	uiFlags["ColorBlue"] = UiFlags::ColorBlue;
	uiFlags["ColorOrange"] = UiFlags::ColorOrange;
	uiFlags["ColorButtonface"] = UiFlags::ColorButtonface;
	uiFlags["ColorButtonpushed"] = UiFlags::ColorButtonpushed;

	uiFlags["AlignCenter"] = UiFlags::AlignCenter;
	uiFlags["AlignRight"] = UiFlags::AlignRight;
	uiFlags["VerticalCenter"] = UiFlags::VerticalCenter;

	uiFlags["KerningFitSpacing"] = UiFlags::KerningFitSpacing;

	uiFlags["ElementDisabled"] = UiFlags::ElementDisabled;
	uiFlags["ElementHidden"] = UiFlags::ElementHidden;

	uiFlags["PentaCursor"] = UiFlags::PentaCursor;
	uiFlags["Outlined"] = UiFlags::Outlined;

	uiFlags["NeedsNextElement"] = UiFlags::NeedsNextElement;

	table["UiFlags"] = uiFlags;

	auto paletteColor = lua.create_table();
	paletteColor["Beige"] = PAL16_BEIGE;
	paletteColor["Blue"] = PAL16_BLUE;
	paletteColor["Yellow"] = PAL16_YELLOW;
	paletteColor["Orange"] = PAL16_ORANGE;
	paletteColor["Red"] = PAL16_RED;
	paletteColor["Gray"] = PAL16_GRAY;
	LuaSetDoc(table, "PaletteColor", "table",
	    "Palette index of the lightest shade of each color, for rect. Add 0 to 15 for darker shades.", paletteColor);

	return table;
}

} // namespace devilution
