-- Rumble conformance cart. The rumble ABI is write-only (the host never
-- reports motor state back), so what this cart draws proves nothing: the
-- assertions live in the JS harness, which records every wc_pad_rumble call
-- and checks the pad ids, the 0..1 strengths and the seconds->ms conversion.
--
-- Frame N drives case N, one case per frame, so the harness can index the
-- recorded calls by position instead of pattern-matching them.
local frame = 0

function love.load()
  -- pad 1 is the rumble-capable pad in the harness, pad 2 is not
  love.log("cap1=" .. tostring(love.pad.hasVibration()))
  love.log("cap2=" .. tostring(love.pad.hasVibration(2)))
  love.log("cap4=" .. tostring(love.pad.hasVibration(4)))
end

local cases = {
  -- implicit pad 1, both motors, half a second
  function() love.pad.setVibration(0.5, 0.25, 0.5) end,
  -- explicit pad 2: 1-based in Lua, so the ABI must see pad id 1
  function() love.pad.setVibration(2, 1.0, 0.0, 2.0) end,
  -- no duration: the host's own cap stands in for "until told otherwise"
  function() love.pad.setVibration(0.75, 0.75) end,
  -- zero strength is a stop, not a silent buzz
  function() love.pad.setVibration(0, 0) end,
  -- explicit stop of pad 3
  function() love.pad.stopVibration(3) end,
  -- no args at all stops pad 1
  function() love.pad.setVibration() end,
  -- out of range on both ends: the engine clamps before the host has to
  function() love.pad.setVibration(2.5, -1, 0.1) end,
  -- duration past WC_RUMBLE_MAX_MS is pinned to the cap
  function() love.pad.setVibration(0.5, 0.5, 99) end,
  -- the Joystick object route, pad 4 -> ABI id 3
  function() love.joystick.getJoysticks()[4]:setVibration(0.2, 0.3, 0.25) end,
  -- getVibration reports back what was last asked for
  function()
    love.pad.setVibration(0.6, 0.4, 1)
    local l, r = love.pad.getVibration()
    love.log(("get=%.2f,%.2f"):format(l, r))
  end,
  -- out-of-range pad number is refused rather than wrapping onto pad 1
  function() love.log("bad=" .. tostring(love.pad.setVibration(9, 1, 1, 1))) end,
}

function love.update()
  frame = frame + 1
  local c = cases[frame]
  if c then c() end
end

function love.draw()
  love.graphics.setColor(0.1, 0.1, 0.2)
  love.graphics.rectangle("fill", 0, 0, 1280, 720)
  love.graphics.setColor(1, 1, 1)
  love.graphics.print("rumble case " .. frame, 40, 40)
end
