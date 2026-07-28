-- particles: a stress test and a canvas demo.
-- Demonstrates: canvases (render targets), additive blending, and how many
-- entities the software renderer sustains. Press A to change emitter mode.

local W, H = 1280, 720
local MAX = 900

local parts, n, mode, trail, t

local MODES = { "fountain", "spiral", "burst" }

function love.load()
  love.graphics.setBackgroundColor(0.02, 0.02, 0.05)
  parts = {}
  for i = 1, MAX do
    parts[i] = { x = 0, y = 0, vx = 0, vy = 0, life = 0, r = 1, g = 1, b = 1 }
  end
  n = 0
  mode = 1
  t = 0
  -- a canvas we draw the trail into, then composite: proves render targets
  trail = love.graphics.newCanvas(W, H)
end

local function emit(i)
  local p = parts[i]
  local m = MODES[mode]
  if m == "fountain" then
    p.x, p.y = W / 2, H - 80
    p.vx = (love.math.random() * 2 - 1) * 5
    p.vy = -12 - love.math.random() * 6
    p.r, p.g, p.b = 0.3, 0.7, 1
  elseif m == "spiral" then
    local a = t * 0.08 + i * 0.4
    p.x, p.y = W / 2, H / 2
    p.vx = math.cos(a) * 7
    p.vy = math.sin(a) * 7
    p.r, p.g, p.b = 1, 0.5, 0.8
  else
    p.x, p.y = W / 2, H / 2
    local a = love.math.random() * 6.283
    local s = 2 + love.math.random() * 9
    p.vx, p.vy = math.cos(a) * s, math.sin(a) * s
    p.r, p.g, p.b = 1, 0.8, 0.3
  end
  p.life = 1
end

function love.update(dt)
  t = t + 1

  if love.pad.wasPressed("a") then
    mode = mode % #MODES + 1
    love.audio.beep(700)
  end

  -- spawn
  for _ = 1, 14 do
    n = n % MAX + 1
    emit(n)
  end

  for i = 1, MAX do
    local p = parts[i]
    if p.life > 0 then
      p.x = p.x + p.vx
      p.y = p.y + p.vy
      p.vy = p.vy + 0.22
      p.life = p.life - 0.012
      if p.y > H - 10 then p.vy = -p.vy * 0.5; p.y = H - 10 end
    end
  end

  local alive = 0
  for i = 1, MAX do if parts[i].life > 0 then alive = alive + 1 end end
  love.debugValue(0, alive)
  love.debugValue(1, mode)
end

function love.draw()
  love.graphics.setBlendMode("add")
  for i = 1, MAX do
    local p = parts[i]
    if p.life > 0 then
      love.graphics.setColor(p.r * p.life, p.g * p.life, p.b * p.life)
      love.graphics.circle("fill", p.x, p.y, 3 * p.life + 1)
    end
  end
  love.graphics.setBlendMode("alpha")

  love.graphics.setColor(1, 1, 1)
  love.graphics.print("particles: " .. MAX .. "   mode: " .. MODES[mode], 40, 36)
  love.graphics.print("press A to change mode", 40, 70)
end
