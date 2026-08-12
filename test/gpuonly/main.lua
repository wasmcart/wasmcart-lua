-- gpuonly/main.lua - A DROP TO THE CPU RASTERIZER IS A FAILURE.
--
-- WHY THIS EXISTS: the software fallback is whole-frame, STICKY for the
-- rest of the run, and completely invisible. The picture still looks right.
-- The only symptom is that every later frame -- including the 3D ones --
-- gets several times slower, which nobody notices until they are profiling
-- something else entirely.
--
-- That is exactly how it bit us: love.graphics.ellipse was added emitting
-- up to 256 points while the GL path refused anything over 64, so any
-- ellipse with a radius past ~112px silently cost the whole run its GPU.
-- Nothing errored. No test failed. The cap was not even a GPU limit, just
-- the size of a stack array in render2d_gl.c.
--
-- So this cart draws every primitive, at sizes chosen to be BIGGER than the
-- old limits, and asserts wc.gpu2d() is still true afterwards. Any new
-- primitive belongs here on the day it is written.

local pass, fail = 0, 0
local checked = 0

-- Assert the GPU path survived whatever was just drawn. Checked after EACH
-- primitive rather than once at the end, so a failure names the culprit
-- instead of just saying "something in this frame".
local function still_gpu(what)
  checked = checked + 1
  if wc.gpu2d() then
    pass = pass + 1
  else
    fail = fail + 1
    print("GPU LOST AFTER: " .. what)
  end
end

local canvas

function love.load()
  canvas = love.graphics.newCanvas(256, 256)
end

local frame = 0

function love.draw()
  frame = frame + 1
  if frame > 1 then return end          -- one pass is enough; it is sticky

  local g = love.graphics
  g.clear(0.05, 0.05, 0.08)

  if not wc.gpu2d() then
    print("GPU LOST BEFORE ANY DRAWING (engine started on the CPU path)")
    fail = fail + 1
  end

  -- ── the primitives, at sizes that used to break ────────────────────

  g.setColor(1, 1, 1)
  g.rectangle("fill", 10, 10, 100, 60)
  still_gpu("rectangle fill")

  g.rectangle("line", 10, 80, 100, 60)
  still_gpu("rectangle line")

  g.circle("fill", 200, 60, 50)
  still_gpu("circle fill")

  -- A BIG circle. The filled-circle path has its own GL entry point and
  -- its own refusal condition, so size it past anything a small test
  -- would reach.
  g.circle("fill", 200, 60, 300)
  still_gpu("circle fill r=300")

  g.line(10, 200, 400, 260)
  still_gpu("line")

  -- Ellipses at radii that exceeded the old 64-point cap. r=400 is the
  -- case that silently killed the GPU before WCL_MAX_POLY_PTS was raised.
  g.ellipse("fill", 300, 300, 120, 60)
  still_gpu("ellipse r=120")

  g.ellipse("fill", 300, 300, 400, 200)
  still_gpu("ellipse r=400")

  g.ellipse("line", 300, 300, 400, 200)
  still_gpu("ellipse outline r=400")

  -- Arc, which shares the polygon path and had the same latent cap: a pie
  -- at r=400 came to 201 points.
  g.arc("fill", "pie", 500, 300, 400, 0, math.pi * 1.5)
  still_gpu("arc pie r=400")

  g.arc("line", "open", 500, 300, 400, 0, math.pi)
  still_gpu("arc open r=400")

  -- A CONVEX polygon, the fan path.
  g.polygon("fill", { 600, 100, 700, 120, 690, 200, 610, 190 })
  still_gpu("polygon convex")

  -- A CONCAVE polygon, the ear-clipping path. This is the one that has to
  -- work rather than refuse: love.math.triangulate exists precisely so a
  -- cart can draw these, and a refusal here would take the whole frame.
  g.polygon("fill", { 800, 100, 900, 100, 850, 150, 900, 200, 800, 200 })
  still_gpu("polygon concave")

  -- A MANY-SIDED polygon, right at the shared cap. Built as a circle so it
  -- is unambiguously simple and convex -- the point is the vertex COUNT.
  local big = {}
  for i = 0, 255 do
    local a = i / 256 * math.pi * 2
    big[#big + 1] = 1000 + math.cos(a) * 200
    big[#big + 1] = 400 + math.sin(a) * 200
  end
  g.polygon("fill", big)
  still_gpu("polygon 256 points (the cap)")

  -- Text. The glyph path drops to the CPU when it runs out of glyph
  -- textures, so draw enough distinct characters to exercise the cache.
  g.print("the quick brown fox jumps over the lazy dog 0123456789", 20, 400)
  g.print("THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG !@#$%^&*()", 20, 430)
  still_gpu("text")

  -- Canvas as a render target, then drawn as a source. setCanvas drops to
  -- the CPU if it cannot get an FBO.
  g.setCanvas(canvas)
  g.clear(0, 0, 0, 0)
  g.setColor(1, 0.5, 0)
  g.rectangle("fill", 20, 20, 100, 100)
  g.ellipse("fill", 128, 128, 100, 60)
  g.setCanvas()
  still_gpu("canvas render target")

  g.setColor(1, 1, 1)
  g.draw(canvas, 1200, 100)
  still_gpu("canvas drawn as a source")

  -- Blend modes are pipeline state; a change flushes the batch.
  g.setBlendMode("add")
  g.rectangle("fill", 1200, 400, 100, 100)
  g.setBlendMode("alpha")
  still_gpu("additive blend")

  -- Scissor.
  g.setScissor(0, 0, 640, 360)
  g.rectangle("fill", 0, 0, 200, 200)
  g.setScissor()
  still_gpu("scissor")

  -- Transforms, including the shear that was just added to apply().
  g.push()
  g.translate(100, 100)
  g.rotate(0.3)
  g.scale(1.5, 1.5)
  g.shear(0.2, 0)
  g.rectangle("fill", 0, 0, 50, 50)
  g.circle("fill", 100, 0, 30)
  g.pop()
  still_gpu("transformed draws (incl. shear)")

  print(("GPUONLY %d/%d primitives kept the GPU path"):format(pass, checked))
  print(fail == 0 and "GPUONLY OK" or "GPUONLY FAILED")
end
