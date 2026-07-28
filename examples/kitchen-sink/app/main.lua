-- kitchen-sink: touches every v1 API module in one screen.
-- This is the contract example: if something here regresses, the engine
-- broke a documented promise. Each panel is labeled with what it proves.

local W, H = 1280, 720
local t = 0
local canvas, font_big
local coro, coro_state

function love.load()
  love.graphics.setBackgroundColor(0.05, 0.06, 0.1)

  -- a canvas drawn once at load and composited every frame
  canvas = love.graphics.newCanvas(220, 130)
  love.graphics.setCanvas(canvas)
  love.graphics.clear(0.12, 0.1, 0.2)
  for i = 1, 22 do
    love.graphics.setColor(i / 22, 0.4, 1 - i / 22)
    love.graphics.rectangle("fill", i * 10, 20 + math.sin(i) * 30, 8, 60)
  end
  love.graphics.setCanvas()

  font_big = love.graphics.newFont(24)

  -- a coroutine that yields a value forever
  coro = coroutine.create(function()
    local n = 0
    while true do n = n + 1; coroutine.yield(n * n) end
  end)
end

function love.update(dt)
  t = t + 1
  local ok, v = coroutine.resume(coro)
  coro_state = ok and v or -1
  love.debugValue(0, t)
  love.debugValue(1, coro_state % 100000)
end

local function panel(x, y, w, h, label)
  love.graphics.setColor(0.11, 0.13, 0.2)
  love.graphics.rectangle("fill", x, y, w, h)
  love.graphics.setColor(0.3, 0.35, 0.5)
  love.graphics.rectangle("line", x, y, w, h)
  love.graphics.setColor(0.55, 0.65, 0.85)
  love.graphics.print(label, x + 12, y + 10)
end

function love.draw()
  -- 1: primitives
  panel(40, 40, 280, 200, "primitives")
  love.graphics.setColor(0.4, 0.9, 1)
  love.graphics.rectangle("fill", 70, 90, 60, 44)
  love.graphics.setColor(1, 0.6, 0.4)
  love.graphics.circle("fill", 180, 112, 26)
  love.graphics.setColor(0.7, 1, 0.5)
  love.graphics.circle("line", 250, 112, 26)
  love.graphics.setColor(1, 0.9, 0.3)
  love.graphics.line(70, 170, 280, 210)
  love.graphics.setColor(0.8, 0.5, 1)
  love.graphics.polygon("fill", { 70, 215, 120, 165, 170, 215 })

  -- 2: transforms
  panel(350, 40, 280, 200, "transform stack")
  love.graphics.push()
  love.graphics.translate(490, 145)
  love.graphics.rotate(t * 0.02)
  love.graphics.setColor(0.4, 1, 0.75)
  love.graphics.rectangle("fill", -45, -45, 90, 90)
  love.graphics.push()
  love.graphics.rotate(t * 0.05)
  love.graphics.setColor(1, 0.4, 0.6)
  love.graphics.rectangle("fill", -18, -18, 36, 36)
  love.graphics.pop()
  love.graphics.pop()

  -- 3: canvas
  panel(660, 40, 280, 200, "canvas (render target)")
  love.graphics.setColor(1, 1, 1)
  love.graphics.draw(canvas, 690, 90)

  -- 4: alpha + additive blending
  panel(970, 40, 270, 200, "blend modes")
  love.graphics.setColor(1, 0.3, 0.3, 0.55)
  love.graphics.circle("fill", 1060, 130, 42)
  love.graphics.setColor(0.3, 1, 0.4, 0.55)
  love.graphics.circle("fill", 1100, 130, 42)
  love.graphics.setBlendMode("add")
  love.graphics.setColor(0.3, 0.4, 1)
  love.graphics.circle("fill", 1080, 185, 34)
  love.graphics.setBlendMode("alpha")

  -- 5: text + fonts
  panel(40, 270, 590, 180, "text: default font, scaled font, printf align")
  love.graphics.setColor(1, 1, 1)
  love.graphics.print("default bitfont abcdefg 0123456789", 70, 320)
  love.graphics.setFont(font_big)
  love.graphics.setColor(0.6, 0.9, 1)
  love.graphics.print("scaled font", 70, 355)
  love.graphics.setFont(love.graphics.newFont(12))
  love.graphics.setColor(1, 0.85, 0.5)
  love.graphics.printf("centered printf", 70, 405, 520, "center")

  -- 6: scissor
  panel(660, 270, 280, 180, "scissor clip")
  love.graphics.setScissor(690, 310, 220, 100)
  love.graphics.setColor(0.9, 0.5, 1)
  for i = 0, 14 do
    love.graphics.circle("fill", 690 + i * 22, 340 + math.sin(t * 0.05 + i) * 40, 16)
  end
  love.graphics.setScissor()

  -- 7: coroutine + string.format + table ops (real Lua)
  panel(970, 270, 270, 180, "real Lua")
  love.graphics.setColor(0.7, 1, 0.7)
  love.graphics.print(string.format("coro yield: %d", coro_state or 0), 1000, 320)
  local list = {}
  for i = 1, 5 do list[i] = i * i end
  love.graphics.print("table: " .. table.concat(list, ","), 1000, 350)
  love.graphics.print(("fmt %.3f %s"):format(math.pi, "ok"), 1000, 380)
  -- gsub returns (string, count); parenthesize to keep only the string
  love.graphics.print("gsub: " .. (("a-b-c"):gsub("-", "+")), 1000, 410)

  -- 8: input state
  panel(40, 480, 590, 200, "input (press buttons on pad 1)")
  local names = { "left", "right", "up", "down", "a", "b", "x", "y", "start", "select" }
  for i, nm in ipairs(names) do
    local col = (i - 1) % 5
    local row = math.floor((i - 1) / 5)
    local down = love.pad.isDown(nm)
    if down then love.graphics.setColor(0.3, 1, 0.5)
    else love.graphics.setColor(0.25, 0.28, 0.38) end
    love.graphics.rectangle("fill", 70 + col * 110, 530 + row * 60, 96, 44)
    love.graphics.setColor(0, 0, 0)
    love.graphics.print(nm, 82 + col * 110, 545 + row * 60)
  end

  -- 9: deterministic RNG + math
  panel(660, 480, 580, 200, "deterministic RNG + love.math.noise")
  love.graphics.setColor(0.5, 0.8, 1)
  for i = 0, 55 do
    local n = love.math.noise(i * 0.15, t * 0.01)
    love.graphics.rectangle("fill", 690 + i * 10, 640 - n * 90, 8, n * 90)
  end
end
