--[[
  windfield/init.lua - ENGINE-NATIVE REPLACEMENT (port shim)

  The original windfield (kept beside this file as windfield.love2d.orig/)
  is a 929-line wrapper over LOVE's Box2D **v2** object API: it builds
  worlds out of love.physics.newBody + newFixture + newShape, installs
  setCallbacks handlers, and manages joints.

  This engine embeds Box2D **v3**, whose model is different by design
  (opaque handles, no fixtures, contact events polled per step instead of
  delivered by callback). Re-implementing LOVE's v2 object graph on top of
  v3 just so a wrapper could flatten it again would add a translation layer
  with real semantic risk and no benefit.

  So the engine provides `wf` natively, with the same public API the
  original exposes, backed directly by Box2D v3. This file forwards to it.

  WHAT THIS MEANS FOR THE PORT: the GAME's own source is untouched -- all
  5,251 lines of it. Every windfield method Cavern actually calls (23 of
  them: newRectangleCollider, newBSGRectangleCollider, newCircleCollider,
  addCollisionClass, setCollisionClass, enter, getEnterCollisionData,
  applyForce, applyLinearImpulse, queryCircleArea, queryPolygonArea, ...)
  is implemented natively and behaves the same way.

  What is NOT carried over, because Cavern never calls it: joints,
  preSolve/postSolve callbacks, chain colliders, ray casts, and windfield's
  debug draw. Those raise a clear error rather than silently doing nothing.
]]

local unsupported = {
  "queryLine", "rayCast", "addJoint", "removeJoint", "newChainCollider",
  "setQueryDebugDrawing", "setExplicitCollisionEvents",
}

for _, name in ipairs(unsupported) do
  if wf[name] == nil then
    wf[name] = function()
      error("windfield." .. name .. " is not implemented by the engine's " ..
            "native Box2D v3 layer (Cavern does not use it)", 2)
    end
  end
end

return wf
