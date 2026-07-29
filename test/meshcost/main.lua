-- meshcost/main.lua - what does a mesh COST in GL calls?
--
-- A mesh cannot ride the quad batcher (that path draws (count/4)*6 indices
-- from a static quad index buffer, and a mesh is arbitrary triangles), so
-- each one is its own glDrawArrays. That is the deliberate trade, and the
-- gate exists to keep it from quietly getting worse: the failure this
-- catches is a mesh draw that starts re-binding the program, re-uploading
-- uniforms, or splitting itself across several buffer uploads.
--
-- 12 meshes, all sharing one texture. The budget in test/run.js is per-frame
-- glUseProgram = 0 (no shader is bound, so the program must never be
-- re-selected) and a bounded call total.
--
-- The meshes deliberately alternate textured and untextured, because a
-- naive implementation flushes and re-binds a texture on every switch. They
-- are also drawn interleaved with plain rectangles, so the batcher's own
-- flush behaviour around a mesh is exercised rather than assumed.

local tex
local meshes = {}

function love.load()
  tex = love.graphics.newImage("tex.png")
  for i = 1, 12 do
    local m = love.graphics.newMesh({
      {  0,  0, 0, 0, 1, 0.4, 0.3, 1 },
      { 80,  0, 1, 0, 0.3, 1, 0.5, 1 },
      { 80, 80, 1, 1, 0.4, 0.5, 1, 1 },
      {  0, 80, 0, 1, 1, 1, 1, 1 },
    }, "fan")
    if i % 2 == 0 then m:setTexture(tex) end
    meshes[i] = m
  end
end

function love.draw()
  love.graphics.setBackgroundColor(0.05, 0.06, 0.1)
  for i = 1, 12 do
    local x = 40 + ((i - 1) % 6) * 190
    local y = 60 + math.floor((i - 1) / 6) * 200
    -- a rectangle next to each mesh: the solid batch has to survive being
    -- interrupted by a mesh draw and keep its ordering
    love.graphics.setColor(0.2, 0.3, 0.5)
    love.graphics.rectangle("fill", x, y + 100, 80, 60)
    love.graphics.setColor(1, 1, 1)
    love.graphics.draw(meshes[i], x, y)
  end
end
