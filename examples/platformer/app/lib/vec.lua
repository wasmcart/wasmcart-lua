-- vec.lua - a tiny 2D vector, loaded via require "vec".
-- Exists to prove the cart module loader handles real Lua: metatables,
-- operator overloading, closures over upvalues.

local Vec = {}
Vec.__index = Vec

function Vec.__add(a, b) return setmetatable({ x = a.x + b.x, y = a.y + b.y }, Vec) end
function Vec.__sub(a, b) return setmetatable({ x = a.x - b.x, y = a.y - b.y }, Vec) end
function Vec.__mul(a, s) return setmetatable({ x = a.x * s, y = a.y * s }, Vec) end
function Vec.__eq(a, b)  return a.x == b.x and a.y == b.y end
function Vec.__tostring(v) return ("(%.1f, %.1f)"):format(v.x, v.y) end

function Vec:len() return math.sqrt(self.x * self.x + self.y * self.y) end

function Vec:normalized()
  local l = self:len()
  if l == 0 then return setmetatable({ x = 0, y = 0 }, Vec) end
  return setmetatable({ x = self.x / l, y = self.y / l }, Vec)
end

return function(x, y)
  return setmetatable({ x = x or 0, y = y or 0 }, Vec)
end
