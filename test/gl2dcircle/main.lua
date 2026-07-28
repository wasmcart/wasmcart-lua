-- gl2dcircle/main.lua - circles on the GL path.
--
-- Filled circles are triangle fans. The software fill is
-- span = round(sqrt(r*r - yy*yy)) per row -- an exact circle, not a polygon
-- -- so a fan can never match pixel for pixel. Measured, the difference is
-- ~0.5 px per BOUNDARY pixel and flat from r=4 to r=100, which makes it edge
-- coverage rather than a shape error.
--
-- That same 0.5 px/edge is 1% of the area at r=100 but 23% at r=4, so small
-- radii keep the exact span fill and drop the frame to software. This cart
-- stays above that threshold; test/prims covers the small-circle fallback.
--
-- Outlines write single pixels. On GL each is a 1x1 quad in the solid batch,
-- because blend_span writes straight to the framebuffer and would be
-- invisible on a GL frame.

function love.draw()
  love.graphics.setBackgroundColor(0.04, 0.05, 0.09)

  -- filled, a range of radii above the threshold
  local r = 26
  for i = 0, 6 do
    love.graphics.setColor(0.9, 0.3 + i * 0.09, 0.25)
    love.graphics.circle("fill", 80 + i * 140, 90, r + i * 8)
  end

  -- translucent and overlapping, so blending compounds over curved edges
  for i = 0, 7 do
    love.graphics.setColor(0.2, 0.7, 1.0, 0.3)
    love.graphics.circle("fill", 140 + i * 70, 250, 60)
  end

  -- large circles, where the fan is closest to exact
  love.graphics.setColor(0.4, 1.0, 0.6)
  love.graphics.circle("fill", 200, 460, 110)
  love.graphics.setColor(1.0, 0.8, 0.2, 0.7)
  love.graphics.circle("fill", 420, 460, 90)

  -- outlines at several radii, including small ones (they batch as 1x1 quads
  -- and do not use the fan at all)
  love.graphics.setColor(1, 1, 1)
  for i = 0, 8 do
    love.graphics.circle("line", 640 + i * 66, 460, 8 + i * 6)
  end

  -- outline over a fill, so the two paths have to agree on placement
  love.graphics.setColor(0.8, 0.3, 0.9)
  love.graphics.circle("fill", 900, 200, 80)
  love.graphics.setColor(1, 1, 0.4)
  love.graphics.circle("line", 900, 200, 80)
  love.graphics.circle("line", 900, 200, 60)
end
