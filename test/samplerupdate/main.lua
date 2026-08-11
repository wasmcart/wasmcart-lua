-- A shader sampler must see setPixel writes.
--
-- ImageData painted after creation and then sent to a shader used to sample
-- as solid white forever: shader sampler textures live in a SECOND cache
-- keyed by the pixel pointer, and wcl_r2d_forget only cleared the draw-path
-- cache. Every symptom pointed elsewhere -- the pixels were right, the UVs
-- were right, the sampler uniform was bound -- and the picture was blank.
--
-- This draws a full-screen quad through a shader that samples an ImageData
-- painted RED after allocation. A red screen means the write reached the
-- GPU; white means the stale texture is back.

local img, shader

function love.load()
  imgData = love.image.newImageData(8, 8)
  local d = imgData
  for y = 0, 7 do
    for x = 0, 7 do d:setPixel(x, y, 1, 0, 0, 1) end
  end
  img = love.graphics.newImage(d)
  print("imagedata id=" .. tostring(d.id) .. " img.id=" .. tostring(img.id) ..
        " same=" .. tostring(d.id == img.id))
  local r,g,b,a = d:getPixel(3,3)
  print(("pixel(3,3) = %.2f %.2f %.2f %.2f"):format(r,g,b,a))

  shader = love.graphics.newShader([[
    extern Image tex;
    vec4 effect(vec4 c, Image t, vec2 uv, vec2 sc) {
      return Texel(tex, uv);
    }
  ]])
end

-- Second phase: repaint the SAME ImageData to blue AFTER it has already
-- been uploaded as a sampler texture. This is the case the pointer-keyed
-- sampler cache would get wrong -- the first upload is fine either way.
local repainted = false
function love.update()
  if not repainted and frameCount and frameCount > 1 then
    repainted = true
    for y = 0, 7 do
      for x = 0, 7 do imgData:setPixel(x, y, 0, 0, 1, 1) end
    end
  end
  -- the assertion is on PIXELS, checked by the harness
  love.debugValue(0, 0)
  love.debugValue(1, 1)
end

frameCount = 0
function love.draw()
  frameCount = frameCount + 1
  love.graphics.clear(0, 0, 0, 1)
  love.graphics.setShader(shader)
  local okSend = pcall(function() shader:send("tex", img) end)
  if not _logged then _logged = true; print("send ok=" .. tostring(okSend)) end
  love.graphics.setColor(1, 1, 1)
  -- draw the IMAGE, not a bare rectangle: effect()'s uv only carries real
  -- texture coordinates for a textured draw, and a filled rect would sample
  -- the engine's own 1x1 white texture instead.
  local W, H = love.graphics.getWidth(), love.graphics.getHeight()
  love.graphics.draw(img, 0, 0, 0, W / img:getWidth(), H / img:getHeight())
  love.graphics.setShader()
end
