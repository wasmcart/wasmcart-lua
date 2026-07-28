-- Your game. Edit this file and rerun ./run.sh

local x, y = 640, 360
local vx, vy = 0, 0
local SPEED = 6

function love.load()
  love.graphics.setBackgroundColor(0.08, 0.09, 0.13)
end

function love.update(dt)
  vx, vy = 0, 0
  if love.pad.isDown("left")  then vx = -SPEED end
  if love.pad.isDown("right") then vx =  SPEED end
  if love.pad.isDown("up")    then vy = -SPEED end
  if love.pad.isDown("down")  then vy =  SPEED end

  -- analog stick also works
  local ax, ay = love.pad.axis("leftx"), love.pad.axis("lefty")
  if math.abs(ax) > 0.2 then vx = ax * SPEED end
  if math.abs(ay) > 0.2 then vy = ay * SPEED end

  x = math.max(40, math.min(1240, x + vx))
  y = math.max(40, math.min(680, y + vy))
end

function love.draw()
  love.graphics.setColor(0.4, 0.8, 1)
  love.graphics.circle("fill", x, y, 36)

  love.graphics.setColor(1, 1, 1)
  love.graphics.print("move with the d-pad or left stick", 40, 40)
end
