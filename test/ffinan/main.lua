-- ffi shim: a non-finite value must not crash an integer field write.
--
-- Real geometry produces NaN. A sphere's tangent at the pole is a 0/0, and
-- 3DreamEngine writes tangents into BYTE fields (n * 127.5 + 127.5), so a
-- single degenerate vertex used to abort the whole mesh build from deep
-- inside a vertex loop with "number has no integer representation".
--
-- C would store some byte and carry on. So does the shim now.

local fails, total = 0, 0
local messages = {}

local function ok(cond, what)
  total = total + 1
  if not cond then
    fails = fails + 1
    messages[#messages + 1] = "FAIL " .. what
  end
end

local ffi = require("ffi")
ffi.cdef [[typedef struct { float x; unsigned char n; int i; } V;]]
local a = ffi.new("V[?]", 8)

-- the ordinary cases must keep working
local good = pcall(function() a[0].n = 191.25 end)
ok(good, "a fractional value still writes to a byte field")
ok(a[0].n == 191, "and truncates toward zero like C")

ok(pcall(function() a[0].x = 1.5 end), "float field takes a float")

-- the non-finite cases must NOT throw
local nanOk = pcall(function() a[1].n = 0 / 0 end)
ok(nanOk, "NaN into a byte field does not throw")

local infOk = pcall(function() a[2].n = 1 / 0 end)
ok(infOk, "+inf into a byte field does not throw")

local ninfOk = pcall(function() a[3].i = -1 / 0 end)
ok(ninfOk, "-inf into an int field does not throw")

-- a float field still stores non-finite values, because those ARE
-- representable there and clamping them would be the wrong answer
ok(pcall(function() a[4].x = 0 / 0 end), "NaN into a float field is allowed")
ok(a[4].x ~= a[4].x, "and stays NaN")

for _, m in ipairs(messages) do print(m) end
print(("ffinan: %d/%d passed"):format(total - fails, total))

function love.update()
  love.debugValue(0, fails)
  love.debugValue(1, total)
end

function love.draw()
  if fails == 0 then
    love.graphics.setColor(0.2, 0.9, 0.4)
    love.graphics.print(("ALL %d ffi NAN TESTS PASSED"):format(total), 60, 60)
  else
    love.graphics.setColor(1, 0.3, 0.3)
    love.graphics.print(("%d / %d FAILED"):format(fails, total), 60, 60)
    for i, m in ipairs(messages) do
      love.graphics.print(m, 60, 100 + i * 26)
    end
  end
end
