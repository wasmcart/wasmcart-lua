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
-- The trigger is a SELF-INTERSECTING polygon fill. That is not a gap waiting
-- to be closed: even-odd leaves the overlap as a hole (a pentagram's centre
-- is empty) while any triangulation of the outline fills it in, so those keep
-- the scanline rasterizer permanently.
--
-- This cart has now been re-triggered twice, and each time the gate said so
-- rather than silently passing. It first used circles, which moved to a
-- fragment shader; then a concave fill, which moved to ear clipping. Both
-- times the gate failed with "the cart drew nothing" -- correct, because its
-- premise had expired. A self-intersecting fill should not expire the same
-- way, since triangulating it would be wrong rather than merely unimplemented.

function love.draw()
  love.graphics.setBackgroundColor(0.05, 0.06, 0.1)

  -- the fallback trigger: a self-intersecting (pentagram) polygon
  love.graphics.setColor(0.9, 0.4, 0.2)
  local star = {}
  for i = 0, 4 do
    local a = -math.pi / 2 + i * 4 * math.pi / 5
    star[#star + 1] = 200 + math.cos(a) * 90
    star[#star + 1] = 170 + math.sin(a) * 90
  end
  love.graphics.polygon("fill", star)

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
