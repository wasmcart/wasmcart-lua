-- gl2dtext/main.lua - text on the GL path.
--
-- stb_truetype bakes every glyph into one 8-bit coverage bitmap at load
-- time, so TTF does NOT need a CPU rasterizer: the bitmap uploads once as a
-- single-channel texture, swizzled so R feeds alpha, and each glyph is a
-- quad in the shared textured batch. Coverage then modulates alpha exactly
-- as the software path's blend_px(cov * a / 255) does.
--
-- Bitfont text draws its pixels as fill_rects, which already batch.

local font

function love.load()
  love.graphics.setBackgroundColor(0.06, 0.07, 0.14)
  font = love.graphics.newFont("VT323-Regular.ttf", 32)
end

function love.draw()
  -- bitfont at several scales
  love.graphics.setColor(1, 1, 1)
  love.graphics.print("BITFONT 1x  ABCdef 0123 !?#", 40, 40)
  love.graphics.setColor(0.4, 1, 0.6)
  love.graphics.print("BITFONT tinted", 40, 70)
  love.graphics.setColor(1, 0.8, 0.3, 0.5)
  love.graphics.print("BITFONT alpha", 40, 100)

  -- TTF, the path that used to force the whole frame onto the CPU
  love.graphics.setFont(font)
  love.graphics.setColor(1, 1, 1)
  love.graphics.print("TTF: the quick brown fox", 40, 160)
  love.graphics.setColor(0.5, 0.8, 1)
  love.graphics.print("TTF tinted 0123456789", 40, 210)
  love.graphics.setColor(1, 0.5, 0.5, 0.6)
  love.graphics.print("TTF with alpha", 40, 260)

  -- text over a filled rect, so glyph coverage blends against real content
  love.graphics.setColor(0.2, 0.3, 0.5)
  love.graphics.rectangle("fill", 40, 320, 600, 80)
  love.graphics.setColor(1, 1, 0.4)
  love.graphics.print("TTF over a rect", 60, 345)

  -- interleaved text and sprites, to prove batches flush in the right order
  love.graphics.setColor(1, 1, 1)
  for i = 0, 5 do
    love.graphics.rectangle("fill", 700 + i * 60, 60, 40, 40)
    love.graphics.print("x" .. i, 700 + i * 60, 110)
  end
end
