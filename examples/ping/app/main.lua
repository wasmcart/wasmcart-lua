-- ping: two-player paddle game.
-- Demonstrates: love.update/draw, love.pad for two players, rectangles,
-- deterministic love.math.random, sound via love.audio.beep.

local W, H = 1280, 720
local PADDLE_W, PADDLE_H = 20, 130
local PADDLE_SPEED = 9
local BALL_R = 12

local left, right, ball, scores

local function reset_ball(dir)
  ball = {
    x = W / 2, y = H / 2,
    vx = dir * (7 + love.math.random() * 2),
    vy = (love.math.random() * 2 - 1) * 6,
  }
end

function love.load()
  love.graphics.setBackgroundColor(0.05, 0.06, 0.09)
  left  = { y = H / 2 - PADDLE_H / 2 }
  right = { y = H / 2 - PADDLE_H / 2 }
  scores = { 0, 0 }
  reset_ball(1)
end

local function move_paddle(p, up, down)
  if up   then p.y = p.y - PADDLE_SPEED end
  if down then p.y = p.y + PADDLE_SPEED end
  p.y = math.max(0, math.min(H - PADDLE_H, p.y))
end

function love.update(dt)
  move_paddle(left,  love.pad.isDown(1, "up"),   love.pad.isDown(1, "down"))
  move_paddle(right, love.pad.isDown(2, "up"),   love.pad.isDown(2, "down"))

  -- analog sticks too
  local a1 = love.pad.axis(1, "lefty")
  if math.abs(a1) > 0.2 then left.y = left.y + a1 * PADDLE_SPEED end
  local a2 = love.pad.axis(2, "lefty")
  if math.abs(a2) > 0.2 then right.y = right.y + a2 * PADDLE_SPEED end
  left.y  = math.max(0, math.min(H - PADDLE_H, left.y))
  right.y = math.max(0, math.min(H - PADDLE_H, right.y))

  ball.x = ball.x + ball.vx
  ball.y = ball.y + ball.vy

  -- top/bottom walls
  if ball.y - BALL_R < 0 then ball.y = BALL_R; ball.vy = -ball.vy; love.audio.beep(500) end
  if ball.y + BALL_R > H then ball.y = H - BALL_R; ball.vy = -ball.vy; love.audio.beep(500) end

  -- paddles
  if ball.vx < 0 and ball.x - BALL_R < 60 and ball.x - BALL_R > 30
     and ball.y > left.y and ball.y < left.y + PADDLE_H then
    ball.vx = -ball.vx * 1.03
    ball.vy = ball.vy + ((ball.y - (left.y + PADDLE_H / 2)) / PADDLE_H) * 6
    love.audio.beep(720)
  end
  if ball.vx > 0 and ball.x + BALL_R > W - 60 and ball.x + BALL_R < W - 30
     and ball.y > right.y and ball.y < right.y + PADDLE_H then
    ball.vx = -ball.vx * 1.03
    ball.vy = ball.vy + ((ball.y - (right.y + PADDLE_H / 2)) / PADDLE_H) * 6
    love.audio.beep(720)
  end

  -- scoring
  if ball.x < -40 then scores[2] = scores[2] + 1; love.audio.beep(220); reset_ball(1) end
  if ball.x > W + 40 then scores[1] = scores[1] + 1; love.audio.beep(220); reset_ball(-1) end

  love.debugValue(0, scores[1])
  love.debugValue(1, scores[2])
end

function love.draw()
  -- center line (starts below the score so the digits stay readable)
  love.graphics.setColor(0.2, 0.22, 0.3)
  for y = 100, H, 40 do
    love.graphics.rectangle("fill", W / 2 - 3, y, 6, 22)
  end

  love.graphics.setColor(0.45, 0.85, 1)
  love.graphics.rectangle("fill", 40, left.y, PADDLE_W, PADDLE_H)
  love.graphics.setColor(1, 0.55, 0.5)
  love.graphics.rectangle("fill", W - 60, right.y, PADDLE_W, PADDLE_H)

  love.graphics.setColor(1, 1, 1)
  love.graphics.circle("fill", ball.x, ball.y, BALL_R)

  love.graphics.print(tostring(scores[1]), W / 2 - 120, 40)
  love.graphics.print(tostring(scores[2]), W / 2 + 90, 40)
  love.graphics.print("p1: pad 1 up/down    p2: pad 2 up/down", 380, H - 50)
end
