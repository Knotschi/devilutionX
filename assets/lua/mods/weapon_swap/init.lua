-- Weapon Swap: swap the items in your hands with items you picked in the inventory.
--
-- Enable it in Settings > Mods. Keys are in Settings > Keymapping (gamepad buttons in Settings >
-- Padmapping, unbound by default).
--
-- Pick items: hold H and click a weapon or shield in the inventory. It gets a red dot. A second pick
-- joins it if the two can be held together, otherwise it replaces it: picking always works, it just
-- may unpick what was there. Clicking a picked item unpicks it. Picks are saved with the
-- hero and follow the item around the inventory.
--
-- Swap: press W. The picked items go to your hands, and only what they replace comes out, so a new
-- sword leaves your shield in place. What came out is picked instead, so W swaps back. If it doesn't fit in the inventory nothing changes and the hero says so.
local audio = require("devilutionx.audio")
local events = require("devilutionx.events")
local herostore = require("devilutionx.herostore")
local input = require("devilutionx.input")
local items = require("devilutionx.items")
local player = require("devilutionx.player")
local render = require("devilutionx.render")
local logic = require("mods.weapon_swap.logic")

local KEY = "weapon_swap.items"
local CELLS = player.InventoryColumns * player.InventoryRows
local MAX_PICKS = 2
local DOT_SIZE, DOT_MARGIN = 4, 2
local DOT_COLOR = render.PaletteColor.Red + 4

local picks = {} -- Item keys. Items have no stable reference, and cells change.
local marking = false
local swapHeld = false -- The key auto-repeats, one physical press is one swap.

local function itemKey(item)
  return item.seed .. ":" .. item.IDidx .. ":" .. item.createInfo
end

local function loadPicks()
  local keys = {}
  for key in (herostore.get(KEY) or ""):gmatch("[^,]+") do keys[#keys + 1] = key end
  return keys
end

local function savePicks()
  herostore.set(KEY, #picks > 0 and table.concat(picks, ",") or nil)
end

local function isPicked(item)
  local key = itemKey(item)
  for _, pick in ipairs(picks) do
    if pick == key then return true end
  end
  return false
end

-- The records of all inventory items, each with `picked` set.
local function inventoryRecords(p)
  local records = {}
  for cell = 0, CELLS - 1 do
    local cells = p:getInventoryItemCells(cell)
    if cells ~= nil and cells[1] == cell then
      local item = p:getInventoryItemAt(cell)
      records[#records + 1] = {
        origin = cell,
        cells = cells,
        shield = item.type == items.ItemType.Shield,
        picked = isPicked(item),
        ref = item,
      }
    end
  end
  return records
end

-- Picks that left the inventory stop counting, they could not be unpicked otherwise.
local function pickedRecords(p)
  local picked = {}
  for _, rec in ipairs(inventoryRecords(p)) do
    if rec.picked then picked[#picked + 1] = rec end
  end
  return picked
end

local function canPick(p, item)
  if not p:canUseItem(item) then return false end
  local location = p:getItemLocation(item)
  return location == items.ItemEquipType.OneHand or location == items.ItemEquipType.TwoHand
end

-- The picks that can stay next to item: those it can be held with, and one at most. Of two the one
-- of its kind goes, so a new sword replaces the sword of a sword and shield pair.
local function picksToKeep(p, item, others)
  local shield = item.type == items.ItemType.Shield
  local keep = {}
  for _, rec in ipairs(others) do
    if p:canWieldTogether(rec.ref, item) then keep[#keep + 1] = rec end
  end
  if #keep == MAX_PICKS then
    table.remove(keep, keep[1].shield == shield and 1 or 2)
  end
  return keep
end

local function togglePick(p, cell)
  local item = p:getInventoryItemAt(cell)
  if item == nil then return end
  local key = itemKey(item)
  local others = pickedRecords(p)
  local keep = {}
  if isPicked(item) then
    for _, rec in ipairs(others) do
      if itemKey(rec.ref) ~= key then keep[#keep + 1] = rec end
    end
  elseif canPick(p, item) then
    keep = picksToKeep(p, item, others)
  else
    p:say(audio.HeroSpeech.ICantDoThat)
    return
  end
  local wasPicked = isPicked(item)
  picks = {}
  for i, rec in ipairs(keep) do picks[i] = itemKey(rec.ref) end
  if not wasPicked then picks[#picks + 1] = key end
  savePicks()
end

local function heldRecord(item)
  if item == nil then return nil end
  return { w = item.invWidth, h = item.invHeight, shield = item.type == items.ItemType.Shield, ref = item }
end

local function toMoveTarget(place)
  if place.body == "left" then return { body = items.InvBodyLoc.HandLeft } end
  if place.body == "right" then return { body = items.InvBodyLoc.HandRight } end
  return place
end

local function swap()
  local p = player.self()
  local records = inventoryRecords(p)
  local view = {
    columns = player.InventoryColumns,
    rows = player.InventoryRows,
    occupied = {},
    marked = {},
    held = {
      left = heldRecord(p:getBodyItem(items.InvBodyLoc.HandLeft)),
      right = heldRecord(p:getBodyItem(items.InvBodyLoc.HandRight)),
    },
    compatible = function(a, b) return p:canWieldTogether(a.ref, b.ref) end,
  }
  for _, rec in ipairs(records) do
    for _, cell in ipairs(rec.cells) do view.occupied[cell] = true end
    if rec.picked then view.marked[#view.marked + 1] = rec end
  end

  local plan, hands = logic.planSwap(view)
  if plan == nil then
    p:say(audio.HeroSpeech.ICantDoThat)
    return
  end
  -- Item references go stale in the move.
  local sfx = view.marked[1].ref.invSfx
  local outgoing = {}
  for i, hand in ipairs(hands) do outgoing[i] = itemKey(view.held[hand].ref) end
  local moves = {}
  for i, move in ipairs(plan) do
    moves[i] = { from = toMoveTarget(move.from), to = toMoveTarget(move.to) }
  end
  if p:moveItems(moves) then
    audio.playSfx(sfx)
    picks = outgoing
    savePicks()
  else
    p:say(audio.HeroSpeech.ICantDoThat)
  end
end

events.GameStart.add(function()
  marking = false
  swapHeld = false
  picks = loadPicks()
end)

input.addAction({
  id = "WeaponSwap",
  name = "Swap Weapons",
  description = "Swap the items in your hands with the items picked in your inventory.",
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
  name = "Pick Weapon Swap Items",
  description = "Hold and click inventory items to pick them for weapon swap.",
  defaultKey = "H",
  onPress = function() marking = true end,
  onRelease = function() marking = false end,
})

events.InventoryCellClicked.add(function(cell)
  if not marking then return false end
  togglePick(player.self(), cell)
  return true
end)

events.InventoryDrawComplete.add(function()
  if #picks == 0 then return end
  local p = player.self()
  for _, rec in ipairs(pickedRecords(p)) do
    local x, y, width = render.inventoryCellRect(rec.origin + rec.ref.invWidth - 1)
    local dotX, dotY = x + width - DOT_SIZE - DOT_MARGIN, y + DOT_MARGIN
    render.rect(dotX, dotY, DOT_SIZE, DOT_SIZE, DOT_COLOR) -- rect is an outline, the second fills it.
    render.rect(dotX + 1, dotY + 1, DOT_SIZE - 2, DOT_SIZE - 2, DOT_COLOR)
  end
end)
