-- ffi.lua - a pure-Lua 5.4 implementation of the LuaJIT `ffi` subset that
-- Lua game libraries actually use.
--
-- WHY THIS EXISTS. LOVE has always shipped LuaJIT, so a large part of the
-- ecosystem assumes `require("ffi")` works. This engine embeds PUC Lua 5.4,
-- which has no FFI at all -- and the libraries that want one do not guard
-- the call: 3DreamEngine requires ffi unconditionally in ten core files and
-- cannot load a single class without it.
--
-- WHAT THIS IS NOT. It is not a foreign function interface. There is no
-- C library to call into, no symbol resolution, and no JIT. What these
-- libraries actually use ffi FOR is typed, packed, indexable memory --
-- vertex buffers, index buffers, struct arrays -- and that is what this
-- provides, on top of Lua strings and tables.
--
-- The supported subset, which is exactly what the surveyed libraries call:
--
--   ffi.cdef("typedef struct { float x, y, z; } Foo;")
--   ffi.new("Foo[?]", n)          -> a zeroed array of n Foo
--   ffi.new("float[?]", n)        -> a zeroed array of n floats
--   ffi.sizeof("Foo")             -> bytes per element
--   ffi.cast("uint32_t*", data)   -> a typed view over a ByteData
--   ffi.copy(dst, src, bytes)
--   arr[i]                        -> scalar for a scalar array,
--                                    a field proxy for a struct array
--   arr[i].x = v                  -> writes into the backing bytes
--   arr + n                       -> a view offset by n ELEMENTS
--
-- Indexing is 0-BASED, matching C and therefore matching what the calling
-- code expects. That is the single most important detail here: a 1-based
-- implementation would appear to work and silently drop the first element
-- of every buffer.

local ffi = {}

-- The REAL Lua type(). The prelude installs a type() that reports LOVE
-- objects as "userdata" so library code type-checks correctly against them;
-- this shim works with those objects AS TABLES (a ByteData's byte store is a
-- plain field), so it must see through that.
local rawtype = rawget(_G, "rawtype") or type

-- ── the type table ──────────────────────────────────────────────────
--
-- Scalar types map to a string.pack format and a byte width. Struct types
-- are a list of fields, each with a name and a scalar type.

local SCALARS = {
  ["float"]          = { fmt = "f", size = 4 },
  ["double"]         = { fmt = "d", size = 8 },
  ["int"]            = { fmt = "i4", size = 4 },
  ["int32_t"]        = { fmt = "i4", size = 4 },
  ["uint32_t"]       = { fmt = "I4", size = 4 },
  ["int16_t"]        = { fmt = "i2", size = 2 },
  ["uint16_t"]       = { fmt = "I2", size = 2 },
  ["short"]          = { fmt = "i2", size = 2 },
  ["int8_t"]         = { fmt = "i1", size = 1 },
  ["uint8_t"]        = { fmt = "I1", size = 1 },
  ["char"]           = { fmt = "i1", size = 1 },
  ["unsigned char"]  = { fmt = "I1", size = 1 },
  ["unsigned int"]   = { fmt = "I4", size = 4 },
  ["unsigned short"] = { fmt = "I2", size = 2 },
}

local structs = {}    -- name -> { fields = {{name, type}, ...}, size = n }

