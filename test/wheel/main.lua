-- wheel/main.lua - does the scroll wheel actually reach Lua?
--
-- Proves the whole chain in one cart: the host accumulates platform wheel
-- events, writes wc_wheel_t, the runtime divides by 120 into notches, and
-- the prelude both exposes the poll (love.mouse.wheel) and fires the
-- LOVE-shaped callback (love.wheelmoved) exactly once per frame with a
-- nonzero delta.
--
-- The clearing contract is the part most likely to break, and the failure
-- is nasty: a wheel that is never cleared scrolls forever off one flick.
-- So the cart reports the per-frame value every frame and the harness
-- asserts it returns to zero.

local frame = 0
local calls = 0          -- how many times wheelmoved fired
local lastDx, lastDy = 0, 0
local acc = 0            -- running total of every notch delivered

function love.load()
  print("@wheel boot poll=" .. tostring(love.mouse and love.mouse.wheel ~= nil)
        .. " cb=" .. tostring(love.wheelmoved ~= nil))
end

function love.wheelmoved(dx, dy)
  calls = calls + 1
  lastDx, lastDy = dx, dy
  acc = acc + dy
  print(string.format("@wheel cb frame=%d dx=%.4f dy=%.4f calls=%d acc=%.4f",
                      frame, dx, dy, calls, acc))
end

function love.update()
  frame = frame + 1
  -- The POLL, read every frame whether or not anything scrolled: zero is
  -- the normal state on hardware with no wheel, and reading it must be
  -- harmless rather than something to guard.
  local px, py = love.mouse.wheel()
  print(string.format("@wheel poll frame=%d dx=%.4f dy=%.4f calls=%d",
                      frame, px, py, calls))
end

function love.draw()
  love.graphics.clear(0.1, 0.12, 0.1)
  love.graphics.setColor(1, 1, 1)
  love.graphics.print("wheel " .. string.format("%.2f", acc), 20, 20)
end
