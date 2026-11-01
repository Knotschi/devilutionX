-- Pure weapon swap rules: no require and no game API.
local logic = {}

local function allMarked(cells, marked)
  for _, cell in ipairs(cells) do
    if not marked[cell] then return false end
  end
  return true
end

-- Marks all the cells, or unmarks them if they were all marked already.
function logic.toggle(marked, cells)
  local mark = not allMarked(cells, marked)
  for _, cell in ipairs(cells) do
    marked[cell] = mark or nil
  end
end

-- Returns the rim of the marked area as runs of grid lines: { x, y, length, horizontal }. x and y
-- are the start corner, counted in cells from the top left of the grid. Lines between two marked
-- cells are not part of it.
function logic.outline(marked, columns, rows)
  local function isMarked(x, y)
    return x >= 0 and x < columns and y >= 0 and y < rows and marked[y * columns + x] == true
  end
  local function isRim(x, y, dx, dy) return isMarked(x, y) ~= isMarked(x - dx, y - dy) end

  local runs = {}
  local function collect(lines, steps, horizontal)
    for line = 0, lines do
      local length = 0
      for step = 0, steps do
        local x, y = step, line
        if not horizontal then x, y = line, step end
        local rim = isRim(x, y, horizontal and 0 or 1, horizontal and 1 or 0)
        if rim then
          length = length + 1
        elseif length > 0 then
          runs[#runs + 1] = {
            x = horizontal and step - length or line,
            y = horizontal and line or step - length,
            length = length,
            horizontal = horizontal,
          }
          length = 0
        end
      end
    end
  end
  collect(rows, columns, true)
  collect(columns, rows, false)
  return runs
end

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
  return allMarked(itemCells(cell, rec, columns), free)
end

-- Finds a free position for each record, trying larger items first and backtracking.
-- Returns the cells in the order of recs, or nil.
local function place(recs, free, columns, rows)
  local order = {}
  for i = 1, #recs do order[i] = i end
  table.sort(order, function(a, b)
    local areaA, areaB = recs[a].w * recs[a].h, recs[b].w * recs[b].h
    if areaA ~= areaB then return areaA > areaB end
    return a < b
  end)

  local positions = {}
  local function placeFrom(index)
    if index > #order then return true end
    local rec = recs[order[index]]
    for cell = 0, columns * rows - 1 do
      if fits(cell, rec, free, columns, rows) then
        local cells = itemCells(cell, rec, columns)
        for _, c in ipairs(cells) do free[c] = nil end
        positions[order[index]] = cell
        if placeFrom(index + 1) then return true end
        for _, c in ipairs(cells) do free[c] = true end
      end
    end
    return false
  end
  if placeFrom(1) then return positions end
  return nil
end

-- The first candidate, plus the first later one that can be wielded with it.
local function selectIncoming(view, candidates)
  local first = candidates[1]
  if first.hand == "two" then return { first } end
  for i = 2, #candidates do
    local rec = candidates[i]
    if rec.hand == "one" and view.compatible(first, rec) then return { first, rec } end
  end
  return { first }
end

local function role(rec)
  return rec.shield and "shield" or "weapon"
end

-- Picks the hands to empty and the hand each incoming item goes to.
local function assign(view, incoming)
  local left, right = view.held.left, view.held.right
  local out, target = {}, {}

  if #incoming == 2 or incoming[1].hand == "two" then
    if left then out[#out + 1] = "left" end
    if right then out[#out + 1] = "right" end
    local first, second = incoming[1], incoming[2]
    if first.shield then first, second = second, first end
    target.left, target.right = first, second
    return out, target
  end

  local item = incoming[1]
  if left and left.hand == "two" and not (right and role(right) == role(item)) then
    out[1] = "left"
    target.left = item
  elseif left and role(left) == role(item) then
    out[1] = "left"
    target.left = item
  elseif right and role(right) == role(item) then
    out[1] = "right"
    target.right = item
  elseif not left then
    target.left = item
  elseif not right then
    target.right = item
  else
    out[1] = "right"
    target.right = item
  end
  return out, target
end

-- Returns { moves = ..., soundItem = rec } for the swap, or nil if it can't be done.
function logic.planSwap(view)
  local candidates = {}
  for _, rec in ipairs(view.items) do
    if rec.hand and allMarked(rec.cells, view.marked) then
      candidates[#candidates + 1] = rec
    end
  end
  if #candidates == 0 then return nil end

  local incoming = selectIncoming(view, candidates)
  local out, target = assign(view, incoming)

  local free = {}
  for cell in pairs(view.marked) do free[cell] = true end
  for _, rec in ipairs(view.items) do
    if rec ~= incoming[1] and rec ~= incoming[2] then
      for _, cell in ipairs(rec.cells) do free[cell] = nil end
    end
  end

  local recs = {}
  for i, hand in ipairs(out) do recs[i] = view.held[hand] end
  local positions = place(recs, free, view.columns, view.rows)
  if not positions then return nil end

  local moves = {}
  for i, hand in ipairs(out) do
    moves[#moves + 1] = { from = { body = hand }, to = { cell = positions[i] } }
  end
  for _, hand in ipairs({ "left", "right" }) do
    if target[hand] then
      moves[#moves + 1] = { from = { cell = target[hand].origin }, to = { body = hand } }
    end
  end
  return { moves = moves, soundItem = incoming[1] }
end

return logic
