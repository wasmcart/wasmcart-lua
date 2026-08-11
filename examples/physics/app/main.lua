-- Physics acceptance cart: exercises Box2D (b2) and Box3D (b3) from Lua,
-- asserts on real simulation behaviour, and reports what the BUILD got for
-- SIMD and threads. Prints PHYSICS-TEST lines a harness can grep.
local results, failed = {}, 0

local function check(name, ok, detail)
  results[#results+1] = { name = name, ok = ok, detail = detail or "" }
  if not ok then failed = failed + 1 end
  print(string.format("PHYSICS-TEST %s %s %s", ok and "PASS" or "FAIL", name, detail or ""))
end

local function approx(a, b, tol) return math.abs(a - b) <= (tol or 1e-3) end

function love.load()
  ---------------------------------------------------------------- Box2D
  check("b2 global exists", type(b2) == "table")
  if type(b2) == "table" then
    local w = b2.world_new(0, 900)            -- gravity down, pixels/s^2
    check("b2 world_new", type(w) == "number")

    -- a dynamic box should fall
    local body = b2.body_new(w, 100, 100, 2)
    b2.shape_box(body, 20, 20)
    local _, y0 = b2.body_position(body)
    for _ = 1, 60 do b2.world_step(w, 1/60, 4) end
    local _, y1 = b2.body_position(body)
    check("b2 gravity pulls a body down", y1 > y0 + 50,
          string.format("y %.1f -> %.1f", y0, y1))

    -- a static floor should stop it
    local floor = b2.body_new(w, 0, 600, 0)
    b2.shape_box(floor, 2000, 20)
    for _ = 1, 240 do b2.world_step(w, 1/60, 4) end
    local _, yr = b2.body_position(body)
    check("b2 body rests on a static floor", yr < 600,
          string.format("resting y %.1f", yr))

    -- impulses move things
    local p = b2.body_new(w, 400, 100, 2)
    if b2.shape_circle then b2.shape_circle(p, 10) end
    local x0 = b2.body_position(p)
    b2.body_apply_impulse(p, 500, 0)
    for _ = 1, 30 do b2.world_step(w, 1/60, 4) end
    local x1 = b2.body_position(p)
    check("b2 impulse moves a body", x1 > x0, string.format("x %.1f -> %.1f", x0, x1))

    b2.world_destroy(w)
  end

  ---------------------------------------------------------------- Box3D
  check("b3 global exists", type(b3) == "table")
  if type(b3) == "table" then
    local info = b3.info()
    print(string.format("PHYSICS-TEST INFO simd=%s threads=%s workers=%d hw=%d",
          tostring(info.simd), tostring(info.threads),
          info.workers or -1, info.hw_threads or -1))
    check("b3 reports a SIMD kernel", type(info.simd) == "string" and #info.simd > 0,
          tostring(info.simd))

    local w = b3.world_new(0, -640, 0)         -- -10 m/s^2, expressed in px/s^2
    check("b3 world_new", type(w) == "number")
    check("b3 uses worker threads when the build has them",
          (info.threads == false) or (info.workers > 0),
          string.format("threads=%s workers=%d", tostring(info.threads), info.workers))

    -- ground plane + a falling sphere
    local ground = b3.body_new(w, 0, 0, 0, 0)
    b3.shape_box(ground, 1000, 10, 1000, 1)

    local ball = b3.body_new(w, 0, 500, 0, 2)
    b3.shape_sphere(ball, 30, 1)
    local _, by0 = b3.body_position(ball)
    for _ = 1, 60 do b3.world_step(w, 1/60, 4) end
    local _, by1 = b3.body_position(ball)
    check("b3 gravity pulls a body down", by1 < by0 - 10,
          string.format("y %.1f -> %.1f", by0, by1))

    for _ = 1, 300 do b3.world_step(w, 1/60, 4) end
    local bx, byr, bz = b3.body_position(ball)
    check("b3 sphere comes to rest above the ground", byr > 0 and byr < 200,
          string.format("resting y %.1f", byr))
    check("b3 body stays on its drop axis", approx(bx, 0, 25) and approx(bz, 0, 25),
          string.format("x %.1f z %.1f", bx, bz))

    -- mass follows from shape density
    check("b3 body has mass from its shape", b3.body_mass(ball) > 0,
          string.format("%.3f kg", b3.body_mass(ball)))

    -- a capsule is a distinct shape that also settles
    local cap = b3.body_new(w, 200, 500, 0, 2)
    b3.shape_capsule(cap, 40, 20, 1)
    for _ = 1, 300 do b3.world_step(w, 1/60, 4) end
    local _, cy = b3.body_position(cap)
    check("b3 capsule settles", cy > 0 and cy < 300, string.format("y %.1f", cy))

    -- rotation is a unit quaternion
    local qx, qy, qz, qw = b3.body_rotation(ball)
    local qlen = math.sqrt(qx*qx + qy*qy + qz*qz + qw*qw)
    check("b3 rotation is a unit quaternion", approx(qlen, 1, 1e-2),
          string.format("|q| = %.4f", qlen))

    -- set_transform then read back
    b3.body_set_transform(cap, 100, 400, 50, 0, 1, 0, 1.0)
    local sx, sy, sz = b3.body_position(cap)
    check("b3 set_transform round-trips",
          approx(sx, 100, 1) and approx(sy, 400, 1) and approx(sz, 50, 1),
          string.format("%.1f,%.1f,%.1f", sx, sy, sz))

    -- raycast straight down onto the ground
    local hx, hy, hz = b3.raycast(w, 0, 400, 0, 0, -1000, 0)
    check("b3 raycast hits the ground", hx ~= nil,
          hx and string.format("hit %.1f,%.1f,%.1f", hx, hy, hz) or "no hit")

    -- a ray into empty space misses
    check("b3 raycast misses empty space",
          b3.raycast(w, 0, 400, 0, 0, 1000, 0) == nil)

    b3.world_destroy(w)
  end

  -- Word this so a clean run contains no failure words at all: the engine's
  -- test harness scans cart logs for them, and a green suite that trips the
  -- harness is indistinguishable from a broken one.
  if failed == 0 then
    print(string.format("PHYSICS-TEST DONE %d/%d ok", #results, #results))
  else
    print(string.format("PHYSICS-TEST DONE %d checks, %d FAILED", #results, failed))
  end
end

function love.draw()
  local g = love.graphics
  g.setColor(1,1,1)
  g.print(string.format("physics: %d checks, %d failed", #results, failed), 20, 20)
  local y = 60
  for _, r in ipairs(results) do
    g.setColor(r.ok and 0.4 or 1, r.ok and 1 or 0.3, 0.4)
    g.print((r.ok and "PASS  " or "FAIL  ") .. r.name .. "  " .. r.detail, 20, y)
    y = y + 22
  end
end
