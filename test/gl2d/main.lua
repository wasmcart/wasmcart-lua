-- gl2d/main.lua - a cart that stays entirely on the GL2D fast path.
--
-- GL2D only implements axis-aligned rects, lines and sprites to the screen.
-- Anything else (circles, polygons, text, canvases, scissor, additive,
-- rotation) trips the sticky whole-frame CPU fallback -- which is correct,
-- but means a cart using them measures the SOFTWARE path no matter which
-- engine runs it. test/prims does exactly that, so it reports a perfect
-- match and proves nothing about GL.
--
-- This cart deliberately uses ONLY what GL2D supports, so a GL/CPU
-- comparison actually measures the GL backend.

-- A real PNG, NOT a canvas. setCanvas trips the sticky fallback the moment
-- it is called -- even in love.load, before a single frame is drawn -- so a
-- cart that prepares its art in a canvas can never reach the GL path. That
-- is worth knowing, and it is why this cart loads its sprite from an asset.
local img

function love.load()
  love.graphics.setBackgroundColor(0.04, 0.05, 0.1)
  img = love.graphics.newImage("sprite.png")
end

function love.draw()
  -- opaque rects: the solid batch, no blending
  for i = 0, 39 do
    love.graphics.setColor(0.9, 0.4 + (i % 5) * 0.1, 0.2)
    love.graphics.rectangle("fill", 20 + (i % 10) * 60, 20 + (i // 10) * 40, 50, 30)
  end

  -- alpha rects: this is where fixed-function blending and div255 diverge,
  -- so it is the case the tolerance exists for
  for i = 0, 39 do
    love.graphics.setColor(0.2, 0.6, 1.0, 0.25 + (i % 4) * 0.25)
    love.graphics.rectangle("fill", 20 + (i % 10) * 60, 200 + (i // 10) * 40, 50, 30)
  end

  -- axis-aligned lines
  love.graphics.setColor(1, 1, 0.3)
  for i = 0, 15 do
    love.graphics.line(40, 380 + i * 4, 620, 380 + i * 4)
  end

  -- sprites: 1:1 and integer scales, the cases that sample identically
  love.graphics.setColor(1, 1, 1)
  love.graphics.draw(img, 700, 20)
  love.graphics.draw(img, 800, 20, 0, 2, 2)
  love.graphics.draw(img, 950, 20, 0, 3, 3)
  -- tinted and alpha-blended sprites
  love.graphics.setColor(1, 0.5, 0.3)
  love.graphics.draw(img, 700, 200, 0, 2, 2)
  love.graphics.setColor(1, 1, 1, 0.5)
  love.graphics.draw(img, 850, 200, 0, 2, 2)
  -- a quad (spritesheet frame)
  love.graphics.setColor(1, 1, 1)
  local q = love.graphics.newQuad(8, 8, 24, 24, 64, 64)
  love.graphics.draw(img, q, 1000, 200, 0, 3, 3)

  -- overlapping alpha, so blending compounds across draws
  for i = 0, 11 do
    love.graphics.setColor(0.9, 0.2, 0.5, 0.3)
    love.graphics.rectangle("fill", 700 + i * 18, 460, 120, 80)
  end
end
