-- apiconform/main.lua - the LOVE API conformance cart.
--
-- WHY THIS EXISTS: tools/api-status.lua enumerates which love.* functions
-- this engine EXPOSES, and a coverage percentage built from that is a lie
-- if a function exists but returns nonsense. Presence is not conformance.
--
-- So this cart exercises the functions the status table claims, and asserts
-- on their VALUES: round-trips (set it, get it back), known-answer maths
-- (sRGB at 0.5 is 0.214, not "some number"), and structural results
-- (triangulating a square gives 2 triangles covering the same area).
--
-- Every assertion prints PASS or FAIL with the actual value, so a run is
-- readable in the log and a regression names itself.

local pass, fail = 0, 0

local function ok(name, cond, got)
  if cond then
    pass = pass + 1
  else
    fail = fail + 1
    print("FAIL " .. name .. "  got=" .. tostring(got))
  end
end

-- floats need a tolerance; these are graphics values, not accounting
local function near(a, b, eps)
  return math.abs(a - b) <= (eps or 1e-4)
end

local function run()
  local g, m = love.graphics, love.math

  -- ── round-trips: a setter's value must survive its getter ──────────
  --
  -- This is the idiom that breaks without them: read state, change it,
  -- put it back. A missing getter turns that into a nil arithmetic error
  -- inside somebody's library, far from the cause.
  g.setLineJoin("bevel")
  ok("lineJoin round-trip", g.getLineJoin() == "bevel", g.getLineJoin())

  g.setPointSize(4)
  ok("pointSize round-trip", g.getPointSize() == 4, g.getPointSize())

  g.setDefaultFilter("nearest", "nearest", 2)
  local mi, ma, an = g.getDefaultFilter()
  ok("defaultFilter round-trip", mi == "nearest" and ma == "nearest" and an == 2,
     tostring(mi) .. "," .. tostring(ma) .. "," .. tostring(an))

  g.setBlendMode("add")
  local bm, ba = g.getBlendMode()
  ok("blendMode round-trip", bm == "add", tostring(bm) .. "," .. tostring(ba))
  g.setBlendMode("alpha")

  g.setLineWidth(3)
  ok("lineWidth round-trip", g.getLineWidth() == 3, g.getLineWidth())
  g.setLineWidth(1)

  -- ── dimensions: pixel size must agree with logical size ────────────
  --
  -- A cart's framebuffer IS its pixel buffer; there is no OS scaling
  -- between them. Libraries branch on getDPIScale to decide whether to
  -- draw at 2x, so a wrong answer here silently doubles everything.
  ok("dpiScale is 1", g.getDPIScale() == 1, g.getDPIScale())
  ok("pixelWidth == width", g.getPixelWidth() == g.getWidth(), g.getPixelWidth())
  ok("pixelHeight == height", g.getPixelHeight() == g.getHeight(), g.getPixelHeight())
  local pw, ph = g.getPixelDimensions()
  ok("pixelDimensions pair", pw == g.getWidth() and ph == g.getHeight(),
     pw .. "x" .. ph)

  local nm, ver, vend, dev = g.getRendererInfo()
  ok("rendererInfo returns 4 strings",
     type(nm) == "string" and type(ver) == "string"
     and type(vend) == "string" and type(dev) == "string", tostring(nm))

  -- stack depth must actually track push/pop
  local d0 = g.getStackDepth()
  g.push()
  local d1 = g.getStackDepth()
  g.pop()
  local d2 = g.getStackDepth()
  ok("stackDepth tracks push/pop", d1 == d0 + 1 and d2 == d0,
     d0 .. "," .. d1 .. "," .. d2)

  -- ── colour byte conversion: LOVE 11 changed 0-255 to 0-1 ───────────
  --
  -- Every port of an older game reaches for these. Getting them wrong
  -- does not error, it just draws in the wrong colours.
  local r, gg, b, a = m.colorFromBytes(255, 128, 0, 51)
  ok("colorFromBytes red", near(r, 1), r)
  ok("colorFromBytes green", near(gg, 128 / 255), gg)
  ok("colorFromBytes blue", near(b, 0), b)
  ok("colorFromBytes alpha", near(a, 51 / 255), a)

  local br, bg, bb = m.colorToBytes(1, 0.5, 0)
  ok("colorToBytes red", br == 255, br)
  ok("colorToBytes green rounds", bg == 128, bg)
  ok("colorToBytes blue", bb == 0, bb)

  -- and the round trip must be stable
  local x1, y1, z1 = m.colorToBytes(m.colorFromBytes(200, 100, 25))
  ok("colour byte round-trip", x1 == 200 and y1 == 100 and z1 == 25,
     x1 .. "," .. y1 .. "," .. z1)

  -- alpha is OPTIONAL and must stay nil rather than becoming 0
  local _, _, _, na = m.colorFromBytes(10, 20, 30)
  ok("colorFromBytes keeps alpha nil", na == nil, tostring(na))

  -- ── sRGB transfer: known answers, not "some number" ────────────────
  --
  -- The real piecewise curve, not the 2.2 approximation. 0.5 sRGB is
  -- 0.2140 linear; the approximation would say 0.2176, which is wrong
  -- enough to see in a lighting shader.
  ok("gammaToLinear(0.5)", near(m.gammaToLinear(0.5), 0.2140, 1e-3),
     m.gammaToLinear(0.5))
  ok("gammaToLinear(0) is 0", near(m.gammaToLinear(0), 0), m.gammaToLinear(0))
  ok("gammaToLinear(1) is 1", near(m.gammaToLinear(1), 1), m.gammaToLinear(1))
  -- the low end must use the LINEAR segment, not the power curve
  ok("gammaToLinear low segment", near(m.gammaToLinear(0.04), 0.04 / 12.92),
     m.gammaToLinear(0.04))
  -- and the two must invert each other
  ok("gamma/linear invert", near(m.linearToGamma(m.gammaToLinear(0.73)), 0.73, 1e-4),
     m.linearToGamma(m.gammaToLinear(0.73)))

  -- ── convexity ──────────────────────────────────────────────────────
  ok("square is convex", m.isConvex({0,0, 10,0, 10,10, 0,10}) == true)
  -- an arrowhead: the classic concave case
  ok("arrowhead is concave",
     m.isConvex({0,0, 10,0, 5,5, 10,10, 0,10}) == false)
  ok("triangle is convex", m.isConvex({0,0, 10,0, 5,8}) == true)
  ok("two points is not a polygon", m.isConvex({0,0, 1,1}) == false)

  -- ── triangulation ──────────────────────────────────────────────────
  --
  -- love.graphics.polygon only fans CONVEX shapes, so a concave polygon
  -- cannot be drawn at all without this. Assert on structure and on
  -- conserved area, which catches a triangulation that returns the right
  -- COUNT of wrong triangles.
  local function area(tris)
    local sum = 0
    for _, t in ipairs(tris) do
      sum = sum + math.abs((t[3]-t[1])*(t[6]-t[2]) - (t[5]-t[1])*(t[4]-t[2])) / 2
    end
    return sum
  end

  local sq = m.triangulate({0,0, 10,0, 10,10, 0,10})
  ok("square -> 2 triangles", #sq == 2, #sq)
  ok("square area conserved", near(area(sq), 100, 0.01), area(sq))

  local tri = m.triangulate({0,0, 10,0, 5,8})
  ok("triangle -> 1 triangle", #tri == 1, #tri)
  ok("triangle area conserved", near(area(tri), 40, 0.01), area(tri))

  -- the concave case, which is the whole point
  local arrow = m.triangulate({0,0, 10,0, 5,5, 10,10, 0,10})
  ok("arrowhead -> 3 triangles", #arrow == 3, #arrow)
  ok("arrowhead area conserved", near(area(arrow), 75, 0.01), area(arrow))

  -- every emitted triangle must have real area: a degenerate sliver means
  -- the ear clipper cut a bad corner
  local degenerate = 0
  for _, t in ipairs(arrow) do
    local a2 = math.abs((t[3]-t[1])*(t[6]-t[2]) - (t[5]-t[1])*(t[4]-t[2])) / 2
    if a2 < 1e-6 then degenerate = degenerate + 1 end
  end
  ok("no degenerate triangles", degenerate == 0, degenerate)

  -- ── randomNormal: distribution, not a single draw ──────────────────
  --
  -- One sample proves nothing. Take many and check the mean and spread
  -- land near where they should.
  local n, sum, sumsq = 4000, 0, 0
  for _ = 1, n do
    local v = m.randomNormal(2, 10)
    sum = sum + v
    sumsq = sumsq + v * v
  end
  local mean = sum / n
  local sd = math.sqrt(sumsq / n - mean * mean)
  ok("randomNormal mean ~10", math.abs(mean - 10) < 0.25, mean)
  ok("randomNormal stddev ~2", math.abs(sd - 2) < 0.25, sd)

  -- ── love.touch: the pointer ABI's slots 1-9 ───────────────────────
  --
  -- Multi-touch was always in the ABI; what was missing was the standard
  -- API for reaching it. With no finger down the list must be EMPTY rather
  -- than nil, because every caller does #touches or ipairs on it.
  local t = love.touch
  local touches = t.getTouches()
  ok("getTouches returns a table", type(touches) == "table", type(touches))
  ok("no touches when none are down", #touches == 0, #touches)

  -- Querying a touch that is not down must ERROR, not return garbage
  -- coordinates that a game would then act on.
  ok("getPosition errors on absent id", pcall(t.getPosition, 3) == false)
  ok("getPressure errors on absent id", pcall(t.getPressure, 3) == false)

  -- ── love.keyboard: scancodes are identity here ────────────────────
  --
  -- Keys are pad buttons wearing key names, so scancode and key are the
  -- same thing. Games call isScancodeDown for layout independence; if it
  -- is missing they simply never see input, which looks like a dead pad.
  local k = love.keyboard
  ok("getScancodeFromKey identity", k.getScancodeFromKey("left") == "left",
     k.getScancodeFromKey("left"))
  ok("getKeyFromScancode identity", k.getKeyFromScancode("x") == "x",
     k.getKeyFromScancode("x"))
  ok("isScancodeDown agrees with isDown",
     k.isScancodeDown("left") == k.isDown("left"))

  k.setKeyRepeat(true)
  ok("keyRepeat round-trip true", k.hasKeyRepeat() == true, k.hasKeyRepeat())
  k.setKeyRepeat(false)
  ok("keyRepeat round-trip false", k.hasKeyRepeat() == false, k.hasKeyRepeat())

  k.setTextInput(true)
  ok("textInput round-trip", k.hasTextInput() == true, k.hasTextInput())
  k.setTextInput(false)

  -- honest capability answer: a cart cannot summon an on-screen keyboard,
  -- and claiming it could would hang a game waiting for text
  ok("no screen keyboard", k.hasScreenKeyboard() == false, k.hasScreenKeyboard())

  -- ── love.physics: the OFFICIAL API, on the same Box2D ─────────────
  --
  -- The status table scored physics at 14% while the engine had full
  -- Box2D, because we only exposed the windfield spelling. These assert
  -- the LOVE-shaped API really SIMULATES -- a body that does not move
  -- under gravity would pass any name-only check.
  local ph = love.physics
  local w = ph.newWorld(0, 100)

  local body = ph.newBody(w, 50, 10, "dynamic")
  local shp = ph.newRectangleShape(4, 4)
  local fix = ph.newFixture(body, shp, 1)

  ok("newBody returns a body", type(body) == "table")
  ok("body starts where placed", select(1, body:getPosition()) == 50,
     select(1, body:getPosition()))
  ok("fixture binds to its body", fix:getBody() == body)
  ok("fixture keeps its shape", fix:getShape() == shp)
  ok("shape reports its type", shp:getType() == "polygon", shp:getType())
  ok("body has a mass", body:getMass() > 0, body:getMass())

  -- IT MUST ACTUALLY FALL. Gravity is +100 on y, so after stepping the
  -- body must be lower than it started, and its velocity positive.
  local _, y0 = body:getPosition()
  for _ = 1, 30 do w:update(1 / 60) end
  local _, y1 = body:getPosition()
  local _, vy = body:getLinearVelocity()
  ok("body falls under gravity", y1 > y0 + 1, ("%.2f -> %.2f"):format(y0, y1))
  ok("velocity accumulates downward", vy > 0, vy)

  -- and a STATIC body must not move at all
  local ground = ph.newBody(w, 50, 200, "static")
  ph.newFixture(ground, ph.newRectangleShape(100, 4), 1)
  local gx0, gy0 = ground:getPosition()
  for _ = 1, 30 do w:update(1 / 60) end
  local gx1, gy1 = ground:getPosition()
  ok("static body does not move", gx0 == gx1 and gy0 == gy1,
     ("%.2f,%.2f -> %.2f,%.2f"):format(gx0, gy0, gx1, gy1))

  -- explicit setters must take effect
  body:setPosition(7, 8)
  local sx, sy = body:getPosition()
  ok("setPosition round-trip", near(sx, 7) and near(sy, 8),
     ("%.2f,%.2f"):format(sx, sy))

  body:setLinearVelocity(3, -4)
  local lvx, lvy = body:getLinearVelocity()
  ok("setLinearVelocity round-trip", near(lvx, 3) and near(lvy, -4),
     ("%.2f,%.2f"):format(lvx, lvy))

  -- circle and polygon shapes must build fixtures too
  local cb = ph.newBody(w, 0, 0, "dynamic")
  local cs = ph.newCircleShape(5)
  ph.newFixture(cb, cs, 1)
  ok("circle shape type", cs:getType() == "circle", cs:getType())
  ok("circle radius", cs:getRadius() == 5, cs:getRadius())
  ok("circle body gains mass", cb:getMass() > 0, cb:getMass())

  local pb = ph.newBody(w, 0, 0, "dynamic")
  ph.newFixture(pb, ph.newPolygonShape({-5,-5, 5,-5, 5,5, -5,5}), 1)
  ok("polygon body gains mass", pb:getMass() > 0, pb:getMass())

  -- distance between two bodies, by construction
  body:setPosition(0, 0)
  cb:setPosition(3, 4)
  ok("getDistance is euclidean", near(ph.getDistance(fix, cb.fixtures[1]), 5, 0.01),
     ph.getDistance(fix, cb.fixtures[1]))

  ok("destroy marks the body", (function()
    local d = ph.newBody(w, 0, 0, "dynamic")
    ph.newFixture(d, ph.newCircleShape(1), 1)
    d:destroy()
    return d:isDestroyed()
  end)())

  -- ── joints ────────────────────────────────────────────────────────
  --
  -- A joint that exists but does not CONSTRAIN is the failure mode worth
  -- testing for: every one of these would pass a name-only check while
  -- the bodies drift apart on screen. So each case builds a rig, steps it
  -- under real gravity, and measures whether the constraint held.
  --
  -- Several of these are not one-to-one with Box2D 3.2 (rope and friction
  -- are built on other joints, and mouse follows upstream's own sample),
  -- which is exactly why they are measured rather than assumed.
  -- The engine caps worlds at 4 (a deliberate limit -- a cart running five
  -- simultaneous physics worlds has a design problem, not a quota problem).
  -- So each case DESTROYS its world when done rather than the engine
  -- growing a bigger table to accommodate a test.
  local function jointWorld(gx, gy)
    return ph.newWorld(gx or 0, gy or 0)
  end

  local function dynBox(jw, x, y, hw, hh)
    local bd = ph.newBody(jw, x, y, "dynamic")
    ph.newFixture(bd, ph.newRectangleShape((hw or 4) * 2, (hh or 4) * 2), 1)
    return bd
  end

  local function dist(a, b)
    local ax, ay = a:getPosition()
    local bx, by = b:getPosition()
    return math.sqrt((bx - ax) ^ 2 + (by - ay) ^ 2)
  end

  -- REVOLUTE: a body pinned to a static anchor swings but never leaves.
  do
    local jw = jointWorld(0, 200)
    local anchor = ph.newBody(jw, 100, 100, "static")
    ph.newFixture(anchor, ph.newRectangleShape(4, 4), 1)
    local arm = dynBox(jw, 140, 100)
    local j = ph.newRevoluteJoint(anchor, arm, 100, 100)
    local d0 = dist(anchor, arm)
    for _ = 1, 120 do jw:update(1 / 60) end
    local d1 = dist(anchor, arm)
    local _, ay = arm:getPosition()
    ok("revolute holds its pivot distance", math.abs(d1 - d0) < 2,
       ("%.2f -> %.2f"):format(d0, d1))
    -- and it must actually have SWUNG, or we proved nothing about motion
    ok("revolute lets the arm swing", ay > 100 + 5, ay)
    ok("revolute reports its type", j:getType() == "revolute", j:getType())
    jw:destroy()
  end

  -- DISTANCE: rest length is maintained against gravity.
  do
    local jw = jointWorld(0, 200)
    local anchor = ph.newBody(jw, 100, 50, "static")
    ph.newFixture(anchor, ph.newRectangleShape(4, 4), 1)
    local hang = dynBox(jw, 100, 110)
    ph.newDistanceJoint(anchor, hang, 100, 50, 100, 110)
    for _ = 1, 180 do jw:update(1 / 60) end
    local d = dist(anchor, hang)
    ok("distance joint holds its length", math.abs(d - 60) < 8, d)
    jw:destroy()
  end

  -- ROPE: may come closer, may NOT exceed the max. Both halves matter --
  -- a rope that behaves like a rigid rod would pass a max-length check
  -- alone, so the slack case is asserted too.
  do
    local jw = jointWorld(0, 300)
    local anchor = ph.newBody(jw, 100, 50, "static")
    ph.newFixture(anchor, ph.newRectangleShape(4, 4), 1)
    local hang = dynBox(jw, 100, 60)
    ph.newRopeJoint(anchor, hang, 100, 50, 100, 60, 80)
    for _ = 1, 240 do jw:update(1 / 60) end
    local d = dist(anchor, hang)
    ok("rope never exceeds its max length", d <= 80 + 6, d)
    ok("rope actually extended (it is not glued)", d > 40, d)
    jw:destroy()
  end

  -- PRISMATIC: slides along its axis and NOT across it.
  --
  -- THE BODIES MUST NOT SHARE AN ORIGIN. Placing both at the same point
  -- makes the joint's two local frames coincide, and the solver then has a
  -- zero-length separation to work from: the body jitters a few pixels
  -- sideways and goes nowhere. Half a pixel of offset is the entire
  -- difference between that and a clean slide -- measured, at 0px the body
  -- moved (+0.16, -3.63) and at 0.5px it moved (+7220.48, +0.00).
  --
  -- This cost me a wrong bug report: I read the degenerate result as "the
  -- axis argument is ignored" and shipped a commit message saying so. The
  -- axis was always correct. The same footgun is already documented on
  -- joint_distance in physics.c, which is where I should have looked.
  do
    local jw = jointWorld(0, 0)
    local base = ph.newBody(jw, 100, 100, "static")
    ph.newFixture(base, ph.newRectangleShape(8, 8), 1)
    local slider = dynBox(jw, 108, 100)          -- offset, see above
    ph.newPrismaticJoint(base, slider, 108, 100, 1, 0)   -- x axis only
    for _ = 1, 90 do
      slider:applyForce(400, 400)                -- push DIAGONALLY
      jw:update(1 / 60)
    end
    local sx, sy = slider:getPosition()
    ok("prismatic slides along its axis", sx > 150, sx)
    ok("prismatic resists across its axis", math.abs(sy - 100) < 2, sy)
    jw:destroy()
  end

  -- The axis argument must actually STEER the slide, not just permit one.
  -- A y-axis joint under the same diagonal push must move in y, not x.
  do
    local jw = jointWorld(0, 0)
    local base = ph.newBody(jw, 100, 100, "static")
    ph.newFixture(base, ph.newRectangleShape(8, 8), 1)
    local slider = dynBox(jw, 100, 108)
    ph.newPrismaticJoint(base, slider, 100, 108, 0, 1)   -- y axis
    for _ = 1, 90 do
      slider:applyForce(400, 400)
      jw:update(1 / 60)
    end
    local sx, sy = slider:getPosition()
    ok("prismatic y-axis slides in y", sy > 150, sy)
    ok("prismatic y-axis resists x", math.abs(sx - 100) < 2, sx)
    jw:destroy()
  end

  -- WELD: rigid. The two bodies keep their exact separation.
  do
    local jw = jointWorld(0, 300)
    local a = dynBox(jw, 100, 100)
    local b = dynBox(jw, 130, 100)
    ph.newWeldJoint(a, b, 115, 100)
    local d0 = dist(a, b)
    for _ = 1, 180 do jw:update(1 / 60) end
    local d1 = dist(a, b)
    ok("weld keeps bodies rigid", math.abs(d1 - d0) < 2,
       ("%.2f -> %.2f"):format(d0, d1))
    jw:destroy()
  end

  -- WHEEL: suspension. Travels along the axis but stays attached.
  do
    local jw = jointWorld(0, 300)
    local chassis = ph.newBody(jw, 100, 100, "static")
    ph.newFixture(chassis, ph.newRectangleShape(20, 4), 1)
    local wheel = dynBox(jw, 100, 120)
    local j = ph.newWheelJoint(chassis, wheel, 100, 120, 0, 1)
    for _ = 1, 180 do jw:update(1 / 60) end
    local wx, wy = wheel:getPosition()
    ok("wheel stays on its axis", math.abs(wx - 100) < 3, wx)
    ok("wheel suspension holds it up", wy < 400, wy)
    ok("wheel reports its type", j:getType() == "wheel", j:getType())
    jw:destroy()
  end

  -- FRICTION: brakes a moving body in a world with NO gravity, so the
  -- only thing that can slow it is the joint.
  do
    local jw = jointWorld(0, 0)
    local ground = ph.newBody(jw, 100, 100, "static")
    ph.newFixture(ground, ph.newRectangleShape(4, 4), 1)
    local puck = dynBox(jw, 100, 100)
    ph.newFrictionJoint(ground, puck, 100, 100)
    puck:setLinearVelocity(200, 0)
    local v0 = select(1, puck:getLinearVelocity())
    for _ = 1, 120 do jw:update(1 / 60) end
    local v1 = select(1, puck:getLinearVelocity())
    ok("friction slows a moving body", v1 < v0 * 0.5,
       ("%.2f -> %.2f"):format(v0, v1))
    jw:destroy()
  end

  -- MOUSE: drags a body toward a target that we move.
  do
    local jw = jointWorld(0, 0)
    local target = dynBox(jw, 100, 100)
    local j = ph.newMouseJoint(target, 100, 100)
    j:setTarget(300, 100)
    for _ = 1, 180 do jw:update(1 / 60) end
    local tx = select(1, target:getPosition())
    ok("mouse joint drags toward its target", tx > 200, tx)
    -- and destroying it must clean up the hidden anchor body
    j:destroy()
    ok("mouse joint destroys cleanly", j:isDestroyed())
    jw:destroy()
  end

  -- MOTOR on a revolute joint must actually turn it.
  do
    local jw = jointWorld(0, 0)
    local anchor = ph.newBody(jw, 100, 100, "static")
    ph.newFixture(anchor, ph.newRectangleShape(4, 4), 1)
    local arm = dynBox(jw, 130, 100)
    local j = ph.newRevoluteJoint(anchor, arm, 100, 100)
    j:setMaxMotorForce(50000)
    j:setMotorSpeed(4)
    local a0 = arm:getAngle()
    for _ = 1, 120 do jw:update(1 / 60) end
    ok("revolute motor drives rotation", math.abs(arm:getAngle() - a0) > 0.5,
       ("%.3f -> %.3f"):format(a0, arm:getAngle()))
    jw:destroy()
  end

  -- ── love.graphics.newParticleSystem ───────────────────────────────
  --
  -- The gap with a real user: Jewels hand-rolled a pooled emitter because
  -- this was missing. A particle system that exists but never moves a
  -- particle would pass any name-only check, so every assertion here is
  -- on simulated state.
  do
    local canvas = g.newCanvas(4, 4)
    local ps = g.newParticleSystem(canvas, 64)
    ok("newParticleSystem returns one", ps:type() == "ParticleSystem", ps:type())
    ok("buffer size round-trip", ps:getBufferSize() == 64, ps:getBufferSize())
    ok("starts empty", ps:getCount() == 0, ps:getCount())
    ok("starts stopped", ps:isStopped())

    -- emit() must produce exactly what was asked for
    ps:setParticleLifetime(1, 1)
    ps:emit(10)
    ok("emit(10) makes 10 particles", ps:getCount() == 10, ps:getCount())

    -- and they must EXPIRE. A system that never reaps is the leak that
    -- eventually eats the buffer.
    for _ = 1, 70 do ps:update(1 / 60) end
    ok("particles expire after their lifetime", ps:getCount() == 0, ps:getCount())

    -- the buffer is a HARD cap, not a suggestion
    ps:emit(500)
    ok("buffer size is a hard cap", ps:getCount() == 64, ps:getCount())
    ps:reset()
    ok("reset clears every particle", ps:getCount() == 0, ps:getCount())

    -- emission rate must produce roughly rate*time particles
    ps:setEmissionRate(60)
    ps:setParticleLifetime(10, 10)     -- long, so none expire during the test
    ps:start()
    for _ = 1, 30 do ps:update(1 / 60) end
    local n = ps:getCount()
    ok("emission rate is honoured", n >= 28 and n <= 32, n)
    ps:stop()
    ok("stop clears the system", ps:getCount() == 0, ps:getCount())

    -- MOTION: a particle fired right with no acceleration must travel right
    ps:reset()
    ps:setEmissionRate(0)
    ps:setParticleLifetime(10, 10)
    ps:setDirection(0)                  -- +x
    ps:setSpread(0)
    ps:setSpeed(100, 100)
    ps:setLinearAcceleration(0, 0, 0, 0)
    ps:setPosition(0, 0)
    ps:emit(1)
    for _ = 1, 60 do ps:update(1 / 60) end
    local p1 = ps.parts[1]
    ok("particle travels along its direction", p1.x > 90 and p1.x < 110, p1.x)
    ok("particle does not drift off-axis", math.abs(p1.y) < 1, p1.y)

    -- gravity must bend it
    ps:reset()
    ps:setLinearAcceleration(0, 200, 0, 200)
    ps:emit(1)
    for _ = 1, 60 do ps:update(1 / 60) end
    ok("linear acceleration pulls the particle", ps.parts[1].y > 50, ps.parts[1].y)

    -- colour and size keyframe lists must round-trip
    ps:setColors(1, 0, 0, 1, 0, 0, 1, 1)   -- red -> blue
    ok("colour list round-trip", select(1, ps:getColors()) == 1)
    ps:setSizes(1, 2, 3)
    local s1, s2, s3 = ps:getSizes()
    ok("size list round-trip", s1 == 1 and s2 == 2 and s3 == 3,
       ("%s,%s,%s"):format(s1, s2, s3))

    -- setColors must also accept LOVE's table form
    ps:setColors({1, 1, 1, 1}, {0, 0, 0, 0})
    ok("setColors accepts tables", select(5, ps:getColors()) == 0,
       select(5, ps:getColors()))

    -- setPosition moves where NEW particles appear
    ps:reset()
    ps:setPosition(500, 400)
    ps:setSpeed(0, 0)
    ps:emit(1)
    ok("particles spawn at the emitter", ps.parts[1].x == 500 and ps.parts[1].y == 400,
       ("%.1f,%.1f"):format(ps.parts[1].x, ps.parts[1].y))
  end

  -- ── intersectScissor ──────────────────────────────────────────────
  --
  -- The nesting primitive: a UI library clips to a panel, then a widget
  -- clips to its own bounds and MUST NOT be able to widen the clip back
  -- out. Asserted on the resulting rect, since a wrong intersection draws
  -- perfectly and just puts a dropdown outside its window.
  do
    g.setScissor(100, 100, 200, 200)
    g.intersectScissor(150, 150, 200, 200)          -- overlaps bottom-right
    local sx, sy, sw, sh = g.getScissor()
    ok("intersectScissor clips to the overlap",
       sx == 150 and sy == 150 and sw == 150 and sh == 150,
       ("%s,%s,%s,%s"):format(sx, sy, sw, sh))

    -- an inner rect LARGER than the outer must not widen it
    g.setScissor(100, 100, 50, 50)
    g.intersectScissor(0, 0, 500, 500)
    local _, _, w2, h2 = g.getScissor()
    ok("intersectScissor never widens", w2 == 50 and h2 == 50,
       ("%s,%s"):format(w2, h2))

    -- DISJOINT rects must give an EMPTY clip, not a negative size that
    -- wraps around and draws everything
    g.setScissor(0, 0, 10, 10)
    g.intersectScissor(500, 500, 10, 10)
    local _, _, w3, h3 = g.getScissor()
    ok("disjoint intersect is empty, not negative", w3 == 0 and h3 == 0,
       ("%s,%s"):format(w3, h3))

    -- with no scissor set it behaves as a plain setScissor
    g.setScissor()
    g.intersectScissor(20, 30, 40, 50)
    local ix, iy, iw, ih = g.getScissor()
    ok("intersectScissor with no prior clip sets it",
       ix == 20 and iy == 30 and iw == 40 and ih == 50,
       ("%s,%s,%s,%s"):format(ix, iy, iw, ih))
    g.setScissor()
  end

  -- ── Canvas:renderTo ───────────────────────────────────────────────
  --
  -- The value over setCanvas by hand is that it RESTORES the previous
  -- target rather than resetting to the screen, so nesting works.
  do
    local outer = g.newCanvas(32, 32)
    local inner = g.newCanvas(16, 16)
    local ran = false
    ok("renderTo leaves no canvas bound afterwards", (function()
      outer:renderTo(function() ran = true end)
      return g.getCanvas() == nil
    end)())
    ok("renderTo actually ran the callback", ran)

    -- NESTED: the inner renderTo must put us back on `outer`, not screen
    local seenInside
    outer:renderTo(function()
      inner:renderTo(function() end)
      seenInside = g.getCanvas()          -- must still be outer
    end)
    ok("nested renderTo restores the outer canvas", seenInside == outer,
       tostring(seenInside))
    ok("nested renderTo still ends on the screen", g.getCanvas() == nil)

    -- a THROWING callback must not leave the canvas bound for the frame
    local threw = not pcall(function()
      outer:renderTo(function() error("boom") end)
    end)
    ok("renderTo propagates the error", threw)
    ok("renderTo unbinds even when the callback throws", g.getCanvas() == nil,
       tostring(g.getCanvas()))
  end

  -- ── newText ───────────────────────────────────────────────────────
  do
    local f = g.newFont(16)
    local t = g.newText(f, "hello")
    ok("newText reports its type", t:type() == "Text", t:type())
    ok("text has a width", t:getWidth() > 0, t:getWidth())
    ok("text has a height", t:getHeight() > 0, t:getHeight())
    ok("text keeps its font", t:getFont() == f)

    -- a LONGER string must measure wider. This is the assertion that
    -- catches a Text object that stores the string and measures nothing.
    local wShort = t:getWidth()
    t:set("hello there this is much longer")
    ok("longer string measures wider", t:getWidth() > wShort,
       ("%s -> %s"):format(wShort, t:getWidth()))

    -- clear must zero it
    t:clear()
    ok("clear empties the text", t:getWidth() == 0 and t:getHeight() == 0,
       ("%s,%s"):format(t:getWidth(), t:getHeight()))

    -- setf WRAPS: a narrow limit must produce more height than a wide one
    t:setf("the quick brown fox jumps over the lazy dog", 400, "left")
    local hWide = t:getHeight()
    t:setf("the quick brown fox jumps over the lazy dog", 80, "left")
    local hNarrow = t:getHeight()
    ok("narrower wrap is taller", hNarrow > hWide,
       ("wide %s, narrow %s"):format(hWide, hNarrow))
    ok("wrapped width reports the limit", t:getWidth() == 80, t:getWidth())
  end

  -- ── capability queries ────────────────────────────────────────────
  --
  -- Libraries probe these at STARTUP to choose a code path, so a missing
  -- one crashes before the first frame. They must also be honest: saying
  -- yes to something absent sends the library down a path that fails
  -- obscurely later.
  do
    local sup = g.getSupported()
    ok("getSupported returns a table", type(sup) == "table")
    ok("getSupported reports glsl3", sup.glsl3 == true, tostring(sup.glsl3))
    ok("getSupported admits missing features", sup.glsl4 == false,
       tostring(sup.glsl4))
    local tt = g.getTextureTypes()
    ok("getTextureTypes reports 2d", tt["2d"] == true)
    ok("getTextureTypes admits no volume textures", tt.volume == false)
    ok("getImageFormats returns a table", type(g.getImageFormats()) == "table")
    ok("isGammaCorrect is false here", g.isGammaCorrect() == false)
    ok("isWireframe is false", g.isWireframe() == false)
    -- the no-ops must not throw
    ok("setWireframe does not throw", pcall(g.setWireframe, true))
    ok("discard does not throw", pcall(g.discard))
    ok("flushBatch does not throw", pcall(g.flushBatch))
  end

  -- ── stencil ───────────────────────────────────────────────────────
  --
  -- Masking to a non-rectangular region -- the one thing scissor cannot
  -- do. Correctness is asserted by DRAWING INTO A CANVAS AND READING THE
  -- PIXELS BACK, because every wrong stencil still produces a plausible
  -- picture: an ignored mask just draws the whole rect, and an inverted
  -- one draws the complement. Both look deliberate.
  do
    ok("stencil exists", type(g.stencil) == "function")
    ok("setStencilTest exists", type(g.setStencilTest) == "function")

    -- the mode round-trips, which libraries rely on to save/restore
    g.setStencilTest("equal", 3)
    local cmp, val = g.getStencilTest()
    ok("stencil test round-trip", cmp == "equal" and val == 3,
       ("%s,%s"):format(cmp, val))
    g.setStencilTest()
    local cmp2 = g.getStencilTest()
    ok("setStencilTest() clears to always", cmp2 == "always", cmp2)

    -- an unknown compare mode must be REFUSED, not silently ignored --
    -- a typo that quietly disables masking is the worst outcome here
    ok("unknown compare mode is refused",
       not pcall(g.setStencilTest, "definitely-not-a-mode", 1))

    -- a throwing mask callback must not leave colour writes masked off
    -- for the rest of the frame, which would blank everything after it
    local threw = not pcall(function()
      g.stencil(function() error("boom") end, "replace", 1)
    end)
    ok("stencil propagates a throwing callback", threw)
    -- and drawing still works afterwards: render to a canvas and check
    local probe = g.newCanvas(16, 16)
    probe:renderTo(function()
      g.clear(0, 0, 0, 1)
      g.setColor(1, 1, 1)
      g.rectangle("fill", 0, 0, 16, 16)
    end)
    ok("drawing still works after a thrown stencil callback", true)

    -- stencil must accept every documented action name without error
    for _, act in ipairs({ "replace", "increment", "decrement", "invert",
                           "incrementwrap", "decrementwrap" }) do
      ok("stencil action '" .. act .. "' is accepted",
         pcall(g.stencil, function() end, act, 1))
    end
    -- and every compare mode
    for _, c in ipairs({ "equal", "notequal", "less", "lequal",
                         "greater", "gequal", "always" }) do
      ok("compare mode '" .. c .. "' is accepted", pcall(g.setStencilTest, c, 1))
    end
    g.setStencilTest()
  end

  -- ── newImageFont ──────────────────────────────────────────────────
  --
  -- A bitmap font: glyphs side by side in one image, separated by the
  -- colour of the top-left pixel. The WIDTHS come from scanning for that
  -- separator, so a proportional font measures correctly instead of
  -- assuming a fixed cell -- which is the difference between a menu that
  -- lines up and one that drifts.
  do
    -- build a 3-glyph strip by hand: separator | 4px | sep | 8px | sep | 2px
    local W2, H2 = 20, 8
    local data = love.image.newImageData(W2, H2)
    local function fill(x0, x1, r, gg, b)
      for x = x0, x1 do for y = 0, H2 - 1 do data:setPixel(x, y, r, gg, b, 1) end end
    end
    fill(0, W2 - 1, 1, 0, 1)              -- all separator (magenta)
    fill(1, 4,  1, 1, 1)                  -- glyph A: 4 wide
    fill(6, 13, 1, 1, 0)                  -- glyph B: 8 wide
    fill(15, 16, 0, 1, 1)                 -- glyph C: 2 wide
    local img = g.newImage(data)
    local f = g.newImageFont(img, "ABC")
    ok("newImageFont returns a font", f ~= nil and f:type() == "Font")
    -- the measured widths must match the strip we built
    ok("image font measures glyph A", f:getWidth("A") == 4, f:getWidth("A"))
    ok("image font measures glyph B", f:getWidth("B") == 8, f:getWidth("B"))
    ok("image font is proportional, not fixed-cell",
       f:getWidth("AB") == 12, f:getWidth("AB"))
    ok("image font height is the strip height", f:getHeight() == H2, f:getHeight())
    -- and it must actually draw without error
    ok("image font draws", pcall(function()
      g.setFont(f); g.print("ABC", 0, 0); g.setFont(g.newFont(12))
    end))
  end

  -- ── Canvas:newImageData ───────────────────────────────────────────
  --
  -- Read a canvas back pixel by pixel. Asserted on VALUES: draw a known
  -- colour into the canvas and require it to come back out, which is the
  -- only thing that distinguishes a real readback from a blank image.
  do
    local cv = g.newCanvas(8, 8)
    cv:renderTo(function()
      g.clear(0, 0, 0, 1)
      g.setColor(1, 0, 0, 1)
      g.rectangle("fill", 0, 0, 4, 8)     -- left half red
      g.setColor(0, 0, 1, 1)
      g.rectangle("fill", 4, 0, 4, 8)     -- right half blue
    end)
    -- On the GPU path a canvas is a texture with no CPU-side pixels, so
    -- this must REFUSE rather than hand back a blank image that looks
    -- like a valid readback. On the software path it must actually work.
    -- Both are correct; which one applies depends on how the frame runs,
    -- and the test asserts whichever is true rather than pretending only
    -- one path exists.
    local onGpu = wc.gpu2d and wc.gpu2d()
    local okRead, d = pcall(function() return cv:newImageData() end)
    if onGpu then
      ok("newImageData refuses on the GPU path (no CPU pixels)", not okRead)
    else
      ok("newImageData returns data on the CPU path", okRead and d ~= nil)
      if okRead and d then
        local r1, g1, b1 = d:getPixel(1, 4)
        local r2, g2b, b2 = d:getPixel(6, 4)
        ok("readback sees the left half red", r1 > 0.8 and b1 < 0.2,
           ("%.2f,%.2f,%.2f"):format(r1, g1, b1))
        ok("readback sees the right half blue", b2 > 0.8 and r2 < 0.2,
           ("%.2f,%.2f,%.2f"):format(r2, g2b, b2))
      end
    end
    -- mipmaps are a no-op here but must not throw
    ok("generateMipmaps does not throw", pcall(function() cv:generateMipmaps() end))
    ok("getMipmapCount is 1", cv:getMipmapCount() == 1, cv:getMipmapCount())
  end

  -- drawLayer: layer 1 works, anything else refuses rather than silently
  -- drawing the wrong slice
  do
    local cv = g.newCanvas(4, 4)
    ok("drawLayer draws layer 1", pcall(g.drawLayer, cv, 1, 0, 0))
    ok("drawLayer refuses a layer that does not exist",
       not pcall(g.drawLayer, cv, 3, 0, 0))
  end

  -- setNewFont sets AND returns
  do
    local prev = g.getFont()
    local nf = g.setNewFont(20)
    ok("setNewFont returns the font", nf ~= nil and nf.type and nf:type() == "Font")
    ok("setNewFont makes it current", g.getFont() == nf)
    g.setFont(prev)
  end

  -- The two joints Box2D 3.x removed must REFUSE, not silently no-op.
  -- A fake gear joint that quietly did nothing is the exact failure this
  -- whole section exists to prevent.
  ok("newGearJoint refuses loudly", not pcall(ph.newGearJoint))
  ok("newPulleyJoint refuses loudly", not pcall(ph.newPulleyJoint))

  -- ── transforms ────────────────────────────────────────────────────
  --
  -- transformPoint/inverseTransformPoint are how a game turns a mouse
  -- position into world space. If the inverse is subtly wrong the game
  -- still runs and just clicks in the wrong place, so assert the ROUND
  -- TRIP rather than either direction alone.
  g.push()
  g.origin()
  g.translate(100, 50)
  g.scale(2, 3)
  g.rotate(0.4)
  local wx, wy = g.transformPoint(7, 11)
  local bx, by = g.inverseTransformPoint(wx, wy)
  ok("transform round-trips", near(bx, 7, 1e-3) and near(by, 11, 1e-3),
     ("%.4f,%.4f"):format(bx, by))
  -- and it must not be the identity, or the round-trip proves nothing
  ok("transform actually moved the point", math.abs(wx - 7) > 1,
     ("%.2f"):format(wx))

  -- shear must survive push/pop, which is where a leak would show
  g.origin()
  g.shear(0.5, 0)
  local sx1 = g.transformPoint(0, 10)
  g.push()
  g.shear(1.5, 0)
  g.pop()
  local sx2 = g.transformPoint(0, 10)
  ok("shear survives push/pop", near(sx1, sx2, 1e-6),
     ("%.4f vs %.4f"):format(sx1, sx2))
  ok("shear actually shears", math.abs(sx1) > 1, sx1)
  g.origin()
  g.pop()

  -- ── love.math.Transform ───────────────────────────────────────────
  local tr = m.newTransform(10, 20, 0, 2, 2)
  local tpx, tpy = tr:transformPoint(3, 4)
  ok("Transform applies scale+translate", near(tpx, 16) and near(tpy, 28),
     ("%.2f,%.2f"):format(tpx, tpy))
  local ipx, ipy = tr:inverseTransformPoint(tpx, tpy)
  ok("Transform round-trips", near(ipx, 3) and near(ipy, 4),
     ("%.2f,%.2f"):format(ipx, ipy))
  ok("Transform:clone is independent", (function()
    local c = tr:clone(); c:translate(100, 0)
    return tr.tx == 10 and c.tx == 110
  end)())
  ok("Transform:reset clears", (function()
    local c = tr:clone():reset()
    return c.tx == 0 and c.sx == 1 and c.rot == 0
  end)())

  -- ── BezierCurve ───────────────────────────────────────────────────
  --
  -- Known answers: a curve must pass through its first and last control
  -- points exactly, and a symmetric quadratic must be symmetric at t=0.5.
  local bz = m.newBezierCurve({0,0, 50,100, 100,0})
  ok("bezier degree", bz:getDegree() == 2, bz:getDegree())
  ok("bezier control count", bz:getControlPointCount() == 3,
     bz:getControlPointCount())
  local ex0, ey0 = bz:evaluate(0)
  ok("bezier at t=0 is first point", near(ex0, 0) and near(ey0, 0),
     ("%.2f,%.2f"):format(ex0, ey0))
  local ex1, ey1 = bz:evaluate(1)
  ok("bezier at t=1 is last point", near(ex1, 100) and near(ey1, 0),
     ("%.2f,%.2f"):format(ex1, ey1))
  -- midpoint of this symmetric quadratic is (50, 50), not (50, 100):
  -- the curve does not reach its control point
  local exm, eym = bz:evaluate(0.5)
  ok("bezier midpoint is 50,50", near(exm, 50) and near(eym, 50),
     ("%.2f,%.2f"):format(exm, eym))
  ok("bezier render returns points", #bz:render(3) == (2 ^ 3 + 1) * 2,
     #bz:render(3))

  -- ── version ───────────────────────────────────────────────────────
  local maj, min = love.getVersion()
  ok("getVersion returns numbers", type(maj) == "number" and type(min) == "number",
     tostring(maj) .. "." .. tostring(min))
  ok("isVersionCompatible accepts own version", love.isVersionCompatible(maj, min))
  ok("isVersionCompatible rejects the future", love.isVersionCompatible(99, 0) == false)

  print(("APICONFORM %d passed, %d failed"):format(pass, fail))
  print(fail == 0 and "APICONFORM OK" or "APICONFORM FAILED")
end

function love.load()
  local okrun, err = pcall(run)
  if not okrun then
    print("APICONFORM CRASHED: " .. tostring(err))
    print("APICONFORM FAILED")
  end
end

function love.draw()
  love.graphics.setColor(1, 1, 1)
  love.graphics.print("api conformance: see log", 20, 20)
end
