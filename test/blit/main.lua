-- blit/main.lua - the sprite-blitter conformance cart.
--
-- WHY THIS EXISTS: test/determinism.js passed for an entire debugging
-- session while the blitter was producing wrong pixels, because those carts
-- draw shapes and text, never sprites. A blitter regression was invisible to
-- the whole suite.
--
-- This cart exercises the blitter across every argument shape a real game
-- produces, so test/run.js can hash the frame and catch a change. The cases
-- are not arbitrary: they were read off a real ported game by instrumenting
-- love.graphics.draw, plus the edge cases that broke during optimization.

local img, big

function love.load()
  love.graphics.setBackgroundColor(0, 0, 0)

  -- A 64x64 source with per-pixel variation, so a one-texel sampling shift
  -- changes the output. A flat colour would hide exactly the bug this cart
  -- is here to catch.
  img = love.graphics.newCanvas(64, 64)
  love.graphics.setCanvas(img)
  love.graphics.clear(0, 0, 0)
  for y = 0, 63 do
    for x = 0, 63 do
      love.graphics.setColor(x / 63, y / 63, ((x // 4 + y // 4) % 2))
      love.graphics.rectangle("fill", x, y, 1, 1)
    end
  end
  love.graphics.setCanvas()

  -- A non-power-of-two source. This is the case that actually broke:
  -- dw = qw * sx lands on values like 54.75 or 67.5, where 1/dw is inexact
  -- and any reciprocal or incremental stepping picks a different texel.
  big = love.graphics.newCanvas(146, 146)
  love.graphics.setCanvas(big)
  love.graphics.clear(0, 0, 0)
  for y = 0, 145, 2 do
    for x = 0, 145, 2 do
      love.graphics.setColor((x % 32) / 31, (y % 32) / 31, 0.5)
      love.graphics.rectangle("fill", x, y, 2, 2)
    end
  end
  love.graphics.setCanvas()
end

function love.draw()
  love.graphics.setColor(1, 1, 1)

  -- row 1: the basics
  love.graphics.draw(img,  20,  20)                     -- 1:1
  love.graphics.draw(img, 100,  20, 0, 2, 2)            -- upscale
  love.graphics.draw(img, 240,  20, 0, 0.5, 0.5)        -- downscale
  love.graphics.draw(img, 300,  20, 0.7, 1.5, 1.5)      -- rotate + scale
  love.graphics.draw(img, 420,  20, 0, -2, 2)           -- flip X
  love.graphics.draw(img, 560,  20, 0, 2, -2)           -- flip Y
  love.graphics.draw(img, 700,  20, 0, -2, -2)          -- flip both

  -- row 2: origin offsets, which interact with flips and rotation
  love.graphics.draw(img,  80, 200, 0, 2, 2, 32, 32)
  love.graphics.draw(img, 220, 200, 0, -2, 2, 32, 32)
  love.graphics.draw(img, 360, 200, 0.6, 2, 2, 32, 32)
  love.graphics.draw(img, 500, 200, math.pi, 2, 2, 32, 32)
  love.graphics.draw(img, 640, 200, 0.6, -2, 2, 32, 32)

  -- row 3: clipped against every screen edge (the destination rect is cut,
  -- so the loop bounds and the source origin must stay consistent)
  love.graphics.draw(img, -40, 340, 0, 2, 2)
  love.graphics.draw(img, 1220, 340, 0, 2, 2)
  love.graphics.draw(img, 200, -30, 0, 2, 2)
  love.graphics.draw(img, 340, 660, 0, 2, 2)
  love.graphics.draw(img, -30, 420, 0.5, 2, 2, 32, 32)

  -- row 4: quads (spritesheet frames), including rotated and flipped
  local q = love.graphics.newQuad(8, 8, 24, 24, 64, 64)
  love.graphics.draw(img, q, 500, 340, 0, 3, 3)
  love.graphics.draw(img, q, 620, 340, 0.5, 3, 3)
  love.graphics.draw(img, q, 740, 340, 0, -3, 3)

  -- row 5: THE REGRESSION CASE. A non-power-of-two source at a fractional
  -- scale, drawn at negative coordinates -- exactly the shape that exposed
  -- the reciprocal bug (dw = 146 * 0.375 = 54.75).
  love.graphics.draw(big, -82.25, 480, 0, 0.375, 0.375)
  love.graphics.draw(big,  100,   480, 0, 0.375, 0.375)
  love.graphics.draw(big,  200,   480, 0, 0.75,  0.75)
  love.graphics.draw(big,  360,   480, 0, 0.25,  0.25)
  love.graphics.draw(big,  440,   480, 0.3, 0.375, 0.375, 73, 73)

  -- row 6: tinting and alpha, which gate the colour fast paths
  love.graphics.setColor(1, 0.5, 0.25)
  love.graphics.draw(img, 560, 480, 0, 2, 2)
  love.graphics.setColor(1, 1, 1, 0.5)
  love.graphics.draw(img, 700, 480, 0, 2, 2)
  love.graphics.setColor(0.2, 0.9, 0.4, 0.75)
  love.graphics.draw(img, 840, 480, 0.4, 2, 2, 32, 32)

  -- row 7: additive blending and scissor, the two paths that must NOT take
  -- the inlined fast destination write
  love.graphics.setColor(1, 1, 1)
  love.graphics.setBlendMode("add")
  love.graphics.draw(img, 980, 480, 0, 2, 2)
  love.graphics.draw(img, 1020, 520, 0, 2, 2)
  love.graphics.setBlendMode("alpha")

  love.graphics.setScissor(1100, 480, 100, 100)
  love.graphics.draw(img, 1080, 460, 0, 3, 3)
  love.graphics.setScissor()

  -- row 8: blitting INTO a canvas, which is the other non-fast destination
  local cv = love.graphics.newCanvas(120, 120)
  love.graphics.setCanvas(cv)
  love.graphics.clear(0.1, 0.1, 0.2)
  love.graphics.draw(img, 10, 10, 0.25, 1.5, 1.5)
  love.graphics.setCanvas()
  love.graphics.draw(cv, 900, 200)
end
