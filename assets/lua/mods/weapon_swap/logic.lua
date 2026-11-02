-- Pure weapon swap rules: no require and no game API.
local logic = {}

local function itemCells(cell, rec, columns)
  local cells = {}
  for y = 0, rec.h - 1 do
    for x = 0, rec.w - 1 do
      cells[#cells + 1] = cell + y * columns + x
    end
  end
  return cells
end

local function fits(cell, rec, free, columns, rows)
  if cell % columns + rec.w > columns or cell // columns + rec.h > rows then return false end
  for _, c in ipairs(itemCells(cell, rec, columns)) do
    if not free[c] then return false end
  end
  return true
end

-- Finds a free position for each record, backtracking. Fills positions, returns whether all fit.
local function place(recs, index, free, positions, columns, rows)
  local rec = recs[index]
  if rec == nil then return true end
  for cell = 0, columns * rows - 1 do
    if fits(cell, rec, free, columns, rows) then
      local cells = itemCells(cell, rec, columns)
      for _, c in ipairs(cells) do free[c] = nil end
      positions[index] = cell
      if place(recs, index + 1, free, positions, columns, rows) then return true end
      for _, c in ipairs(cells) do free[c] = true end
    end
  end
  return false
end

-- Picks the hands to empty: all for two marked items, else those whose item doesn't go with the
-- marked one. If that leaves no hand free, the item of the marked one's kind has to go.
local function handsToEmpty(view)
  local left, right = view.held.left, view.held.right
  local item = view.marked[1]
  local out = {}
  for _, hand in ipairs({ "left", "right" }) do
    local held = view.held[hand]
    if held and (view.marked[2] or not view.compatible(item, held)) then out[hand] = true end
  end
  if left and right and not out.left and not out.right then
    out[left.shield == item.shield and "left" or "right"] = true
  end
  return out
end

-- view: { columns, rows, occupied = { [cell] = true }, marked = { rec... }, held = { left, right },
-- compatible = function(a, b) }. A marked record has origin, cells and shield, a held one w, h and
-- shield. Hands holding what the marked items replace are emptied, and a held item that can stay
-- does, so a new sword leaves the shield alone. Marked shields go right if they can. Returns the
-- moves and the hands emptied, or nil if there is no marked item or no room for what comes out.
function logic.planSwap(view)
  if #view.marked == 0 then return nil end

  local free = {}
  for cell = 0, view.columns * view.rows - 1 do
    if not view.occupied[cell] then free[cell] = true end
  end
  for _, rec in ipairs(view.marked) do
    for _, cell in ipairs(rec.cells) do free[cell] = true end
  end

  local out = handsToEmpty(view)
  local hands, recs = {}, {}
  for _, hand in ipairs({ "left", "right" }) do
    if out[hand] then
      hands[#hands + 1] = hand
      recs[#recs + 1] = view.held[hand]
    end
  end
  local positions = {}
  if not place(recs, 1, free, positions, view.columns, view.rows) then return nil end

  local moves = {}
  for i, hand in ipairs(hands) do
    moves[#moves + 1] = { from = { body = hand }, to = { cell = positions[i] } }
  end
  local first, second = view.marked[1], view.marked[2]
  if first.shield then first, second = second, first end
  local function isFree(hand) return view.held[hand] == nil or out[hand] end
  if first and second then
    moves[#moves + 1] = { from = { cell = first.origin }, to = { body = "left" } }
    moves[#moves + 1] = { from = { cell = second.origin }, to = { body = "right" } }
  else
    local item = first or second
    local hand = item.shield and "right" or "left"
    if not isFree(hand) then hand = hand == "left" and "right" or "left" end
    moves[#moves + 1] = { from = { cell = item.origin }, to = { body = hand } }
  end
  return moves, hands
end

return logic
