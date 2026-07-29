-- shaders/main.lua - love.graphics.newShader / setShader on the GL2D backend.
--
-- The whole point of this cart is that a WRONG result is obvious. The screen
-- is split down the middle and the SAME draws are issued on both halves:
--
--   left  - no shader
--   right - the same geometry through a shader that inverts colour
--
-- If the shader ran, the two halves are photographic negatives of each
-- other. If it silently did not run (link failure, program never bound, a
-- batch drawn under the wrong program), the halves come out IDENTICAL --
-- which is the failure this layout exists to catch. "It rendered 60 frames"
-- cannot tell those apart; looking at the screenshot can.
--
-- The right half also proves the shader applies to EVERY draw path, not just
-- textured ones: rectangles (solid batch), a sprite (atlas batch), text
-- (glyph coverage batch) and a circle (the fragment-evaluated coverage rule)
-- are all drawn on both sides.

local invert, wave
local img
local t = 0

function love.load()
  love.graphics.setBackgroundColor(0.08, 0.09, 0.13)
  img = love.graphics.newImage("sprite.png")

  -- The LOVE shape: only effect() is written here. The #version line, the
  -- ins/outs and main() are synthesized by the engine.
  invert = love.graphics.newShader [[
    vec4 effect(vec4 color, Image tex, vec2 texture_coords, vec2 screen_coords) {
      vec4 px = Texel(tex, texture_coords) * color;
      return vec4(1.0 - px.rgb, px.a);
    }
  ]]

  -- A second shader, driven by uniforms sent from Lua each frame. Uniform
  -- plumbing is the half of the feature an inversion alone does not test.
  wave = love.graphics.newShader [[
    uniform float u_time;
    uniform vec3 u_tint;
    vec4 effect(vec4 color, Image tex, vec2 texture_coords, vec2 screen_coords) {
      vec4 px = Texel(tex, texture_coords) * color;
      float band = 0.5 + 0.5 * sin(screen_coords.y * 0.05 + u_time * 3.0);
      return vec4(px.rgb * mix(vec3(1.0), u_tint, band), px.a);
    }
  ]]
end

-- One block of drawing, issued twice at different x offsets so the two
-- halves are comparable pixel for pixel.
local function scene(ox)
  for i = 0, 23 do
    love.graphics.setColor(0.95, 0.35 + (i % 4) * 0.15, 0.2)
    love.graphics.rectangle("fill", ox + 30 + (i % 6) * 90, 90 + (i // 6) * 60, 76, 46)
  end

  love.graphics.setColor(0.3, 0.85, 1.0)
  love.graphics.circle("fill", ox + 150, 420, 70)

  love.graphics.setColor(1, 1, 1)
  love.graphics.draw(img, ox + 280, 360, 0, 3, 3)

  love.graphics.setColor(1, 0.9, 0.3)
  love.graphics.print("SHADED TEXT 0123", ox + 30, 560)
end

function love.draw()
  t = t + 1 / 60

  -- left: the default program
  love.graphics.setColor(1, 1, 1)
  love.graphics.print("no shader", 30, 40)
  scene(0)

  -- right: the same scene, inverted
  love.graphics.setShader(invert)
  love.graphics.print("invert shader", 670, 40)
  scene(640)
  love.graphics.setShader()

  -- a uniform-driven band across the bottom, so the send() path is exercised
  -- by something visible rather than only by a return value
  wave:send("u_time", t)
  wave:send("u_tint", { 0.2, 1.0, 0.4 })
  love.graphics.setShader(wave)
  love.graphics.setColor(0.8, 0.8, 0.9)
  love.graphics.rectangle("fill", 0, 620, 1280, 90)
  love.graphics.setShader()

  -- the divider, drawn last and unshaded so it is a fixed reference
  love.graphics.setColor(1, 1, 1)
  love.graphics.rectangle("fill", 638, 0, 4, 620)
end
