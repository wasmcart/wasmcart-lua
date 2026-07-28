-- gl2dblend/main.lua - additive blending on the GL path.
--
-- The software path does dst = min(255, dst + src*a/255). That is
-- GL_SRC_ALPHA / GL_ONE for colour, with destination alpha left alone so the
-- framebuffer and canvases stay opaque -- which is what blend_px does.
--
-- Additive is where "close enough" is most visible: it saturates, so a
-- rounding difference that would vanish under alpha blending can accumulate
-- across stacked draws instead.

local img

function love.load()
  love.graphics.setBackgroundColor(0.02, 0.02, 0.06)
  img = love.graphics.newImage("sprite.png")
end

function love.draw()
  -- a normal-blended backdrop to add onto
  love.graphics.setColor(0.15, 0.1, 0.25)
  love.graphics.rectangle("fill", 0, 0, 1280, 400)

  love.graphics.setBlendMode("add")

  -- overlapping additive rects: the classic glow stack, and the case that
  -- saturates
  for i = 0, 9 do
    love.graphics.setColor(0.15, 0.05, 0.02, 1)
    love.graphics.rectangle("fill", 60 + i * 40, 60, 200, 100)
  end

  -- additive at partial alpha
  for i = 0, 7 do
    love.graphics.setColor(0.1, 0.2, 0.3, 0.5)
    love.graphics.rectangle("fill", 60 + i * 50, 200, 220, 90)
  end

  -- additive sprites, including overlapping ones
  love.graphics.setColor(1, 1, 1)
  love.graphics.draw(img, 700, 60, 0, 2, 2)
  love.graphics.draw(img, 760, 100, 0, 2, 2)
  love.graphics.setColor(1, 0.6, 0.3, 0.7)
  love.graphics.draw(img, 900, 60, 0, 3, 3)

  -- additive lines and text
  love.graphics.setColor(0.3, 0.5, 0.2)
  for i = 0, 11 do
    love.graphics.line(60, 320 + i * 5, 620, 320 + i * 5)
  end
  love.graphics.setColor(0.4, 0.4, 0.1)
  love.graphics.print("ADDITIVE TEXT", 700, 330)

  -- back to alpha, and draw over the top: proves the mode is restored
  love.graphics.setBlendMode("alpha")
  love.graphics.setColor(0.9, 0.3, 0.4, 0.8)
  love.graphics.rectangle("fill", 60, 430, 400, 120)
  love.graphics.setColor(1, 1, 1)
  love.graphics.draw(img, 520, 430, 0, 2, 2)
  love.graphics.print("ALPHA AGAIN", 700, 470)
end
