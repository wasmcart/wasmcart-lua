-- gl2dpoly/main.lua - convex polygon fills on the GL path.
--
-- The software fill is an even-odd scanline sampling at pixel centres, which
-- is what GPU triangle rasterization does, so interiors agree and only the
-- boundary can differ -- the same edge-coverage story as rotated sprites.
--
-- Only CONVEX polygons take the GL path. A triangle fan over a concave
-- polygon covers area the polygon does not, which is a visible error rather
-- than an edge difference, so concave fills stay on the scanline rasterizer.
-- This cart is convex-only so it measures the GL path; the concave case is
-- covered by test/prims, which falls back.

function love.draw()
  love.graphics.setBackgroundColor(0.05, 0.05, 0.1)

  -- triangles at assorted orientations
  for i = 0, 5 do
    love.graphics.setColor(0.9, 0.3 + i * 0.1, 0.2)
    local x, y = 60 + i * 110, 60
    love.graphics.polygon("fill", x, y + 80, x + 45, y, x + 90, y + 80)
  end

  -- quads, including a non-axis-aligned one
  love.graphics.setColor(0.2, 0.7, 1.0)
  love.graphics.polygon("fill", 60, 200, 200, 190, 210, 300, 70, 310)
  love.graphics.setColor(0.3, 0.9, 0.5, 0.6)
  love.graphics.polygon("fill", 250, 195, 390, 210, 370, 305, 240, 290)

  -- regular n-gons: 5, 6, 8, 12 sides
  local function ngon(cx, cy, r, sides, rot)
    local pts = {}
    for i = 0, sides - 1 do
      local a = rot + i * math.pi * 2 / sides
      pts[#pts + 1] = cx + math.cos(a) * r
      pts[#pts + 1] = cy + math.sin(a) * r
    end
    love.graphics.polygon("fill", pts)
  end
  love.graphics.setColor(1, 0.8, 0.2)
  ngon(500, 250, 60, 5, 0.3)
  love.graphics.setColor(0.8, 0.4, 1.0)
  ngon(650, 250, 60, 6, 0)
  love.graphics.setColor(0.4, 1.0, 0.8, 0.7)
  ngon(800, 250, 60, 8, 0.2)
  love.graphics.setColor(1, 0.5, 0.5)
  ngon(950, 250, 60, 12, 0)

  -- thin and degenerate-ish convex shapes, where the boundary is most of it
  love.graphics.setColor(0.9, 0.9, 0.3)
  love.graphics.polygon("fill", 60, 380, 400, 385, 400, 395, 60, 390)
  love.graphics.setColor(0.5, 0.8, 1.0)
  love.graphics.polygon("fill", 60, 420, 70, 420, 400, 470, 60, 460)

  -- overlapping translucent polygons, so blending compounds
  for i = 0, 5 do
    love.graphics.setColor(0.9, 0.2, 0.6, 0.35)
    ngon(500 + i * 45, 450, 70, 6, i * 0.2)
  end

  -- outlines, which are lines and already on the GL path
  love.graphics.setColor(1, 1, 1)
  ngon(200, 580, 70, 3, 0)
  love.graphics.setColor(0.6, 1, 0.6)
  love.graphics.polygon("line", 350, 520, 480, 530, 460, 640, 340, 620)
end
