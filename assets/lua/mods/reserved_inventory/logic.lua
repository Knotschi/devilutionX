-- Pure reserved inventory rules: no require and no game API.
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

-- Whether a width by height item at cell would touch a marked cell.
function logic.overlaps(marked, cell, width, height, columns)
  for y = 0, height - 1 do
    for x = 0, width - 1 do
      if marked[cell + y * columns + x] then return true end
    end
  end
  return false
end

return logic
