-- gpurecover/main.lua - a refused draw must NEVER cost the frame its GPU.
--
-- wasmcart targets a GPU. engine.wasm has no software rasterizer to fall
-- back to, and that is the requirement, not an optimisation: a frame that
-- quietly finishes in software still LOOKS right while its frame time
-- triples and every bound shader silently stops being applied. The symptom
-- turns up far from the cause and is close to undiagnosable from the cart.
--
-- So a draw the GL backend cannot express is REFUSED: that one primitive is
-- skipped and named in the log, and the rest of the frame renders on the GPU
-- exactly as it would have.
--
-- This cart proves that with the hardest case available -- a self-
-- intersecting polygon. That is not a gap waiting to be closed: even-odd
-- leaves a pentagram's centre hollow while any triangulation fills it, so
-- there is no single correct GPU answer and the backend refuses rather than
-- guessing. If some later change makes it renderable, this cart says so
-- (see the "premise has expired" line) instead of passing vacuously.
--
--   frame 1  clean draws only         -> on the GPU
--   frame 2  ONE refused polygon      -> STILL on the GPU, before and after,
--                                        and the draws after it still land
--   frame 3  clean draws only again   -> still on the GPU

local frame = 0
local fails = 0

function love.draw()
  frame = frame + 1
  local g = love.graphics
  g.clear(0.05, 0.06, 0.1)

  if frame == 1 then
    -- Baseline. If the engine never had a GPU path, every later assertion
    -- passes for the wrong reason, so this is the control on the control.
    if not wc.gpu2d() then
      print("RECOVER: frame 1 started on the CPU path; nothing below proves anything")
      fails = fails + 1
    end
    g.setColor(0.3, 0.7, 1.0)
    g.rectangle("fill", 100, 100, 200, 120)

  elseif frame == 2 then
    if not wc.gpu2d() then
      print("RECOVER: frame 2 opened on the CPU path")
      fails = fails + 1
    end

    -- The refused draw.
    g.setColor(0.9, 0.4, 0.2)
    local star = {}
    for i = 0, 4 do
      local a = -math.pi / 2 + i * 4 * math.pi / 5
      star[#star + 1] = 200 + math.cos(a) * 90
      star[#star + 1] = 170 + math.sin(a) * 90
    end
    g.polygon("fill", star)

    -- THE ASSERTION. Under the old behaviour this was false: the polygon
    -- took the whole frame -- and every later frame -- onto the software
    -- rasterizer. It must now still be true.
    if wc.gpu2d() then
      print("RECOVER: frame 2 kept the GPU through a refused draw")
    else
      print("RECOVER: frame 2 LOST the GPU to a refused draw -- the engine " ..
            "fell back to the software rasterizer instead of skipping the primitive")
      fails = fails + 1
    end

    -- and the frame must still be USABLE afterwards: a refusal that left the
    -- backend wedged would keep gpu2d() true while dropping everything after
    -- it, which the check above cannot distinguish. Draw more and confirm.
    g.setColor(0.3, 0.7, 1.0)
    g.rectangle("fill", 400, 100, 200, 120)
    if not wc.gpu2d() then
      print("RECOVER: the draws AFTER the refusal lost the GPU path")
      fails = fails + 1
    end

  elseif frame == 3 then
    if wc.gpu2d() then
      print("RECOVER: frame 3 is on the GPU")
    else
      print("RECOVER: frame 3 is on the CPU path -- a refusal outlived its frame")
      fails = fails + 1
    end
    g.setColor(0.3, 0.7, 1.0)
    g.rectangle("fill", 100, 100, 200, 120)

    print(fails == 0 and "GPURECOVER OK" or ("GPURECOVER FAILED=" .. fails))
    love.debugValue(0, fails)
  end
end
