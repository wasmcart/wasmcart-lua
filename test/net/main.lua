-- love.net conformance cart. Driven against a REAL WebSocket server
-- (wasmcart's test/wsserver.mjs) through the real reference host, so every
-- assertion here crosses an actual socket rather than a stub.
--
-- Nothing about networking reaches the framebuffer, so the harness reads the
-- log lines this cart emits. Each line is a single fact with a stable prefix
-- the harness matches on.
--
-- Role and address come from app assets the harness writes per run, because
-- the two halves of the relay test are two separate carts that must behave
-- differently while sharing one binary.
local role = (love.filesystem.read("role.txt") or "echo"):gsub("%s+$", "")
local addr = (love.filesystem.read("addr.txt") or ""):gsub("%s+$", "")

local peer = nil
local frame = 0
local sent_probe = false

-- A payload with an embedded NUL and a high byte. If any part of the path
-- treats this as a C string it comes back short, and if any part assumes
-- UTF-8 it comes back mangled. Both are silent failures at the Lua level
-- unless the round trip is compared byte for byte.
local PROBE = "AB\0CD\255\0\0EF"

local function hex(s)
  return (s:gsub(".", function(c) return string.format("%02x", c:byte()) end))
end

function love.load()
  peer = love.net.open(addr)
  love.log("open=" .. tostring(peer ~= nil))
  -- A bad address must be refused rather than yielding a phantom peer.
  love.log("badopen=" .. tostring(love.net.open("nonsense://not-a-host/x")))
  love.log("badtype=" .. tostring(love.net.open(12345)))
  -- Before anything connects, an unknown peer reads as closed.
  love.log("ghost=" .. love.net.state(9999))
end

function love.net.connected(p, name)
  -- The harness may register host-side peers as well, so tag the one this
  -- cart dialed separately: peers the host hands over are not the same case
  -- and the assertions differ.
  local tag = (p == peer) and "dialed" or "host"
  love.log("connected=" .. tostring(p) .. " kind=" .. tag .. " name=" .. tostring(name))
  love.log("state=" .. tostring(p) .. "=" .. love.net.state(p))
  love.log("count=" .. tostring(love.net.count()))
  love.log("peers=" .. table.concat(love.net.peers(), ","))
  local t = love.net.transport(p)
  love.log("transport=" .. tostring(p) .. "=" .. tostring(t.reliable) .. "," .. tostring(t.ordered))
end

function love.net.message(p, data)
  love.log("msg=" .. tostring(p) .. " len=" .. #data .. " hex=" .. hex(data))
  if data == PROBE then love.log("roundtrip=exact") end
end

function love.net.disconnected(p)
  love.log("disconnected=" .. tostring(p))
  love.log("state_after=" .. love.net.state(p))
end

function love.net.error(p)
  love.log("neterr=" .. tostring(p))
end

function love.update()
  frame = frame + 1
  if not sent_probe and peer and love.net.isOpen(peer) then
    sent_probe = true
    if role == "relay-b" then
      -- The B end answers rather than opening; A's probe arrives as a message
      -- and B echoes it back through the relay via broadcast.
      love.log("ready=b")
    else
      love.log("sent=" .. tostring(love.net.send(peer, PROBE)))
      -- broadcast reaches every open peer, so with one peer it returns 1
      love.log("bcast=" .. tostring(love.net.broadcast("z")))
      -- a send to an id the host never issued must be refused, not silently
      -- dropped as if it worked
      love.log("badsend=" .. tostring(love.net.send(4242, "x")))
    end
  end
  if role == "relay-b" and frame == 40 then
    love.net.close(peer)
    love.log("bclose=" .. love.net.state(peer))
  end
end

function love.draw()
  love.graphics.setColor(0.1, 0.1, 0.2)
  love.graphics.rectangle("fill", 0, 0, 1280, 720)
  love.graphics.setColor(1, 1, 1)
  love.graphics.print("net " .. role .. " frame " .. frame, 40, 40)
end
