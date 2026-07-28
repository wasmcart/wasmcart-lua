-- bench/main.lua - where does the time actually go?
--
-- This exists to answer one question before any JIT work starts: is Lua
-- execution the bottleneck, or is it pixel fill? The JIT_PLAN is explicit
-- that Tier 1 must be aimed at MEASURED hot spots, not guessed ones, and
-- that C builtins (Tier -1) are the cheaper lever if the hot loop is not
-- actually in Lua.
--
-- Two groups:
--   micro  - classic Lua benchmarks (pure interpreter throughput)
--   macro  - what games actually do (entity updates, collision, particles)
--
-- IMPORTANT: love.timer.getTime() is frame-quantized by design (it is
-- frame_n * 1/60, so replays stay deterministic). It therefore CANNOT time
-- work inside a frame -- every delta would read exactly zero. So the cart
-- runs exactly ONE workload per frame and announces which one; the HOST
-- (test/bench.js) times each wc_render call with a real clock.

local WORK = {}
local function bench(name, fn, n) WORK[#WORK + 1] = { name = name, fn = fn, n = n or 1 } end

-- ── micro: classic interpreter benchmarks ──────────────────────────

local function fib(n)
  if n < 2 then return n end
  return fib(n - 1) + fib(n - 2)
end

local function mandel(size)
  local sum = 0
  for y = 0, size - 1 do
    local ci = 2 * y / size - 1
    for x = 0, size - 1 do
      local cr = 2 * x / size - 1.5
      local zr, zi, i = 0, 0, 0
      while i < 50 and zr * zr + zi * zi < 4 do
        zr, zi = zr * zr - zi * zi + cr, 2 * zr * zi + ci
        i = i + 1
      end
      sum = sum + i
    end
  end
  return sum
end

local function nbody(steps)
  -- 5 bodies, the classic shootout setup, simplified
  local b = {}
  for i = 1, 5 do
    b[i] = { x = i * 1.1, y = i * 0.7, z = i * 0.3,
             vx = 0, vy = 0, vz = 0, m = 1.0 / i }
  end
  for _ = 1, steps do
    for i = 1, 5 do
      for j = i + 1, 5 do
        local dx = b[i].x - b[j].x
        local dy = b[i].y - b[j].y
        local dz = b[i].z - b[j].z
        local d2 = dx * dx + dy * dy + dz * dz + 1e-9
        local mag = 0.01 / (d2 * math.sqrt(d2))
        b[i].vx = b[i].vx - dx * b[j].m * mag
        b[i].vy = b[i].vy - dy * b[j].m * mag
        b[i].vz = b[i].vz - dz * b[j].m * mag
        b[j].vx = b[j].vx + dx * b[i].m * mag
        b[j].vy = b[j].vy + dy * b[i].m * mag
        b[j].vz = b[j].vz + dz * b[i].m * mag
      end
    end
    for i = 1, 5 do
      b[i].x = b[i].x + b[i].vx
      b[i].y = b[i].y + b[i].vy
      b[i].z = b[i].z + b[i].vz
    end
  end
  return b[1].x
end

local function strcat(n)
  local t = {}
  for i = 1, n do t[#t + 1] = tostring(i) end
  return #table.concat(t, ",")
end

-- ── macro: what games actually do ──────────────────────────────────

local ents
local function entity_update(count)
  if not ents or #ents ~= count then
    ents = {}
    for i = 1, count do
      ents[i] = { x = i % 1280, y = (i * 7) % 720,
                  vx = (i % 5) - 2, vy = (i % 3) - 1, hp = 10, alive = true }
    end
  end
  -- the shape of a bullet-hell update: position, bounds, per-entity branch
  for i = 1, count do
    local e = ents[i]
    if e.alive then
      e.x = e.x + e.vx
      e.y = e.y + e.vy
      if e.x < 0 or e.x > 1280 then e.vx = -e.vx end
      if e.y < 0 or e.y > 720 then e.vy = -e.vy end
    end
  end
end

local function aabb_sweep(count)
  -- naive O(n^2/2) overlap test on a subset: the classic thing games do
  -- badly in Lua and the strongest candidate for a C builtin
  local hits = 0
  for i = 1, count do
    local a = ents[i]
    for j = i + 1, math.min(count, i + 24) do
      local b = ents[j]
      if a.x < b.x + 16 and a.x + 16 > b.x
         and a.y < b.y + 16 and a.y + 16 > b.y then
        hits = hits + 1
      end
    end
  end
  return hits
end

local parts
local function particle_step(count)
  if not parts or #parts ~= count then
    parts = {}
    for i = 1, count do
      parts[i] = { x = 640, y = 360, vx = (i % 11) - 5, vy = (i % 7) - 3, life = 1 }
    end
  end
  for i = 1, count do
    local p = parts[i]
    p.x = p.x + p.vx
    p.y = p.y + p.vy
    p.vy = p.vy + 0.22
    p.life = p.life - 0.01
    if p.life <= 0 then p.x, p.y, p.life = 640, 360, 1 end
  end
end

-- ── the renderer, for comparison ────────────────────────────────────
-- If drawing dominates, the JIT is aimed at the wrong thing.

local function draw_rects(n)
  for i = 1, n do
    love.graphics.setColor(0.3, 0.6, 0.9)
    love.graphics.rectangle("fill", (i * 13) % 1200, (i * 29) % 680, 24, 24)
  end
end

local function draw_circles(n)
  for i = 1, n do
    love.graphics.setColor(0.9, 0.5, 0.3)
    love.graphics.circle("fill", (i * 17) % 1200, (i * 31) % 680, 12)
  end
end

local function fill_screen(n)
  for _ = 1, n do
    love.graphics.setColor(0.1, 0.1, 0.15)
    love.graphics.rectangle("fill", 0, 0, 1280, 720)
  end
end

-- ── driver ─────────────────────────────────────────────────────────
--
-- One workload per frame. The cart names the workload it is ABOUT to run
-- via the debug channel, runs it, and advances. The host times each frame.

bench("micro/fib(24)",         function() fib(24) end)
bench("micro/mandel(48)",      function() mandel(48) end)
bench("micro/nbody(2000)",     function() nbody(2000) end)
bench("micro/strcat(20k)",     function() strcat(20000) end)
bench("macro/entities(2000)",  function() entity_update(2000) end)
bench("macro/entities(2000)x20", function() for _=1,20 do entity_update(2000) end end)
bench("macro/aabb(2000x24)",   function() aabb_sweep(2000) end)
bench("macro/particles(2000)", function() particle_step(2000) end)
bench("macro/particles(2000)x20", function() for _=1,20 do particle_step(2000) end end)
bench("draw/rects(2000)",      function() draw_rects(2000) end)
bench("draw/circles(2000)",    function() draw_circles(2000) end)
bench("draw/fullscreen(10)",   function() fill_screen(10) end)

local idx, warmed = 0, false

function love.load()
  love.graphics.setBackgroundColor(0.04, 0.05, 0.08)
  -- build the entity/particle tables once so allocation is not timed
  entity_update(2000); particle_step(2000)
  love.log("BENCHCOUNT " .. #WORK)
end

function love.update(dt)
  -- one warm frame first: the first frame after boot pays for JIT-warmup in
  -- the HOST's wasm engine and would otherwise be charged to workload 1
  if not warmed then warmed = true; return end
  idx = idx + 1
  local w = WORK[idx]
  if not w then
    if idx == #WORK + 1 then love.log("BENCHDONE") end
    return
  end
  love.log("BENCHRUN " .. w.name)
  w.fn()
end

function love.draw()
  love.graphics.setColor(1, 1, 1)
  local w = WORK[idx]
  love.graphics.print(w and ("running " .. w.name) or "bench complete", 40, 40)
end
