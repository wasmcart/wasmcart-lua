-- b3 surface-material / damping / sleep / contact-event tests.
--
-- These assert BEHAVIOUR, not just that a function exists: every check here
-- fails on the pre-extension binding, where density was the only settable
-- property and a struck ball would bounce off nothing and roll forever.
--
-- Results mirror into debug fields (0 = fail count, 1 = total), same shape
-- as test/unit.

local fails, total = 0, 0
local messages = {}

local function check(cond, what)
  total = total + 1
  if not cond then
    fails = fails + 1
    messages[#messages + 1] = "FAIL " .. what
  end
end

local function near(a, b, tol, what)
  total = total + 1
  if math.abs(a - b) > tol then
    fails = fails + 1
    messages[#messages + 1] = ("FAIL %s: got %.4f want %.4f (+-%.4f)")
      :format(what, a, b, tol)
  end
end

local function run()
  check(b3 ~= nil, "b3 global exists")
  if not b3 then return end

  -- ── round-trip every new setter ───────────────────────────────────
  local w = b3.world_new(0, -980, 0)
  local body = b3.body_new(w, 0, 500, 0)
  local sh = b3.shape_sphere(body, 20)

  b3.shape_set_friction(sh, 0.21)
  near(b3.shape_get_friction(sh), 0.21, 0.001, "friction round-trip")

  b3.shape_set_restitution(sh, 0.93)
  near(b3.shape_get_restitution(sh), 0.93, 0.001, "restitution round-trip")

  b3.shape_set_rolling_resistance(sh, 0.05)
  near(b3.shape_get_rolling_resistance(sh), 0.05, 0.001, "rolling resistance round-trip")

  b3.shape_set_material(sh, 0.4, 0.7, 0.02)
  near(b3.shape_get_friction(sh), 0.4, 0.001, "set_material friction")
  near(b3.shape_get_restitution(sh), 0.7, 0.001, "set_material restitution")
  near(b3.shape_get_rolling_resistance(sh), 0.02, 0.001, "set_material rolling")

  b3.body_set_linear_damping(body, 0.35)
  near(b3.body_get_linear_damping(body), 0.35, 0.001, "linear damping round-trip")

  b3.body_set_angular_damping(body, 0.44)
  near(b3.body_get_angular_damping(body), 0.44, 0.001, "angular damping round-trip")

  b3.body_set_angular_velocity(body, 1, 2, 3)
  local ax, ay, az = b3.body_angular_velocity(body)
  near(ax, 1, 0.01, "angular velocity x")
  near(ay, 2, 0.01, "angular velocity y")
  near(az, 3, 0.01, "angular velocity z")

  b3.body_set_bullet(body, true)
  check(b3.body_is_bullet(body) == true, "bullet round-trip")

  b3.body_set_gravity_scale(body, 0.5)
  near(b3.body_get_gravity_scale(body), 0.5, 0.001, "gravity scale round-trip")

  b3.body_set_type(body, 1)
  check(b3.body_get_type(body) == 1, "body type round-trip")
  b3.body_set_type(body, 2)

  b3.world_destroy(w)

  -- ── BEHAVIOUR: restitution actually bounces ───────────────────────
  -- A ball dropped on a floor with restitution 0.9 must rebound; with
  -- restitution 0 it must not. This is the check that would have caught
  -- the missing binding: both cases were identical before.
  local function drop(rest)
    local ww = b3.world_new(0, -980, 0)
    local floor = b3.body_new(ww, 0, 0, 0, 0)          -- static
    local fs = b3.shape_box(floor, 400, 10, 400)
    b3.shape_set_material(fs, 0.5, rest)

    local ball = b3.body_new(ww, 0, 200, 0)
    local bs = b3.shape_sphere(ball, 15)
    b3.shape_set_material(bs, 0.5, rest)
    b3.body_enable_sleep(ball, false)

    -- fall, hit, and rebound; track the highest point reached AFTER the
    -- first contact rather than the initial drop height
    local hitYet, peak = false, -1e9
    for _ = 1, 240 do
      b3.world_step(ww, 1 / 60, 4)
      local _, y = b3.body_position(ball)
      local _, vy = b3.body_velocity(ball)
      if not hitYet and y < 40 then hitYet = true end
      if hitYet and vy > 0 then peak = math.max(peak, y) end
    end
    b3.world_destroy(ww)
    return peak
  end

  local bouncy = drop(0.9)
  local dead   = drop(0.0)
  check(bouncy > 60, ("restitution 0.9 rebounds (peak %.1f > 60)"):format(bouncy))
  check(dead < 40, ("restitution 0 does not rebound (peak %.1f < 40)"):format(dead))
  check(bouncy > dead + 30, "bouncy rebounds much higher than dead")

  -- ── BEHAVIOUR: damping actually slows a body ──────────────────────
  local function slide(damping)
    local ww = b3.world_new(0, 0, 0)                    -- no gravity
    local b = b3.body_new(ww, 0, 0, 0)
    b3.shape_sphere(b, 15)
    b3.body_set_linear_damping(b, damping)
    b3.body_enable_sleep(b, false)
    b3.body_set_velocity(b, 600, 0, 0)
    for _ = 1, 120 do b3.world_step(ww, 1 / 60, 4) end
    local vx = b3.body_velocity(b)
    b3.world_destroy(ww)
    return vx
  end

  local fast = slide(0.0)
  local slow = slide(2.0)
  near(fast, 600, 1.0, "damping 0 keeps its speed")
  check(slow < 100, ("damping 2.0 bleeds speed (%.1f < 100)"):format(slow))

  -- ── BEHAVIOUR: sleep reports a settled table ──────────────────────
  -- The game asks "have all the balls stopped?" to end a turn. Without
  -- body_is_awake there is no way to ask.
  local ws = b3.world_new(0, -980, 0)
  local fl = b3.body_new(ws, 0, 0, 0, 0)
  b3.shape_box(fl, 400, 10, 400)
  local rest = b3.body_new(ws, 0, 40, 0)
  b3.shape_sphere(rest, 15)
  check(b3.body_is_awake(rest) == true, "a fresh body is awake")
  for _ = 1, 600 do b3.world_step(ws, 1 / 60, 4) end
  check(b3.body_is_awake(rest) == false, "a settled body falls asleep")
  b3.world_destroy(ws)

  -- ── BEHAVIOUR: contact events fire with a real approach speed ─────
  local wc3 = b3.world_new(0, -980, 0)
  b3.world_set_hit_threshold(wc3, 30)
  local gfl = b3.body_new(wc3, 0, 0, 0, 0)
  local gfs = b3.shape_box(gfl, 400, 10, 400)
  b3.shape_enable_hit_events(gfs, true)
  local drop2 = b3.body_new(wc3, 0, 300, 0)
  local ds = b3.shape_sphere(drop2, 15)
  b3.shape_enable_hit_events(ds, true)

  local sawHit, hitSpeed = false, 0
  for _ = 1, 180 do
    b3.world_step(wc3, 1 / 60, 4)
    local ev = b3.contact_events(wc3)
    if ev and ev.hits and #ev.hits > 0 then
      sawHit = true
      hitSpeed = math.max(hitSpeed, ev.hits[1].speed or 0)
    end
  end
  check(sawHit, "a falling ball raises a hit event")
  check(hitSpeed > 100, ("hit reports a real approach speed (%.1f px/s)"):format(hitSpeed))
  b3.world_destroy(wc3)

  -- ── b2: joints and post-creation material ─────────────────────────
  -- Box2D's whole constraint vocabulary was unreachable from Lua before
  -- this: no hinge, no rope, no slider, no weld.
  check(b2 ~= nil, "b2 global exists")
  if b2 then
    local w2 = b2.world_new(0, 980)

    -- a hinge holds a swinging body at a fixed distance from its pivot
    local anchor = b2.body_new(w2, 400, 100, 0)          -- static
    b2.shape_box(anchor, 10, 10)
    local arm = b2.body_new(w2, 500, 100)                -- dynamic
    b2.shape_box(arm, 50, 8)
    local hinge = b2.joint_revolute(w2, anchor, arm, 400, 100)
    check(hinge ~= nil and hinge > 0, "joint_revolute returns a handle")

    for _ = 1, 180 do b2.world_step(w2, 1 / 60, 4) end
    local ax, ay = b2.body_position(arm)
    local d = math.sqrt((ax - 400) ^ 2 + (ay - 100) ^ 2)
    -- it must SWING (leave its start) but stay tethered to the pivot
    check(ay > 105, ("hinged arm swings down (y %.1f > 105)"):format(ay))
    near(d, 100, 12, "hinged arm stays at its pivot radius")

    local fx, fy = b2.joint_force(hinge)
    check(type(fx) == "number" and type(fy) == "number", "joint_force returns numbers")
    check(math.abs(fx) + math.abs(fy) > 0, "a loaded hinge reports nonzero force")

    b2.joint_destroy(hinge)
    -- with the hinge gone the arm is in free fall, so it leaves the radius
    for _ = 1, 120 do b2.world_step(w2, 1 / 60, 4) end
    local _, fy2 = b2.body_position(arm)
    check(fy2 > ay + 50, "destroying the hinge frees the body")

    -- a rope joint caps separation
    local hookA = b2.body_new(w2, 800, 100, 0)
    b2.shape_box(hookA, 10, 10)
    local hangB = b2.body_new(w2, 800, 140)
    b2.shape_box(hangB, 10, 10)
    local rope = b2.joint_distance(w2, hookA, hangB, 800, 100, 60)
    check(rope ~= nil and rope > 0, "joint_distance returns a handle")
    for _ = 1, 240 do b2.world_step(w2, 1 / 60, 4) end
    local rx, ry = b2.body_position(hangB)
    local rd = math.sqrt((rx - 800) ^ 2 + (ry - 100) ^ 2)
    near(rd, 60, 12, "rope holds its length under gravity")

    -- weld is rigid: the welded body must not swing away
    local wA = b2.body_new(w2, 1200, 100, 0)
    b2.shape_box(wA, 10, 10)
    local wB = b2.body_new(w2, 1240, 100)
    b2.shape_box(wB, 20, 8)
    local weld = b2.joint_weld(w2, wA, wB, 1200, 100)
    check(weld ~= nil and weld > 0, "joint_weld returns a handle")
    for _ = 1, 180 do b2.world_step(w2, 1 / 60, 4) end
    local _, wy = b2.body_position(wB)
    check(math.abs(wy - 100) < 20, ("welded body stays put (y %.1f)"):format(wy))

    -- post-creation material setters
    local mb = b2.body_new(w2, 100, 900)
    local ms = b2.shape_circle(mb, 10)
    b2.shape_set_friction(ms, 0.13)
    near(b2.shape_get_friction(ms), 0.13, 0.001, "b2 friction round-trip")
    b2.shape_set_restitution(ms, 0.77)
    near(b2.shape_get_restitution(ms), 0.77, 0.001, "b2 restitution round-trip")

    b2.body_set_angular_velocity(mb, 3.5)
    near(b2.body_angular_velocity(mb), 3.5, 0.01, "b2 angular velocity round-trip")

    b2.world_destroy(w2)
  end

  -- ── the shape of the event table ──────────────────────────────────
  local we = b3.world_new(0, 0, 0)
  local ev = b3.contact_events(we)
  check(type(ev) == "table", "contact_events returns a table")
  check(type(ev.hits) == "table", "events.hits is a table")
  check(type(ev.begins) == "table", "events.begins is a table")
  check(type(ev.ends) == "table", "events.ends is a table")
  b3.world_destroy(we)
end

local ok, err = pcall(run)
if not ok then
  fails = fails + 1
  total = total + 1
  messages[#messages + 1] = "FAIL threw: " .. tostring(err)
end

for _, m in ipairs(messages) do print(m) end
print(("b3mat: %d/%d passed"):format(total - fails, total))

function love.update()
  love.debugValue(0, fails)
  love.debugValue(1, total)
end

function love.draw()
  if fails == 0 then
    love.graphics.setColor(0.2, 0.9, 0.4)
    love.graphics.print(("ALL %d b3 MATERIAL TESTS PASSED"):format(total), 60, 60)
  else
    love.graphics.setColor(1, 0.3, 0.3)
    love.graphics.print(("%d / %d FAILED"):format(fails, total), 60, 60)
    for i, m in ipairs(messages) do
      if i > 14 then break end
      love.graphics.print(m, 60, 100 + i * 26)
    end
  end
end
