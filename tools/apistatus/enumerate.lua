-- Enumerate every love.* function this engine actually exposes, at runtime.
-- Reflection beats grep: it sees functions defined any which way, and it
-- cannot report something the engine does not really have.
local out = {}
local function walk(tbl, prefix, depth)
  if depth > 2 then return end
  local keys = {}
  for k in pairs(tbl) do if type(k) == "string" then keys[#keys+1] = k end end
  table.sort(keys)
  for _, k in ipairs(keys) do
    local ok, v = pcall(function() return tbl[k] end)
    if ok then
      local path = prefix .. "." .. k
      if type(v) == "function" then
        out[#out+1] = path
      elseif type(v) == "table" and depth < 2 and k:sub(1,1) ~= "_" then
        walk(v, path, depth + 1)
      end
    end
  end
end
walk(love, "love", 0)

-- OBJECT METHODS.
--
-- walk() only sees functions hanging off love.* TABLES, so every method on
-- an object it returns was invisible -- and the LOVE surface names plenty
-- of those (love.graphics.Canvas.renderTo, Canvas.getMSAA, ...). They were
-- being reported as missing while sitting right there on the metatable,
-- which is a false NEGATIVE: the opposite failure to counting an error()
-- stub as implemented, and just as misleading.
--
-- So: build one real instance of each object kind and enumerate what it
-- actually answers to. Constructing them can fail on a headless or
-- half-initialised engine, hence the pcall -- a probe that cannot build
-- the object simply reports nothing for it rather than taking the whole
-- enumeration down.
local function methodsOf(obj, prefix)
  if not obj then return end
  local mt = getmetatable(obj)
  local src = (mt and mt.__index) or obj
  if type(src) ~= "table" then return end
  local keys = {}
  for k, v in pairs(src) do
    if type(k) == "string" and type(v) == "function" then keys[#keys+1] = k end
  end
  table.sort(keys)
  for _, k in ipairs(keys) do out[#out+1] = prefix .. "." .. k end
end

local probes = {
  { "love.graphics.Canvas", function() return love.graphics.newCanvas(4, 4) end },
  { "love.graphics.Image",  function() return love.graphics.newCanvas(4, 4) end },
  { "love.graphics.Text",   function() return love.graphics.newText(love.graphics.newFont(12), "x") end },
  { "love.graphics.Quad",   function() return love.graphics.newQuad(0, 0, 1, 1, 2, 2) end },
  { "love.graphics.SpriteBatch", function()
      return love.graphics.newSpriteBatch(love.graphics.newCanvas(4, 4)) end },
  { "love.graphics.ParticleSystem", function()
      return love.graphics.newParticleSystem(love.graphics.newCanvas(4, 4), 8) end },
  { "love.graphics.Font",   function() return love.graphics.newFont(12) end },
}
for _, p in ipairs(probes) do
  local ok, obj = pcall(p[2])
  if ok then methodsOf(obj, p[1]) end
end

table.sort(out)
print("WCLUA_API_BEGIN")
for _, p in ipairs(out) do print(p) end
print("WCLUA_API_END " .. #out)
