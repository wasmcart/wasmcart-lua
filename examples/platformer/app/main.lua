-- platformer: tile collision, gravity, jumping, a scrolling camera.
-- Demonstrates: require (lib/), transform stack for the camera, tilemaps,
-- edge-triggered input (love.pad.wasPressed).

local vec = require "vec"

local W, H = 1280, 720
local TILE = 48
local GRAVITY = 0.9
local JUMP = -17
local MOVE = 6

-- 1 = solid. 60 tiles * 48px = 2880px wide, well past the 1280px screen,
-- so the camera actually has somewhere to scroll.
local MAP = {
  "111111111111111111111111111111111111111111111111111111111111",
  "1..........................................................1",
  "1..........................................................1",
  "1.......111...................111........11................1",
  "1.....................111..................................1",
  "1..111.........1111...................111.........111......1",
  "1................................11......................111",
  "1.....11..............111.........................111......1",
  "1..........1111................111.........11..............1",
  "1........................11.........111..............111...1",
  "1...111...........111......................11..............1",
  "1.................................111......................1",
  "1............11...........11.....................111.......1",
  "111111111111111111111111111111111111111111111111111111111111",
}

local player, cam, coins, collected

local function solid(tx, ty)
  local row = MAP[ty + 1]
  if not row then return true end
  local c = row:sub(tx + 1, tx + 1)
  return c == "1" or c == ""
end

local function rect_hits_map(x, y, w, h)
  local x0, x1 = math.floor(x / TILE), math.floor((x + w - 1) / TILE)
  local y0, y1 = math.floor(y / TILE), math.floor((y + h - 1) / TILE)
  for ty = y0, y1 do
    for tx = x0, x1 do
      if solid(tx, ty) then return true end
    end
  end
  return false
end

function love.load()
  love.graphics.setBackgroundColor(0.07, 0.09, 0.14)
  player = { pos = vec(96, 96), vel = vec(0, 0), w = 34, h = 44, grounded = false }
  cam = vec(0, 0)
  collected = 0
  coins = {}
  local width = #MAP[1]
  for i = 1, 26 do
    local tx = 2 + (i * 7) % (width - 4)
    local ty = 2 + (i * 5) % 10
    if not solid(tx, ty) then
      coins[#coins + 1] = { x = tx * TILE + TILE / 2, y = ty * TILE + TILE / 2, got = false }
    end
  end
end

function love.update(dt)
  local p = player

  local dx = 0
  if love.pad.isDown("left")  then dx = -MOVE end
  if love.pad.isDown("right") then dx =  MOVE end
  local ax = love.pad.axis("leftx")
  if math.abs(ax) > 0.25 then dx = ax * MOVE end
  p.vel.x = dx

  if (love.pad.wasPressed("a") or love.pad.wasPressed("up")) and p.grounded then
    p.vel.y = JUMP
    p.grounded = false
    love.audio.beep(660)
  end

  p.vel.y = math.min(p.vel.y + GRAVITY, 20)

  -- axis-separated collision: move X, resolve; move Y, resolve.
  local nx = p.pos.x + p.vel.x
  if not rect_hits_map(nx, p.pos.y, p.w, p.h) then p.pos.x = nx else p.vel.x = 0 end

  local ny = p.pos.y + p.vel.y
  if not rect_hits_map(p.pos.x, ny, p.w, p.h) then
    p.pos.y = ny
    p.grounded = false
  else
    if p.vel.y > 0 then p.grounded = true end
    p.vel.y = 0
  end

  for _, c in ipairs(coins) do
    if not c.got and math.abs(c.x - (p.pos.x + p.w / 2)) < 28
       and math.abs(c.y - (p.pos.y + p.h / 2)) < 28 then
      c.got = true
      collected = collected + 1
      love.audio.beep(900)
    end
  end

  -- camera follows with a dead zone, clamped to the level
  local target = p.pos.x - W / 2
  cam.x = cam.x + (target - cam.x) * 0.12
  cam.x = math.max(0, math.min(#MAP[1] * TILE - W, cam.x))

  love.debugValue(0, collected)
  love.debugValue(1, math.floor(p.pos.x))
end

function love.draw()
  love.graphics.push()
  love.graphics.translate(-cam.x, -cam.y)

  -- tiles (only the visible columns)
  local first = math.max(0, math.floor(cam.x / TILE))
  local last = math.min(#MAP[1] - 1, first + math.ceil(W / TILE))
  for ty = 0, #MAP - 1 do
    for tx = first, last do
      if solid(tx, ty) then
        love.graphics.setColor(0.22, 0.28, 0.42)
        love.graphics.rectangle("fill", tx * TILE, ty * TILE, TILE, TILE)
        love.graphics.setColor(0.30, 0.38, 0.55)
        love.graphics.rectangle("fill", tx * TILE, ty * TILE, TILE, 5)
      end
    end
  end

  for _, c in ipairs(coins) do
    if not c.got then
      love.graphics.setColor(1, 0.85, 0.3)
      love.graphics.circle("fill", c.x, c.y, 11)
    end
  end

  love.graphics.setColor(0.4, 0.9, 0.6)
  love.graphics.rectangle("fill", player.pos.x, player.pos.y, player.w, player.h)

  love.graphics.pop()

  love.graphics.setColor(1, 1, 1)
  love.graphics.print(("coins %d of %d"):format(collected, #coins), 40, 36)
  love.graphics.print("left/right to move, A or up to jump", 40, 70)
end
