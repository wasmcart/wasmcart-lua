-- breakout: bricks, save data, and game states.
-- Demonstrates: love.filesystem save/load (persisted high score), state
-- machines, and string serialization round-tripping through the save region.

local W, H = 1280, 720

local paddle, ball, bricks, score, high, state, lives

local function load_high()
  local s = love.filesystem.load_save()
  if not s then return 0 end
  local n = tonumber(s:match("^high=(%d+)"))
  return n or 0
end

local function save_high(v)
  love.filesystem.write(nil, "high=" .. math.floor(v))
end

local function build_bricks()
  bricks = {}
  local cols, rows = 12, 6
  local bw, bh = 88, 34
  local ox = (W - cols * (bw + 8)) / 2
  for r = 1, rows do
    for c = 1, cols do
      bricks[#bricks + 1] = {
        x = ox + (c - 1) * (bw + 8),
        y = 90 + (r - 1) * (bh + 10),
        w = bw, h = bh, alive = true,
        hue = r / rows,
      }
    end
  end
end

local function reset_ball()
  ball = { x = W / 2, y = H - 180, vx = 5.5, vy = -6.5, r = 11 }
end

function love.load()
  love.graphics.setBackgroundColor(0.04, 0.05, 0.09)
  paddle = { x = W / 2 - 90, w = 180, h = 22, y = H - 70 }
  score, lives = 0, 3
  high = load_high()
  state = "play"
  build_bricks()
  reset_ball()
end

function love.update(dt)
  if state ~= "play" then
    if love.pad.wasPressed("start") or love.pad.wasPressed("a") then
      score, lives = 0, 3
      state = "play"
      build_bricks()
      reset_ball()
    end
    return
  end

  local dx = 0
  if love.pad.isDown("left")  then dx = -11 end
  if love.pad.isDown("right") then dx =  11 end
  local ax = love.pad.axis("leftx")
  if math.abs(ax) > 0.2 then dx = ax * 11 end
  paddle.x = math.max(0, math.min(W - paddle.w, paddle.x + dx))

  ball.x = ball.x + ball.vx
  ball.y = ball.y + ball.vy

  if ball.x - ball.r < 0 then ball.x = ball.r; ball.vx = -ball.vx; love.audio.beep(500) end
  if ball.x + ball.r > W then ball.x = W - ball.r; ball.vx = -ball.vx; love.audio.beep(500) end
  if ball.y - ball.r < 0 then ball.y = ball.r; ball.vy = -ball.vy; love.audio.beep(500) end

  -- paddle
  if ball.vy > 0 and ball.y + ball.r > paddle.y
     and ball.y < paddle.y + paddle.h
     and ball.x > paddle.x and ball.x < paddle.x + paddle.w then
    ball.vy = -math.abs(ball.vy)
    ball.vx = ((ball.x - (paddle.x + paddle.w / 2)) / (paddle.w / 2)) * 8
    love.audio.beep(800)
  end

  -- bricks
  for _, b in ipairs(bricks) do
    if b.alive and ball.x + ball.r > b.x and ball.x - ball.r < b.x + b.w
       and ball.y + ball.r > b.y and ball.y - ball.r < b.y + b.h then
      b.alive = false
      ball.vy = -ball.vy
      score = score + 10
      love.audio.beep(1000)
      break
    end
  end

  -- lost the ball
  if ball.y > H + 40 then
    lives = lives - 1
    love.audio.beep(180)
    if lives <= 0 then
      state = "over"
      if score > high then high = score; save_high(high) end
    else
      reset_ball()
    end
  end

  local left = 0
  for _, b in ipairs(bricks) do if b.alive then left = left + 1 end end
  if left == 0 then
    state = "won"
    if score > high then high = score; save_high(high) end
  end

  love.debugValue(0, score)
  love.debugValue(1, lives)
end

function love.draw()
  for _, b in ipairs(bricks) do
    if b.alive then
      love.graphics.setColor(0.4 + b.hue * 0.6, 0.9 - b.hue * 0.5, 1 - b.hue * 0.4)
      love.graphics.rectangle("fill", b.x, b.y, b.w, b.h)
    end
  end

  love.graphics.setColor(0.9, 0.95, 1)
  love.graphics.rectangle("fill", paddle.x, paddle.y, paddle.w, paddle.h)
  love.graphics.circle("fill", ball.x, ball.y, ball.r)

  love.graphics.setColor(1, 1, 1)
  love.graphics.print("score " .. score .. "   lives " .. lives .. "   high " .. high, 40, 36)

  if state == "over" then
    love.graphics.print("GAME OVER - press A to restart", W / 2 - 260, H / 2)
  elseif state == "won" then
    love.graphics.print("CLEARED! - press A to play again", W / 2 - 280, H / 2)
  end
end
