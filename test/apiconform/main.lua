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
