-- Weapon Swap: swap the items in your hands with items stored in marked inventory cells.
--
-- Enable it in Settings > Mods. Keys are in Settings > Keymapping (gamepad buttons in Settings >
-- Padmapping, unbound by default).
--
-- Mark cells: hold H and click inventory cells. Clicking an item marks all of its cells, clicking
-- again unmarks them. Marked cells get a stone background and a golden rim around the whole area
-- (brighter while H is held) and are saved with the hero. Pickups never land in them, but you can still put items there by hand.
--
-- Swap: press W. Usable weapons and shields lying entirely in marked cells go to your hands. Only
-- what they replace comes out, so a lone sword in the area leaves your shield in place. What comes
-- out must fit in marked cells that are free after the swap: three marked cells holding a sword
-- can't take a 2x3 staff from your hands, six can.
--
-- A swap that can't be done changes nothing and the hero says so. Anything else in the marked
-- cells takes up room. The item on the cursor is left alone.
local audio = require("devilutionx.audio")
local events = require("devilutionx.events")
local herostore = require("devilutionx.herostore")
local input = require("devilutionx.input")
local items = require("devilutionx.items")
local player = require("devilutionx.player")
local render = require("devilutionx.render")
local logic = require("mods.weapon_swap.logic")

local KEY = "weapon_swap.cells"
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
local swapHeld = false -- The key auto-repeats, one physical press is one swap.

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

local function syncReserved(p)
  for cell = 0, CELLS - 1 do
    p:setInventoryCellReserved(cell, marked[cell] == true)
  end
end

local function handOf(p, item)
  local location = p:getItemLocation(item)
  if location == items.ItemEquipType.OneHand then return "one" end
  if location == items.ItemEquipType.TwoHand then return "two" end
  return nil
end

local function isShield(item)
  return item.type == items.ItemType.Shield
end

local function heldRecord(p, item)
  if item == nil then return nil end
  return { w = item.invWidth, h = item.invHeight, hand = handOf(p, item), shield = isShield(item), ref = item }
end

local function buildView(p)
  local records = {}
  for cell = 0, CELLS - 1 do
    local cells = p:getInventoryItemCells(cell)
    if cells ~= nil and cells[1] == cell then
      local item = p:getInventoryItemAt(cell)
      records[#records + 1] = {
        origin = cell,
        cells = cells,
        w = item.invWidth,
        h = item.invHeight,
        hand = p:canUseItem(item) and handOf(p, item) or nil,
        shield = isShield(item),
        ref = item,
      }
    end
  end
  return {
    columns = player.InventoryColumns,
    rows = player.InventoryRows,
    marked = marked,
    items = records,
    held = {
      left = heldRecord(p, p:getBodyItem(items.InvBodyLoc.HandLeft)),
      right = heldRecord(p, p:getBodyItem(items.InvBodyLoc.HandRight)),
    },
    compatible = function(a, b) return p:canWieldTogether(a.ref, b.ref) end,
  }
end

-- The logic names hands with strings, moveItems wants body locations.
local function toMoveTarget(place)
  if place.body == "left" then return { body = items.InvBodyLoc.HandLeft } end
  if place.body == "right" then return { body = items.InvBodyLoc.HandRight } end
  return place
end

local function swap()
  local p = player.self()
  local plan = logic.planSwap(buildView(p))
  if plan == nil then
    p:say(audio.HeroSpeech.ICantDoThat)
    return
  end
  local moves = {}
  for i, move in ipairs(plan.moves) do
    moves[i] = { from = toMoveTarget(move.from), to = toMoveTarget(move.to) }
  end
  local sfx = plan.soundItem.ref.invSfx -- Item references go stale in the move.
  if p:moveItems(moves) then
    audio.playSfx(sfx)
  else
    p:say(audio.HeroSpeech.ICantDoThat)
  end
end

events.GameStart.add(function()
  marking = false
  swapHeld = false
  marked = loadMarks()
  rim = logic.outline(marked, COLUMNS, ROWS)
  syncReserved(player.self())
end)

input.addAction({
  id = "WeaponSwap",
  name = "Swap Weapons",
  description = "Swap the items in your hands with the ones in your marked inventory cells.",
  defaultKey = "W",
  onPress = function()
    if swapHeld then return end
    swapHeld = true
    swap()
  end,
  onRelease = function() swapHeld = false end,
})

input.addAction({
  id = "WeaponSwapMark",
  name = "Mark Weapon Swap Cells",
  description = "Hold and click inventory cells or items to mark them for weapon swap.",
  defaultKey = "H",
  onPress = function() marking = true end,
  onRelease = function() marking = false end,
})

events.InventoryCellClicked.add(function(cell)
  if not marking then return false end
  local p = player.self()
  logic.toggle(marked, p:getInventoryItemCells(cell) or { cell })
  rim = logic.outline(marked, COLUMNS, ROWS)
  syncReserved(p)
  saveMarks()
  return true
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
