-- Reserved Inventory: keep inventory cells free of what you pick up or buy.
--
-- Enable it in Settings > Mods. The key is in Settings > Keymapping.
--
-- Hold J and click inventory cells. Clicking an item marks all of its cells, clicking again unmarks
-- them. Marked cells are saved with the hero. Pickups, purchases and gold never land in them. You
-- can still put items there by hand, and items you unequip may end up there. Sorting ignores the
-- marks.
local events = require("devilutionx.events")
local herostore = require("devilutionx.herostore")
local input = require("devilutionx.input")
local player = require("devilutionx.player")
local render = require("devilutionx.render")
local logic = require("mods.reserved_inventory.logic")

local KEY = "reserved_inventory.cells"
local COLUMNS = player.InventoryColumns
local ROWS = player.InventoryRows
local CELLS = COLUMNS * ROWS

-- Cells are 29 pixels apart. The grid art draws the line right of a cell 1 pixel past its rectangle
-- from inventoryCellRect, and the line below it on the rectangle's last row.
local PITCH = 29
local LINE_OFFSET_X = -1
local LINE_OFFSET_Y = -2

-- The bare stone in the top left of the panel, 3 by 2 cells, repeated over the marked cells.
local STONE_X, STONE_Y = 8, 11
local STONE_COLUMNS, STONE_ROWS = 3, 2
-- Fills what the 3 pixel wide grid lines leave of a cell, so that the lines stay visible.
local STONE_SIZE = 26
local STONE_SHADE, STONE_SHADE_MARKING = 3, 1

-- The rim replaces the bright middle of the grid line.
local RIM_COLOR = render.PaletteColor.Yellow + 3
local RIM_COLOR_MARKING = render.PaletteColor.Yellow

local marked = {}
local rim = {} -- The runs of logic.outline, updated when the marks change.
local marking = false

local function loadMarks()
  local cells = {}
  for text in (herostore.get(KEY) or ""):gmatch("%d+") do
    local cell = tonumber(text)
    if cell < CELLS then cells[cell] = true end
  end
  return cells
end

local function saveMarks()
  local cells = {}
  for cell in pairs(marked) do cells[#cells + 1] = cell end
  table.sort(cells)
  herostore.set(KEY, #cells > 0 and table.concat(cells, ",") or nil)
end

events.GameStart.add(function()
  marking = false
  marked = loadMarks()
  rim = logic.outline(marked, COLUMNS, ROWS)
end)

input.addAction({
  id = "ReservedInventoryMark",
  name = "Reserve Inventory Cells",
  description = "Hold and click inventory cells or items to keep pickups and purchases out of them.",
  defaultKey = "J",
  onPress = function() marking = true end,
  onRelease = function() marking = false end,
})

events.InventoryCellClicked.add(function(cell)
  if not marking then return false end
  local p = player.self()
  logic.toggle(marked, p:getInventoryItemCells(cell) or { cell })
  rim = logic.outline(marked, COLUMNS, ROWS)
  saveMarks()
  return true
end)

events.InventoryPlacementCheck.add(function(cell, width, height)
  return logic.overlaps(marked, cell, width, height, COLUMNS)
end)

events.InventoryDrawBackground.add(function()
  local shade = marking and STONE_SHADE_MARKING or STONE_SHADE
  for cell in pairs(marked) do
    local x, y = render.inventoryCellRect(cell)
    local srcX = STONE_X + cell % COLUMNS % STONE_COLUMNS * PITCH
    local srcY = STONE_Y + cell // COLUMNS % STONE_ROWS * PITCH
    render.inventoryPanelBlit(srcX, srcY, STONE_SIZE, STONE_SIZE, x + 1, y, shade)
  end
end)

events.InventoryDrawComplete.add(function()
  local originX, originY = render.inventoryCellRect(0)
  local color = marking and RIM_COLOR_MARKING or RIM_COLOR
  for _, run in ipairs(rim) do
    local x = originX + LINE_OFFSET_X + run.x * PITCH
    local y = originY + LINE_OFFSET_Y + run.y * PITCH
    local length = run.length * PITCH + 1
    if run.horizontal then
      render.rect(x, y, length, 1, color)
    else
      render.rect(x, y, 1, length, color)
    end
  end
end)
