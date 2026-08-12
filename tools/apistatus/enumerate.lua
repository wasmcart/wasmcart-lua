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
table.sort(out)
print("WCLUA_API_BEGIN")
for _, p in ipairs(out) do print(p) end
print("WCLUA_API_END " .. #out)
