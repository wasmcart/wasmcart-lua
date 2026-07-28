-- shmup: a vertical shooter with pooled entities.
-- Demonstrates: coroutines for the wave script, table-heavy update loops
-- (the GC-pressure case), blend modes, deterministic RNG.

local W, H = 1280, 720

local player, bullets, enemies, particles, score, lives, wave_co, t

local function spawn_enemy(x, y, kind)
  enemies[#enemies + 1] = {
    x = x, y = y, kind = kind,
    hp = kind == "big" and 6 or 1,
    r = kind == "big" and 34 or 18,
    vy = kind == "big" and 1.4 or 2.6,
    phase = love.math.random() * 6.28,
  }
end

-- the wave script as a coroutine: linear code for a timeline
local function wave_script()
  local function wait(frames) for _ = 1, frames do coroutine.yield() end end
  while true do
    for row = 1, 3 do
      for i = 1, 8 do spawn_enemy(180 + i * 120, -40 - row * 70, "small") end
      wait(40)
    end
    wait(90)
    spawn_enemy(W / 2, -80, "big")
    wait(200)
  end
end

function love.load()
  love.graphics.setBackgroundColor(0.03, 0.04, 0.08)
  player = { x = W / 2, y = H - 120, cool = 0 }
  bullets, enemies, particles = {}, {}, {}
  score, lives, t = 0, 3, 0
  wave_co = coroutine.create(wave_script)
end

local function burst(x, y, n, r, g, b)
  for _ = 1, n do
    particles[#particles + 1] = {
      x = x, y = y,
      vx = (love.math.random() * 2 - 1) * 6,
      vy = (love.math.random() * 2 - 1) * 6,
      life = 1, r = r, g = g, b = b,
    }
  end
end

function love.update(dt)
  t = t + 1

  if coroutine.status(wave_co) == "suspended" then
    local ok, err = coroutine.resume(wave_co)
    if not ok then love.log("wave error:", err) end
  end

  -- player
  local dx = 0
  if love.pad.isDown("left")  then dx = -8 end
  if love.pad.isDown("right") then dx =  8 end
  local ax = love.pad.axis("leftx")
  if math.abs(ax) > 0.2 then dx = ax * 8 end
  player.x = math.max(30, math.min(W - 30, player.x + dx))

  player.cool = player.cool - 1
  if love.pad.isDown("a") and player.cool <= 0 then
    bullets[#bullets + 1] = { x = player.x, y = player.y - 26, vy = -14 }
    player.cool = 8
    love.audio.beep(1200)
  end

  -- bullets
  for i = #bullets, 1, -1 do
    local b = bullets[i]
    b.y = b.y + b.vy
    if b.y < -20 then table.remove(bullets, i) end
  end

  -- enemies
  for i = #enemies, 1, -1 do
    local e = enemies[i]
    e.y = e.y + e.vy
    e.x = e.x + math.sin((t + e.phase * 20) * 0.03) * 2
    if e.y > H + 60 then
      table.remove(enemies, i)
    else
      for j = #bullets, 1, -1 do
        local b = bullets[j]
        local ddx, ddy = b.x - e.x, b.y - e.y
        if ddx * ddx + ddy * ddy < e.r * e.r then
          table.remove(bullets, j)
          e.hp = e.hp - 1
          burst(e.x, e.y, 6, 1, 0.7, 0.2)
          if e.hp <= 0 then
            burst(e.x, e.y, 20, 1, 0.4, 0.2)
            score = score + (e.kind == "big" and 500 or 100)
            love.audio.beep(300)
            table.remove(enemies, i)
          end
          break
        end
      end
    end
  end

  -- particles
  for i = #particles, 1, -1 do
    local p = particles[i]
    p.x = p.x + p.vx; p.y = p.y + p.vy
    p.vy = p.vy + 0.15
    p.life = p.life - 0.025
    if p.life <= 0 then table.remove(particles, i) end
  end

  love.debugValue(0, score)
  love.debugValue(1, #enemies)
end

function love.draw()
  -- starfield (deterministic from the frame counter, no state)
  love.graphics.setColor(0.5, 0.55, 0.7)
  for i = 1, 60 do
    local sx = (i * 197) % W
    local sy = ((i * 131) + t * 2) % H
    love.graphics.rectangle("fill", sx, sy, 2, 2)
  end

  love.graphics.setBlendMode("add")
  for _, p in ipairs(particles) do
    love.graphics.setColor(p.r * p.life, p.g * p.life, p.b * p.life)
    love.graphics.circle("fill", p.x, p.y, 4 * p.life + 1)
  end
  love.graphics.setBlendMode("alpha")

  love.graphics.setColor(1, 0.95, 0.4)
  for _, b in ipairs(bullets) do
    love.graphics.rectangle("fill", b.x - 3, b.y - 12, 6, 18)
  end

  for _, e in ipairs(enemies) do
    if e.kind == "big" then love.graphics.setColor(1, 0.4, 0.45)
    else love.graphics.setColor(0.9, 0.5, 1) end
    love.graphics.circle("fill", e.x, e.y, e.r)
  end

  love.graphics.setColor(0.4, 0.9, 1)
  love.graphics.polygon("fill", {
    player.x, player.y - 26,
    player.x - 22, player.y + 20,
    player.x + 22, player.y + 20,
  })

  love.graphics.setColor(1, 1, 1)
  love.graphics.print("score " .. score, 40, 36)
  love.graphics.print("left/right to move, A to fire", 40, H - 54)
end
