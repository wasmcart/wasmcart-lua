-- prims/main.lua - the vector-primitive conformance cart.
--
-- WHY THIS EXISTS: exactly the same reason test/blit/ exists. While
-- optimizing the primitives, a control that deliberately corrupted every
-- ALPHA line pixel changed NOTHING in any of the 11 carts, and a control
-- that corrupted opaque lines moved pixels in only one. The suite could not
-- see a line regression, just as it previously could not see a blitter one.
--
-- So this cart drives every primitive through every branch the optimizer
-- touched: the opaque fast path, the alpha blend path, additive blending,
-- scissored output, and canvas destinations. Those five are the decisions
-- blend_span and the line writer hoist out of their inner loops, so each
-- combination needs at least one pixel here or the hoist is unguarded.

local cv

function love.load()
  love.graphics.setBackgroundColor(0, 0, 0)
  cv = love.graphics.newCanvas(200, 150)
end

-- Every primitive, at a given alpha, laid out in a column at x.
-- Coordinates are deliberately fractional and negative in places: the
-- clipping and rounding paths are part of the contract too.
local function suite(x, y, a)
  love.graphics.setColor(1.0, 0.35, 0.1, a)
  love.graphics.rectangle("fill", x, y, 60, 40)
  love.graphics.setColor(0.2, 0.9, 0.4, a)
  love.graphics.rectangle("line", x + 2.5, y + 45.5, 55, 30)

  love.graphics.setColor(0.3, 0.5, 1.0, a)
  love.graphics.circle("fill", x + 30, y + 105, 22)
  love.graphics.setColor(1.0, 0.9, 0.2, a)
  love.graphics.circle("line", x + 30, y + 150, 20)

  -- lines at many slopes, so Bresenham's major/minor axis choice and both
  -- step directions are all exercised, not just the diagonal
  love.graphics.setColor(0.9, 0.3, 0.8, a)
  for i = 0, 7 do
    local ang = (i / 8) * math.pi * 2
    love.graphics.line(x + 30, y + 200,
                       x + 30 + math.cos(ang) * 28,
                       y + 200 + math.sin(ang) * 28)
  end
  -- axis-aligned and single-pixel degenerate cases
  love.graphics.line(x, y + 235, x + 60, y + 235)
  love.graphics.line(x + 30, y + 240, x + 30, y + 270)
  love.graphics.line(x + 5, y + 245, x + 5, y + 245)

  love.graphics.setColor(0.1, 0.8, 0.9, a)
  love.graphics.polygon("fill", x + 5, y + 280, x + 55, y + 290,
                        x + 40, y + 325, x + 10, y + 315)
  love.graphics.setColor(1, 1, 1, a)
  love.graphics.polygon("line", x + 8, y + 335, x + 52, y + 345, x + 20, y + 370)

  love.graphics.print("Ag#01", x, y + 375)
end

function love.draw()
  -- opaque: the fast paths
  suite(20, 10, 1.0)
  -- partial alpha: the blend paths (the branch NO cart covered before)
  suite(100, 10, 0.5)
  suite(180, 10, 0.25)
  -- alpha 1/255 and 254/255, the boundaries of the div255 exactness
  -- argument. These need a lit backdrop: at a = 1/255 over black the result
  -- rounds to black and the column would prove nothing at all.
  love.graphics.setColor(0.55, 0.55, 0.55)
  love.graphics.rectangle("fill", 255, 5, 160, 400)
  suite(260, 10, 1 / 255)
  suite(340, 10, 254 / 255)

  -- Additive blending, which must not take the opaque store. Over BLACK,
  -- additive and alpha produce the same pixels, so a black backdrop here
  -- would leave the branch untested; draw onto a mid-grey field so the two
  -- modes visibly and numerically diverge.
  love.graphics.setColor(0.45, 0.3, 0.55)
  love.graphics.rectangle("fill", 415, 5, 160, 400)
  love.graphics.setBlendMode("add")
  suite(420, 10, 1.0)
  suite(500, 10, 0.5)
  love.graphics.setBlendMode("alpha")

  -- scissored, including primitives that straddle the scissor edge so the
  -- span clamp is exercised rather than a whole-primitive reject
  love.graphics.setColor(0.15, 0.25, 0.15)
  love.graphics.rectangle("fill", 585, 5, 210, 400)
  love.graphics.setScissor(600, 100, 90, 250)
  suite(590, 10, 1.0)
  love.graphics.setScissor(700, 100, 90, 250)
  suite(690, 10, 0.5)
  love.graphics.setScissor()

  -- canvas destination: the other non-fast path
  love.graphics.setCanvas(cv)
  love.graphics.clear(0.05, 0.05, 0.15)
  love.graphics.setColor(1, 0.5, 0.2)
  love.graphics.rectangle("fill", 10, 10, 50, 30)
  love.graphics.setColor(0.3, 1, 0.5, 0.5)
  love.graphics.circle("fill", 100, 60, 30)
  love.graphics.line(0, 0, 200, 150)
  love.graphics.setBlendMode("add")
  love.graphics.polygon("fill", 120, 20, 190, 40, 160, 90)
  love.graphics.setBlendMode("alpha")
  love.graphics.setCanvas()
  love.graphics.setColor(1, 1, 1)
  love.graphics.draw(cv, 800, 20)

  -- clipped hard against all four screen edges
  suite(-40, 420, 1.0)
  suite(1250, 420, 0.5)
  love.graphics.setColor(1, 1, 1, 0.5)
  love.graphics.circle("fill", 640, -10, 40)
  love.graphics.circle("fill", 640, 730, 40)
  love.graphics.rectangle("fill", -20, 690, 80, 60)
  love.graphics.line(-50, 600, 200, 640)
  love.graphics.line(1100, 600, 1400, 700)

  -- A full-width span, which is the largest single blend_span call and the
  -- one most likely to expose a bad loop bound. Confined to the empty band
  -- at the bottom: as a full-SCREEN overlay it washed over every result
  -- above and made the frame unreadable when opened.
  love.graphics.setColor(0.1, 0.15, 0.3, 0.35)
  love.graphics.rectangle("fill", 0, 660, 1280, 60)
  love.graphics.setColor(0.9, 0.9, 0.2, 0.5)
  love.graphics.rectangle("fill", -100, 670, 1480, 20)
end
