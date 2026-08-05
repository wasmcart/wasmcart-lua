-- The cart asks for a non-default resolution, LOVE's way. The harness
-- asserts the engine honored it (info struct AND Lua-visible dims) and that
-- the framebuffer stride matches: the corner marker below only lands in the
-- last row/column of a 1600x900 frame if every scanline is 1600 pixels wide.
function love.conf(t)
  t.window.width = 1600
  t.window.height = 900
end
