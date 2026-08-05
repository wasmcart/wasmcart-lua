-- confres: proves conf.lua resolution selection end to end.
-- debugValue(0) = fail count, debugValue(1) = assertions run (unit-cart idiom).

local fails, total = 0, 0
local function eq(got, want, what)
  total = total + 1
  if got ~= want then
    fails = fails + 1
    print(string.format("confres FAIL %s: got %s want %s", what, tostring(got), tostring(want)))
  end
end

function love.load()
  eq(love.graphics.getWidth(), 1600, "getWidth follows conf")
  eq(love.graphics.getHeight(), 900, "getHeight follows conf")
  love.graphics.setBackgroundColor(0.1, 0.2, 0.3)
end

function love.draw()
  -- A 100x100 marker flush against the bottom-right corner. If the engine's
  -- framebuffer stride disagreed with the reported width, this square would
  -- shear into diagonal streaks and the harness's corner probe would miss it.
  love.graphics.setColor(1, 0.5, 0)
  love.graphics.rectangle("fill", 1500, 800, 100, 100)
  love.debugValue(0, fails)
  love.debugValue(1, total)
end
