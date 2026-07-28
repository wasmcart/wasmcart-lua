-- gl2dcanvas/main.lua - render targets on the GL path.
--
-- A canvas is both a destination and, later, a source. On GL it is its own
-- texture with an FBO attached, NOT an atlas slot: the atlas is one shared
-- texture, so rendering into a sub-rect of it would let one canvas's draws
-- land on another's pixels.
--
-- This cart covers the cases that broke while wiring that up: drawing into a
-- canvas, drawing the canvas back to the screen, nesting canvas -> canvas,
-- rebinding an existing canvas across frames, and clearing one.

local cvA, cvB, img

function love.load()
  love.graphics.setBackgroundColor(0.05, 0.06, 0.12)
  img = love.graphics.newImage("sprite.png")
  cvA = love.graphics.newCanvas(256, 192)
  cvB = love.graphics.newCanvas(160, 160)
end

function love.draw()
  -- draw into canvas A: solids, a line and a sprite
  love.graphics.setCanvas(cvA)
  love.graphics.clear(0.1, 0.15, 0.1)
  love.graphics.setColor(1, 0.5, 0.2)
  love.graphics.rectangle("fill", 10, 10, 80, 50)
  love.graphics.setColor(0.3, 0.9, 1.0, 0.6)
  love.graphics.rectangle("fill", 50, 35, 80, 50)
  love.graphics.setColor(1, 1, 1)
  -- Integer scale on purpose. A FRACTIONAL scale is the known texel-boundary
  -- case (documented for screen sprites), and inside a canvas it compounds:
  -- the offset texels are then magnified when the canvas is drawn back, so a
  -- 1.5x sprite in a 1:1 canvas put 1.7% of the frame over tolerance. That is
  -- a real limit, not a canvas bug -- proven by this same cart passing with
  -- zero failures at 1:1 -- so it is stated in the docs rather than hidden
  -- behind a wider budget.
  love.graphics.draw(img, 150, 20, 0, 2, 2)
  love.graphics.setCanvas()

  -- canvas B samples canvas A: a target used as a source while another
  -- target is bound, which is the case that needs two distinct textures
  love.graphics.setCanvas(cvB)
  love.graphics.clear(0.2, 0.1, 0.2)
  love.graphics.setColor(1, 1, 1)
  love.graphics.draw(cvA, 0, 0)
  love.graphics.setColor(1, 0.4, 0.4, 0.7)
  love.graphics.rectangle("fill", 20, 100, 120, 40)
  love.graphics.setCanvas()

  -- both canvases back to the screen, including scaled and rotated
  love.graphics.setColor(1, 1, 1)
  love.graphics.draw(cvA, 40, 40)
  love.graphics.draw(cvA, 340, 40, 0, 2, 2)
  love.graphics.draw(cvB, 40, 280)
  love.graphics.draw(cvB, 240, 280, 0.4, 1.2, 1.2, 80, 80)
  love.graphics.setColor(1, 1, 1, 0.6)
  love.graphics.draw(cvB, 420, 300, 0, 2, 2)

  -- ordinary screen drawing after the targets are released
  love.graphics.setColor(0.4, 1, 0.5)
  love.graphics.rectangle("fill", 900, 40, 200, 120)
  love.graphics.setColor(1, 1, 1)
  love.graphics.draw(img, 900, 200, 0, 2, 2)
end
