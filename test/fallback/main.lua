-- fallback/main.lua - a cart that DELIBERATELY stays on the software path.
--
-- Used by the gl-display gate, which checks that when a frame falls back,
-- wc_gl_blit presents the software framebuffer exactly. That needs a cart
-- which actually falls back, and the set of those shrinks every time GL2D
-- gains a feature: this gate previously used test/prims, which fell back
-- because it drew circles, and silently broke the moment circles moved to
-- the GPU (it reported "the cart drew nothing", since prims then had an
-- empty software framebuffer).
--
-- A CONCAVE polygon fill is the trigger here: a triangle fan over one covers
-- area the polygon does not, so those keep the scanline rasterizer by design
-- rather than by omission. If GL2D ever gains concave fills, this cart needs
-- a new trigger and the gate will say so the same way.

function love.draw()
  love.graphics.setBackgroundColor(0.05, 0.06, 0.1)

  -- the fallback trigger: a concave (arrow) polygon
  love.graphics.setColor(0.9, 0.4, 0.2)
  love.graphics.polygon("fill", 200, 100, 300, 250, 200, 200, 100, 250)

  -- ordinary content, so the blit has a real frame to carry
  love.graphics.setColor(0.3, 0.7, 1.0)
  love.graphics.rectangle("fill", 400, 100, 200, 120)
  love.graphics.setColor(1, 0.9, 0.3, 0.6)
  love.graphics.circle("fill", 750, 160, 70)
  love.graphics.setColor(1, 1, 1)
  love.graphics.print("CPU FALLBACK FRAME", 400, 280)
  love.graphics.setColor(0.6, 1, 0.6)
  love.graphics.line(100, 350, 900, 380)
end