-- Parse the ONE declaration form these libraries emit:
--     typedef struct { <type> <name>[, <name>...]; ... } <Identifier>;
-- Anything else is refused by name. A partial C parser that silently
-- mis-parses a declaration would produce a buffer with the wrong stride,
-- and wrong-stride geometry renders as noise rather than as an error.
function ffi.cdef(decl)
  -- Two spellings, both common:
  --     typedef struct { ... } Name;     -- the name is at the END
  --     struct Name { ... };             -- the name is at the FRONT, and
  --                                         the trailing ';' is optional
  -- g3d uses the second, 3DreamEngine the first. Supporting only one of
  -- them fails the other with a message about syntax the author did not
  -- write wrong.
  local body, name = decl:match("typedef%s+struct%s*{(.-)}%s*([%w_]+)%s*;?")
  if not body then
    name, body = decl:match("struct%s+([%w_]+)%s*{(.-)}")
  end
  if not body then
    error("ffi.cdef: this engine's ffi shim understands struct declarations " ..
          "-- 'typedef struct { ... } Name;' or 'struct Name { ... };' -- " ..
          "and nothing else. Got:\n" .. tostring(decl), 2)
  end
  local fields, size = {}, 0
  for stmt in body:gmatch("[^;]+") do
    stmt = stmt:match("^%s*(.-)%s*$")
    if #stmt > 0 then
      -- Longest type name first, so "unsigned char" is not read as "unsigned".
      local ctype, rest
      for t in pairs(SCALARS) do
        if stmt:sub(1, #t) == t and (not ctype or #t > #ctype) then
          ctype = t
        end
      end
      if not ctype then
        error("ffi.cdef: unsupported field type in '" .. stmt .. "'", 2)
      end
      rest = stmt:sub(#ctype + 1)
      for fname in rest:gmatch("[%w_]+") do
        fields[#fields + 1] = { name = fname, type = ctype }
        size = size + SCALARS[ctype].size
      end
    end
  end
  structs[name] = { fields = fields, size = size }
  return true
end

-- Resolve a type name to {size, fmt} for a scalar or {size, fields} for a
-- struct. Trailing "*" (a pointer type in a cast) names the POINTEE.
local function resolve(ctype)
  ctype = ctype:gsub("%s*%*%s*$", ""):match("^%s*(.-)%s*$")
  ctype = ctype:gsub("%[%?%]$", ""):match("^%s*(.-)%s*$")
  if SCALARS[ctype] then return { scalar = SCALARS[ctype], size = SCALARS[ctype].size } end
  if structs[ctype] then return { struct = structs[ctype], size = structs[ctype].size } end
  -- C lets a struct be named with the `struct` keyword, and code declared
  -- as `struct vertex { ... }` then refers to it as "struct vertex*".
  local bare = ctype:match("^struct%s+([%w_]+)$")
  if bare and structs[bare] then
    return { struct = structs[bare], size = structs[bare].size }
  end
  return nil, ctype
end

function ffi.sizeof(ctype, n)
  local t, missing = resolve(ctype)
  if not t then
    error("ffi.sizeof: unknown type '" .. tostring(missing) .. "'", 2)
  end
  return t.size * (n or 1)
end

-- ── the buffer ──────────────────────────────────────────────────────
--
-- Backing storage is a table of bytes (integers 0..255), not a Lua string:
-- these buffers are written element by element, and a string would mean an
-- allocation per write. A byte table with string.pack/unpack at the edges
-- is O(1) per field write.

local Array = {}

local function readField(self, byteOffset, ctype)
  local sc = SCALARS[ctype]
  local bytes = {}
  for i = 1, sc.size do bytes[i] = self.data[byteOffset + i] or 0 end
  return (string.unpack("<" .. sc.fmt, string.char(table.unpack(bytes))))
end

local function writeField(self, byteOffset, ctype, value)
  local sc = SCALARS[ctype]
  -- Integer formats reject a float, which is exactly the case a caller hits
  -- when it stores a computed index; floor rather than error, matching what
  -- a C assignment does.
  if sc.fmt ~= "f" and sc.fmt ~= "d" then value = math.floor(value or 0) end
  local packed = string.pack("<" .. sc.fmt, value or 0)
  for i = 1, sc.size do
    self.data[byteOffset + i] = packed:byte(i)
  end
end

-- A proxy for one struct ELEMENT, so `arr[i].x = v` writes through to the
-- backing bytes instead of into a temporary copy. Created on access; these
-- are short-lived and the alternative (materializing every element up
-- front) would defeat the point of a packed buffer.
local Element = {}
Element.__index = function(self, key)
  local f = self._fields[key]
  if not f then return nil end
  return readField(self._arr, self._base + f.offset, f.type)
end
Element.__newindex = function(self, key, value)
  local f = self._fields[key]
  if not f then
    error("ffi: struct has no field '" .. tostring(key) .. "'", 2)
  end
  writeField(self._arr, self._base + f.offset, f.type, value)
end

Array.__index = function(self, key)
  if rawtype(key) ~= "number" then return Array[key] end
  -- 0-BASED, as in C.
  local base = self.offset + key * self.type.size
  if self.type.scalar then
    return readField(self, base, self.ctype)
  end
  return setmetatable({ _arr = self, _base = base, _fields = self.fieldmap },
                      Element)
end

Array.__newindex = function(self, key, value)
  if rawtype(key) ~= "number" then rawset(self, key, value) return end
  local base = self.offset + key * self.type.size
  if self.type.scalar then
    writeField(self, base, self.ctype, value)
    return
  end
  error("ffi: cannot assign a whole struct element; assign its fields", 2)
end

-- `ptr + n` advances by n ELEMENTS, which is C pointer arithmetic and what
-- ffi.copy(self.buffer + dstOffset, ...) relies on. The result shares the
-- same backing store, so writes through it are visible through the original.
Array.__add = function(a, n)
  return setmetatable({
    data = a.data, offset = a.offset + n * a.type.size,
    type = a.type, ctype = a.ctype, fieldmap = a.fieldmap,
    count = a.count - n,
  }, Array)
end

local function fieldmap_of(t)
  if not t.struct then return nil end
  local m, off = {}, 0
  for _, f in ipairs(t.struct.fields) do
    m[f.name] = { offset = off, type = f.type }
    off = off + SCALARS[f.type].size
  end
  return m
end

local function new_array(ctype, count, data, offset)
  local t, missing = resolve(ctype)
  if not t then
    error("ffi: unknown type '" .. tostring(missing) .. "'", 3)
  end
  local arr = setmetatable({
    data = data or {},
    offset = offset or 0,
    type = t,
    ctype = ctype:gsub("%s*%*%s*$", ""):gsub("%[%?%]$", ""):match("^%s*(.-)%s*$"),
    fieldmap = fieldmap_of(t),
    count = count,
  }, Array)
  if not data then
    -- Zero-fill: C's calloc semantics, and callers rely on a fresh buffer
    -- being zero (an index buffer left as nil would index element 0).
    for i = 1, count * t.size do arr.data[i] = 0 end
  end
  return arr
end

-- ffi.new("Foo[?]", n) | ffi.new("float[?]", n) | ffi.new("Foo")
function ffi.new(ctype, count)
  return new_array(ctype, count or 1)
end

-- ffi.cast("uint32_t*", byteDataOrArray)
--
-- A VIEW, not a copy -- writes through the cast must be visible in the
-- original, which is the whole reason the calling code casts rather than
-- reads. A ByteData from love.data.newByteData carries `_bytes`, the same
-- table an Array uses, so the two are already the same storage.
function ffi.cast(ctype, src)
  if rawtype(src) == "table" and getmetatable(src) == Array then
    return new_array(ctype, src.count, src.data, src.offset)
  end
  if rawtype(src) == "table" and src._bytes then
    local t = resolve(ctype)
    return new_array(ctype, t and (#src._bytes // t.size) or 0, src._bytes, 0)
  end
  error("ffi.cast: expected a ByteData or an ffi array, got " .. rawtype(src), 2)
end

function ffi.copy(dst, src, len)
  local d = (getmetatable(dst) == Array) and dst or nil
  local s = (getmetatable(src) == Array) and src or nil
  if not d then error("ffi.copy: destination must be an ffi array", 2) end
  if not s then
    -- copying from a ByteData
    if rawtype(src) == "table" and src._bytes then
      for i = 1, len do d.data[d.offset + i] = src._bytes[i] or 0 end
      return
    end
    error("ffi.copy: source must be an ffi array or a ByteData", 2)
  end
  for i = 1, len do
    d.data[d.offset + i] = s.data[s.offset + i] or 0
  end
end

-- ffi.string / ffi.fill round out the surface for callers that reach for
-- them; both are straightforward over the byte table.
function ffi.string(arr, len)
  local out = {}
  len = len or arr.count * arr.type.size
  for i = 1, len do out[i] = string.char(arr.data[arr.offset + i] or 0) end
  return table.concat(out)
end

function ffi.fill(arr, len, value)
  value = value or 0
  for i = 1, len do arr.data[arr.offset + i] = value end
end

-- Present so a caller can branch on the runtime rather than crash on a nil
-- index. Reporting the truth matters: code that special-cases LuaJIT should
-- take its non-JIT path here.
ffi.os = "Other"
ffi.arch = "wasm32"
ffi.abi = function() return false end

-- ffi.C is the C library namespace. There is none; any access is a hard
-- error rather than a nil that would be called.
ffi.C = setmetatable({}, {
  __index = function(_, k)
    error("ffi.C." .. tostring(k) .. ": there is no C library to call into " ..
          "from a cart. This engine's ffi provides typed memory only.", 2)
  end,
})

return ffi
