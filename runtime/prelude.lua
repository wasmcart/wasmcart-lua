--[[
  prelude.lua - the LOVE-style Lua API surface for the wasmcart Lua engine.

  Embedded into the engine wasm at build time. Games ship only their own Lua.
  An app/prelude.lua asset overrides this file entirely (a hacking hook).

  NOT LOVE: this is an unaffiliated engine with a LOVE-STYLE api. Source
  familiarity is the goal; compatibility is not claimed. Where behavior
  deliberately differs (fixed dt, no real filesystem, no threads/shaders in
  v1) the stub fails loudly with what to use instead.

  Coordinates are top-left origin, y grows DOWN, like LOVE.
  Colors are 0..1 floats, like LOVE 11+.
]]

local W, H = __WC_WIDTH, __WC_HEIGHT
__WC_WIDTH, __WC_HEIGHT = nil, nil

love = {}
local love = love

-- ── internal state ─────────────────────────────────────────────────
local FIXED_DT = 1 / 60
local frame_n = 0

-- transform stack (Lua-side; C is a dumb rasterizer in world coords)
local tx, ty, tsx, tsy, trot = 0, 0, 1, 1, 0
local tstack = {}

local function apply(x, y)
  x, y = x * tsx, y * tsy
  if trot ~= 0 then
    local c, s = math.cos(trot), math.sin(trot)
    x, y = x * c - y * s, x * s + y * c
  end
  return x + tx, y + ty
end

-- current color as 0..255 ints for the C layer
local cr, cg, cb, ca = 255, 255, 255, 255

local function err(name, why)
  return function()
    error(name .. " is not available in this engine: " .. why, 2)
  end
end

-- ── love.graphics ──────────────────────────────────────────────────
local graphics = {}
love.graphics = graphics

local bg = { 0, 0, 0 }
local cur_font = nil

function graphics.setColor(r, g, b, a)
  if type(r) == "table" then r, g, b, a = r[1], r[2], r[3], r[4] end
  cr = math.floor((r or 0) * 255 + 0.5)
  cg = math.floor((g or 0) * 255 + 0.5)
  cb = math.floor((b or 0) * 255 + 0.5)
  ca = math.floor((a or 1) * 255 + 0.5)
  wc.set_color(cr, cg, cb, ca)
end

function graphics.getColor()
  return cr / 255, cg / 255, cb / 255, ca / 255
end

function graphics.setBackgroundColor(r, g, b)
  if type(r) == "table" then r, g, b = r[1], r[2], r[3] end
  bg[1], bg[2], bg[3] = r or 0, g or 0, b or 0
end

function graphics.getBackgroundColor() return bg[1], bg[2], bg[3] end

function graphics.clear(r, g, b)
  if r then
    if type(r) == "table" then r, g, b = r[1], r[2], r[3] end
    wc.clear(math.floor(r * 255 + 0.5), math.floor((g or 0) * 255 + 0.5),
             math.floor((b or 0) * 255 + 0.5))
  else
    wc.clear(math.floor(bg[1] * 255 + 0.5), math.floor(bg[2] * 255 + 0.5),
             math.floor(bg[3] * 255 + 0.5))
  end
end

function graphics.getWidth()  return W end
function graphics.getHeight() return H end
function graphics.getDimensions() return W, H end

-- rectangle("fill"|"line", x, y, w, h)
--
-- Under rotation a rect is no longer axis-aligned, so it cannot go through
-- the fast wc.rect path (which takes x/y/w/h and knows nothing about the
-- transform). Rotated rects are emitted as polygons -- correct at any angle,
-- and still the cheap path when trot == 0, which is the common case.
function graphics.rectangle(mode, x, y, w, h)
  if trot ~= 0 then
    local x0, y0 = apply(x, y)
    local x1, y1 = apply(x + w, y)
    local x2, y2 = apply(x + w, y + h)
    local x3, y3 = apply(x, y + h)
    wc.polygon(mode == "fill" and 1 or 0, { x0, y0, x1, y1, x2, y2, x3, y3 })
    return
  end
  local px, py = apply(x, y)
  wc.rect(mode == "fill" and 1 or 0, px, py, w * tsx, h * tsy)
end

-- circles are rotation-invariant about their center, so only the center
-- needs transforming
function graphics.circle(mode, x, y, r)
  local px, py = apply(x, y)
  wc.circle(mode == "fill" and 1 or 0, px, py, r * tsx)
end

function graphics.line(...)
  local pts = ...
  if type(pts) ~= "table" then pts = { ... } end
  for i = 1, #pts - 3, 2 do
    local x1, y1 = apply(pts[i], pts[i + 1])
    local x2, y2 = apply(pts[i + 2], pts[i + 3])
    wc.line(x1, y1, x2, y2)
  end
end

function graphics.points(...)
  local pts = ...
  if type(pts) ~= "table" then pts = { ... } end
  for i = 1, #pts - 1, 2 do
    local x, y = apply(pts[i], pts[i + 1])
    wc.point(x, y)
  end
end

function graphics.polygon(mode, ...)
  local pts = ...
  if type(pts) ~= "table" then pts = { ... } end
  local out = {}
  for i = 1, #pts - 1, 2 do
    local x, y = apply(pts[i], pts[i + 1])
    out[#out + 1] = x
    out[#out + 1] = y
  end
  wc.polygon(mode == "fill" and 1 or 0, out)
end

-- Forward declaration: graphics.draw() dispatches on SpriteBatch, which is
-- defined further down. Without this the reference inside draw would bind
-- to a global (nil) instead of the local table.
local SpriteBatch

-- Image / Quad / Canvas objects
local Image = {}
Image.__index = Image
function Image:getWidth()  return self.w end
function Image:getHeight() return self.h end
function Image:getDimensions() return self.w, self.h end
function Image:type() return "Image" end
-- Texture sampling knobs are GPU concepts. The software renderer samples
-- nearest-neighbour always (correct for pixel art, which is what carts
-- ship), so these are accepted and ignored rather than erroring: map and
-- sprite libraries call setFilter unconditionally on every texture.
function Image:setFilter() end
function Image:getFilter() return "nearest", "nearest" end
function Image:setWrap() end
function Image:getWrap() return "clamp", "clamp" end
function Image:release() return true end

local Quad = {}
Quad.__index = Quad
function Quad:getViewport() return self.x, self.y, self.w, self.h end
function Quad:setViewport(x, y, w, h) self.x, self.y, self.w, self.h = x, y, w, h end
function Quad:type() return "Quad" end

-- newImage(path) | newImage(imageData) - LOVE accepts either, and map
-- loaders routinely pass an ImageData they made earlier.
function graphics.newImage(src)
  if type(src) == "table" then
    if src._img then return src._img end          -- our ImageData wrapper
    error("love.graphics.newImage: unsupported table argument", 2)
  end
  local id, w, h = wc.image_load(src)
  if not id then error("could not load image: " .. tostring(src), 2) end
  return setmetatable({ id = id, w = w, h = h }, Image)
end

function graphics.newQuad(x, y, w, h, sw, sh)
  return setmetatable({ x = x, y = y, w = w, h = h, sw = sw, sh = sh }, Quad)
end

function graphics.newCanvas(w, h)
  local id = wc.canvas_new(w or W, h or H)
  if not id then error("could not create canvas", 2) end
  return setmetatable({ id = id, w = w or W, h = h or H, canvas = true }, Image)
end

function graphics.setCanvas(c)
  if c then wc.set_canvas(c.id) else wc.set_canvas(nil) end
end

-- draw(image, [quad], x, y, r, sx, sy, ox, oy)
function graphics.draw(img, a, b, c, d, e, f, g, h)
  if not img then return end

  -- a SpriteBatch replays its entries, offset by the batch's own transform
  if getmetatable(img) == SpriteBatch then
    local bx, by = a or 0, b or 0
    local batchImg = img.image
    if not batchImg or not batchImg.id then return end
    for i = 1, img.n do
      local it = img.items[i]
      local px, py = apply(it.x + bx, it.y + by)
      local qx, qy, qw, qh = 0, 0, 0, 0
      if it.quad then qx, qy, qw, qh = it.quad.x, it.quad.y, it.quad.w, it.quad.h end
      wc.image_draw(batchImg.id, px, py, it.r + trot,
                    it.sx * tsx, it.sy * tsy, it.ox, it.oy, qx, qy, qw, qh)
    end
    return
  end

  if not img.id then return end
  local quad, x, y, r, sx, sy, ox, oy
  if type(a) == "table" and a.getViewport then
    quad = a; x, y, r, sx, sy, ox, oy = b, c, d, e, f, g, h
  else
    x, y, r, sx, sy, ox, oy = a, b, c, d, e, f, g
  end
  x = x or 0; y = y or 0; r = r or 0
  sx = sx or 1; sy = sy or sx; ox = ox or 0; oy = oy or 0
  local px, py = apply(x, y)
  local qx, qy, qw, qh = 0, 0, 0, 0
  if quad then qx, qy, qw, qh = quad.x, quad.y, quad.w, quad.h end
  wc.image_draw(img.id, px, py, r + trot, sx * tsx, sy * tsy, ox, oy, qx, qy, qw, qh)
end

-- SpriteBatch
--
-- In LOVE this is a GPU optimization: many quads of one texture uploaded
-- as a single draw call. The software renderer has no per-draw GPU cost to
-- amortize, so here a batch is simply a retained list of (quad, transform)
-- entries that replays on draw. Same API, same visual result, and tile-map
-- libraries depend on it heavily.
SpriteBatch = {}
SpriteBatch.__index = SpriteBatch

function SpriteBatch:add(a, b, c, d, e, f, g, h, i)
  local quad, x, y, r, sx, sy, ox, oy
  if type(a) == "table" and a.getViewport then
    quad = a; x, y, r, sx, sy, ox, oy = b, c, d, e, f, g, h
  else
    x, y, r, sx, sy, ox, oy = a, b, c, d, e, f, g
  end
  self.n = self.n + 1
  self.items[self.n] = { quad = quad, x = x or 0, y = y or 0, r = r or 0,
                         sx = sx or 1, sy = sy or sx or 1, ox = ox or 0, oy = oy or 0 }
  return self.n
end

function SpriteBatch:set(idx, a, b, c, d, e, f, g, h)
  local it = self.items[idx]
  if not it then return end
  local quad, x, y, r, sx, sy, ox, oy
  if type(a) == "table" and a.getViewport then
    quad = a; x, y, r, sx, sy, ox, oy = b, c, d, e, f, g, h
  else
    x, y, r, sx, sy, ox, oy = a, b, c, d, e, f, g
  end
  it.quad = quad or it.quad
  it.x, it.y, it.r = x or 0, y or 0, r or 0
  it.sx, it.sy = sx or 1, sy or sx or 1
  it.ox, it.oy = ox or 0, oy or 0
end

function SpriteBatch:clear() self.items, self.n = {}, 0 end
function SpriteBatch:getCount() return self.n end
function SpriteBatch:setTexture(img) self.image = img end
function SpriteBatch:getTexture() return self.image end
function SpriteBatch:flush() end            -- nothing is buffered on a GPU
function SpriteBatch:setColor() end         -- per-sprite tint: not supported
function SpriteBatch:type() return "SpriteBatch" end

function graphics.newSpriteBatch(img, maxsprites)
  return setmetatable({ image = img, items = {}, n = 0 }, SpriteBatch)
end

-- Font
local Font = {}
Font.__index = Font
function Font:getHeight() return self.px end
function Font:getWidth(s) local w = wc.text_size(s, self.id, self.scale) return w end
function Font:type() return "Font" end

function graphics.newFont(a, b)
  -- newFont(path, size) | newFont(size) -> scaled bitfont.
  -- Sizes are rounded: games compute them from screen scale, so a
  -- fractional size is normal and must not be an error.
  if type(a) == "string" then
    local px = math.max(1, math.floor((tonumber(b) or 12) + 0.5))
    local id, real = wc.font_load(a, px)
    if not id then error("could not load font: " .. tostring(a), 2) end
    return setmetatable({ id = id, px = real, scale = 1 }, Font)
  end
  local size = tonumber(a) or 12
  local scale = math.max(1, math.floor(size / 8 + 0.5))
  return setmetatable({ id = -1, px = 8 * scale, scale = scale }, Font)
end

function graphics.setFont(f) cur_font = f end
function graphics.getFont()
  if not cur_font then cur_font = graphics.newFont(12) end
  return cur_font
end

function graphics.print(text, x, y)
  local f = graphics.getFont()
  local px, py = apply(x or 0, y or 0)
  wc.print(tostring(text), px, py, f.id, f.scale)
end

-- printf(text, x, y, limit, align)
--
-- `limit` is a WRAP WIDTH, not just an alignment box: LOVE breaks the text
-- into lines no wider than `limit` and then aligns each line. Treating it
-- as alignment-only makes multi-line messages draw on top of each other.
function graphics.printf(text, x, y, limit, align)
  local f = graphics.getFont()
  text = tostring(text)
  limit = limit or W
  align = align or "left"

  -- greedy word wrap at `limit`, honoring explicit newlines
  local lines = {}
  for paragraph in (text .. "\n"):gmatch("([^\n]*)\n") do
    if paragraph == "" then
      lines[#lines + 1] = ""
    else
      local cur = nil
      for word in paragraph:gmatch("%S+") do
        local try = cur and (cur .. " " .. word) or word
        if wc.text_size(try, f.id, f.scale) <= limit or not cur then
          cur = try
        else
          lines[#lines + 1] = cur
          cur = word
        end
      end
      if cur then lines[#lines + 1] = cur end
    end
  end

  -- Line advance must be applied in SCREEN space, not world space. The
  -- glyphs are rasterized at a fixed pixel height regardless of the
  -- transform, so folding lineH into the pre-transform y makes the gap
  -- shrink with the scale while the text does not -- lines then overlap
  -- under a zoomed-out camera. Transform the line origin, then step down
  -- by the font's true pixel height.
  local lineH = f.px * 1.2
  for i, line in ipairs(lines) do
    local lw = wc.text_size(line, f.id, f.scale)
    local px = x or 0
    if align == "center" then px = px + (limit - lw) / 2
    elseif align == "right" then px = px + limit - lw end
    local ax, ay = apply(px, y or 0)
    wc.print(line, ax, ay + (i - 1) * lineH, f.id, f.scale)
  end
end

-- transform stack
function graphics.push()
  tstack[#tstack + 1] = { tx, ty, tsx, tsy, trot }
end

function graphics.pop()
  local t = table.remove(tstack)
  if t then tx, ty, tsx, tsy, trot = t[1], t[2], t[3], t[4], t[5] end
end

-- translate composes THROUGH the current rotation+scale, so a translate
-- after a rotate moves along the rotated axes (matrix order, like LOVE).
function graphics.translate(x, y)
  local dx, dy = apply(x or 0, y or 0)
  tx, ty = dx, dy
end
function graphics.scale(sx, sy) tsx = tsx * (sx or 1); tsy = tsy * (sy or sx or 1) end
function graphics.rotate(r) trot = trot + (r or 0) end
function graphics.origin() tx, ty, tsx, tsy, trot = 0, 0, 1, 1, 0 end

local sc_rect = nil
function graphics.setScissor(x, y, w, h)
  if x then
    sc_rect = { x, y, w, h }
    wc.set_scissor(x, y, w, h)
  else
    sc_rect = nil
    wc.set_scissor(nil)
  end
end

-- Camera libraries save the scissor, clip, then restore it, so the getter
-- has to return what was actually set (nil when unclipped).
function graphics.getScissor()
  if not sc_rect then return nil end
  return sc_rect[1], sc_rect[2], sc_rect[3], sc_rect[4]
end

-- Line width is not implemented by the rasterizer (all lines are 1px), but
-- games set it constantly. Store and report it so round-trips are honest
-- rather than erroring mid-draw.
local line_width = 1
function graphics.setLineWidth(w) line_width = w or 1 end
function graphics.getLineWidth() return line_width end
function graphics.setLineStyle() end
function graphics.getLineStyle() return "rough" end
function graphics.setLineJoin() end

function graphics.setBlendMode(mode)
  wc.set_blend(mode == "add" and 1 or 0)
end

-- deliberate v1 cuts, each loud
graphics.newShader   = err("love.graphics.newShader", "shaders arrive with the GL renderer; use canvases and tinting for now")
graphics.setShader   = err("love.graphics.setShader", "shaders arrive with the GL renderer")
graphics.newMesh     = err("love.graphics.newMesh", "meshes arrive with the GL renderer; use polygon() for now")
graphics.newVideo    = err("love.graphics.newVideo", "video playback is out of scope for this engine")

-- ── love.audio ─────────────────────────────────────────────────────
local audio = {}
love.audio = audio

local Source = {}
Source.__index = Source
function Source:play()
  self.ch = wc.sound_play(self.id, self.vol, self.loop and 1 or 0)
  if self.pitch ~= 1 and self.ch then wc.sound_pitch(self.ch, self.pitch) end
  return self.ch
end
function Source:stop() if self.ch then wc.sound_stop(self.ch) end end
function Source:pause() if self.ch then wc.sound_paused(self.ch, 1) end end
function Source:resume() if self.ch then wc.sound_paused(self.ch, 0) end end
function Source:setLooping(v) self.loop = v and true or false end
function Source:isLooping() return self.loop end
function Source:setVolume(v)
  self.vol = v
  if self.ch then wc.sound_gain(self.ch, v) end
end
function Source:getVolume() return self.vol end
function Source:setPitch(p)
  self.pitch = p
  if self.ch then wc.sound_pitch(self.ch, p) end
end
function Source:getPitch() return self.pitch end
function Source:isPlaying() return self.ch and wc.sound_playing(self.ch) or false end
function Source:seek(t) if self.ch then wc.sound_seek(self.ch, t) end end
function Source:tell() return self.ch and wc.sound_playtime(self.ch) or 0 end
function Source:type() return "Source" end

function audio.newSource(path, kind)
  local id = wc.sound_load(path)
  if not id then error("could not load sound: " .. tostring(path), 2) end
  return setmetatable({ id = id, vol = 1, pitch = 1, loop = false, kind = kind or "static" }, Source)
end

function audio.play(s) if s then s:play() end end
function audio.stop(s) if s then s:stop() end end
function audio.beep(freq, vol) wc.beep(freq or 440, vol or 0.8) end

-- ── love.keyboard / gamepad ────────────────────────────────────────
-- wasmcart is a cartridge console: the GAMEPAD is the primary input.
-- Desktop hosts map arrows/z/x onto the pad so keyboard testing works.

local BTN = {
  a = 1, b = 2, x = 4, y = 8,
  l = 16, r = 32, start = 64, select = 128,
  up = 256, down = 512, left = 1024, right = 2048,
  l3 = 4096, r3 = 8192,
}

local pads = {}
for i = 1, 4 do
  pads[i] = { buttons = 0, prev = 0, lx = 0, ly = 0, rx = 0, ry = 0 }
end

local function has(mask, name) return (mask & BTN[name]) ~= 0 end

local keyboard = {}
love.keyboard = keyboard

-- keyboard names mapped onto pad 1 (desktop-dev convenience)
-- Keyboard names mapped onto pad 1. WASD is included because games treat
-- it as a second d-pad and frequently gate progress on it (Cavern advances
-- its intro text only while one of space/return/w/a/s/d is held) -- an
-- incomplete map leaves such a game silently stuck rather than erroring.
local KEYMAP = {
  left = "left", right = "right", up = "up", down = "down",
  a = "left", d = "right", w = "up", s = "down",
  z = "a", x = "b", c = "x", v = "y",
  space = "a", ["return"] = "start", enter = "start",
  escape = "select", backspace = "select", tab = "select",
  lshift = "l", rshift = "r", lctrl = "l", rctrl = "r",
  up2 = "up",
}

function keyboard.isDown(...)
  local p = pads[1]
  for _, k in ipairs({ ... }) do
    local btn = KEYMAP[k]
    if btn and has(p.buttons, btn) then return true end
  end
  return false
end

local joystick = {}
love.joystick = joystick

local Joystick = {}
Joystick.__index = Joystick
function Joystick:isGamepadDown(...)
  local p = pads[self.n]
  for _, b in ipairs({ ... }) do
    if BTN[b] and has(p.buttons, b) then return true end
  end
  return false
end
function Joystick:getGamepadAxis(axis)
  local p = pads[self.n]
  if axis == "leftx" then return p.lx / 32767 end
  if axis == "lefty" then return p.ly / 32767 end
  if axis == "rightx" then return p.rx / 32767 end
  if axis == "righty" then return p.ry / 32767 end
  return 0
end
function Joystick:isConnected() return true end
function Joystick:getID() return self.n end
function Joystick:type() return "Joystick" end

local joy_objs = {}
for i = 1, 4 do joy_objs[i] = setmetatable({ n = i }, Joystick) end

function joystick.getJoysticks() return joy_objs end
function joystick.getJoystickCount() return 4 end

-- the idiomatic wasmcart surface: love.pad
local pad = {}
love.pad = pad
function pad.isDown(n, b)
  if type(n) == "string" then n, b = 1, n end
  return has(pads[n].buttons, b)
end
function pad.wasPressed(n, b)
  if type(n) == "string" then n, b = 1, n end
  local p = pads[n]
  return has(p.buttons, b) and not has(p.prev, b)
end
function pad.wasReleased(n, b)
  if type(n) == "string" then n, b = 1, n end
  local p = pads[n]
  return not has(p.buttons, b) and has(p.prev, b)
end
function pad.axis(n, which)
  if type(n) == "string" then n, which = 1, n end
  local p = pads[n]
  local v = (which == "leftx" and p.lx) or (which == "lefty" and p.ly)
         or (which == "rightx" and p.rx) or (which == "righty" and p.ry) or 0
  return v / 32767
end

-- ── rumble ─────────────────────────────────────────────────────────
--
-- LOVE's Joystick:setVibration idiom: left/right motor strength 0..1 and a
-- duration in SECONDS. The wasmcart ABI wants milliseconds and 0-based pad
-- ids, so both conversions happen here and nowhere else.
--
-- Capability is per-DEVICE (a keyboard-only setup has none), so ask with
-- hasVibration rather than assuming. Calls to a pad without motors are
-- silent no-ops, so skipping the query is safe if wasteful. The host stops
-- the motors on its own timer; for sustained rumble, re-arm each frame.
local RUMBLE_MAX_SEC = 5.0   -- WC_RUMBLE_MAX_MS; the host caps this too

-- last strengths handed to each pad, so getVibration can report them the way
-- LOVE does (the ABI is write-only, the host never reports motor state back)
local vibration = {}
for i = 1, 4 do vibration[i] = { 0, 0 } end

function pad.hasVibration(n)
  return wc.pad_has_rumble((n or 1) - 1)
end

-- setVibration(left, right, duration) targets pad 1, matching LOVE.
-- setVibration(n, left, right, duration) names the pad.
--
-- Both forms are all-numbers, so unlike pad.isDown there is no type to
-- dispatch on: the pad number is recognised by ARGUMENT COUNT, and the
-- explicit form therefore has to pass a duration (0 = the host's cap).
function pad.setVibration(...)
  local argc = select("#", ...)
  local pn, left, right, dur
  if argc >= 4 then
    pn, left, right, dur = ...
  else
    pn = 1
    left, right, dur = ...
  end
  left, right, dur = left or 0, right or 0, dur or 0
  if type(pn) ~= "number" or pn < 1 or pn > 4 then return false end
  left = math.max(0, math.min(1, left))
  right = math.max(0, math.min(1, right))
  if left <= 0 and right <= 0 then
    wc.pad_rumble_stop(pn - 1)
    vibration[pn][1], vibration[pn][2] = 0, 0
    return true
  end
  if dur <= 0 or dur > RUMBLE_MAX_SEC then dur = RUMBLE_MAX_SEC end
  wc.pad_rumble(pn - 1, left, right, math.floor(dur * 1000 + 0.5))
  vibration[pn][1], vibration[pn][2] = left, right
  return true
end

function pad.stopVibration(n)
  n = n or 1
  wc.pad_rumble_stop(n - 1)
  if vibration[n] then vibration[n][1], vibration[n][2] = 0, 0 end
end

function pad.getVibration(n)
  local v = vibration[n or 1]
  if not v then return 0, 0 end
  return v[1], v[2]
end

function Joystick:setVibration(l, r, d)
  return pad.setVibration(self.n, l or 0, r or 0, d or 0)
end
function Joystick:isVibrationSupported() return pad.hasVibration(self.n) end
function Joystick:getVibration() return pad.getVibration(self.n) end

-- ── love.mouse ─────────────────────────────────────────────────────
--
-- Backed by the wasmcart pointer ABI (unified mouse/touch), so it works on
-- hosts that have a pointer. On a pure-gamepad host the pointer is simply
-- inactive and the right stick drives a virtual cursor instead -- games
-- that aim with the mouse stay playable on a controller.
local mouse = {}
love.mouse = mouse

local vcursor_x, vcursor_y = W / 2, H / 2
local prev_mouse = 0   -- last frame's pointer button mask, for edge detection
local VCURSOR_SPEED = 12

function mouse.getPosition()
  local px, py, _, active = wc.pointer(0)
  if active then return px, py end
  return vcursor_x, vcursor_y
end

function mouse.getX() local x = mouse.getPosition() return x end
function mouse.getY() local _, y = mouse.getPosition() return y end

function mouse.isDown(...)
  local _, _, buttons, active = wc.pointer(0)
  for _, b in ipairs({ ... }) do
    if active and (buttons & (1 << (b - 1))) ~= 0 then return true end
    -- gamepad fallback: R trigger is "fire", mirroring button 1
    if b == 1 and pads[1] and (pads[1].buttons & BTN.r) ~= 0 then return true end
  end
  return false
end

function mouse.setVisible() end
function mouse.isVisible() return true end
function mouse.setGrabbed() end

-- advanced once per frame by the frame driver
local function update_vcursor()
  local p = pads[1]
  if not p then return end
  local ax, ay = p.rx / 32767, p.ry / 32767
  if math.abs(ax) > 0.15 then vcursor_x = vcursor_x + ax * VCURSOR_SPEED end
  if math.abs(ay) > 0.15 then vcursor_y = vcursor_y + ay * VCURSOR_SPEED end
  vcursor_x = math.max(0, math.min(W, vcursor_x))
  vcursor_y = math.max(0, math.min(H, vcursor_y))
end

-- ── love.math ──────────────────────────────────────────────────────
local lmath = {}
love.math = lmath

-- deterministic: the ONLY entropy is the host seed
function lmath.random(a, b)
  local r = wc.rand()
  if not a then return r end
  if not b then return math.floor(r * a) + 1 end
  return math.floor(r * (b - a + 1)) + a
end

function lmath.setRandomSeed() end -- host owns the seed; no-op by design

-- An independent generator object. Seeded deterministically (xorshift32)
-- so two carts given the same seed produce the same stream, which the
-- engine's replay guarantee depends on.
local RNG = {}
RNG.__index = RNG
function RNG:random(a, b)
  local x = self.s
  x = (x ~ (x << 13)) & 0xFFFFFFFF
  x = x ~ (x >> 17)
  x = (x ~ (x << 5)) & 0xFFFFFFFF
  self.s = x == 0 and 2463534242 or x
  local r = (self.s >> 8) / 16777216.0
  if not a then return r end
  if not b then return math.floor(r * a) + 1 end
  return math.floor(r * (b - a + 1)) + a
end
function RNG:setSeed(s) self.s = (math.floor(s or 1) & 0xFFFFFFFF) | 1 end
function RNG:getSeed() return self.s end
function RNG:randomNormal(sd, mean)
  -- Box-Muller, so libraries expecting a gaussian get one
  local u1 = math.max(1e-12, self:random())
  local u2 = self:random()
  return math.sqrt(-2 * math.log(u1)) * math.cos(2 * math.pi * u2) * (sd or 1) + (mean or 0)
end
function RNG:type() return "RandomGenerator" end

function lmath.newRandomGenerator(seed)
  local r = setmetatable({}, RNG)
  r:setSeed(seed or 12345)
  return r
end

-- value noise (deterministic, seed-independent)
local function fade(t) return t * t * t * (t * (t * 6 - 15) + 10) end
local function hash2(x, y)
  local n = x * 374761393 + y * 668265263
  n = (n ~ (n >> 13)) * 1274126177
  return ((n ~ (n >> 16)) % 65536) / 65536
end
function lmath.noise(x, y)
  y = y or 0
  local x0, y0 = math.floor(x), math.floor(y)
  local fx, fy = fade(x - x0), fade(y - y0)
  local a, b = hash2(x0, y0), hash2(x0 + 1, y0)
  local c, d = hash2(x0, y0 + 1), hash2(x0 + 1, y0 + 1)
  local top = a + (b - a) * fx
  local bot = c + (d - c) * fx
  return top + (bot - top) * fy
end

-- override the stdlib RNG too so math.random is deterministic as well
math.random = lmath.random
math.randomseed = function() end

-- ── love.timer ─────────────────────────────────────────────────────
local timer = {}
love.timer = timer
function timer.getTime() return frame_n * FIXED_DT end
function timer.getDelta() return FIXED_DT end
function timer.getFPS() return 60 end
function timer.step() end
function timer.sleep() end

-- ── love.filesystem ────────────────────────────────────────────────
-- Reads come from the cart's asset bundle. Writes go to the wc save region.
local fs = {}
love.filesystem = fs

function fs.read(path)
  local s = wc.asset_read(path)
  if not s then return nil, "could not open file " .. tostring(path) end
  return s, #s
end

function fs.getInfo(path)
  if wc.asset_exists(path) then return { type = "file" } end
  return nil
end

fs.exists = function(path) return wc.asset_exists(path) end

function fs.lines(path)
  local s = wc.asset_read(path)
  if not s then error("could not open file " .. tostring(path), 2) end
  return s:gmatch("([^\n]*)\n?")
end

-- save data: one blob, size-capped, persisted by the host
function fs.write(_, data) return wc.save_write(tostring(data)) end
function fs.load_save() return wc.save_read() end

-- love.filesystem.load: compile a cart asset into a chunk. Tiled map
-- loaders (STI) use this to pull .lua map files, so it is load-bearing for
-- any game with external levels.
function fs.load(path)
  local src = wc.asset_read(path)
  if not src then return nil, "could not open file " .. tostring(path) end
  return load(src, "@" .. path)
end

fs.newFile   = err("love.filesystem.newFile", "carts have no real filesystem; use love.filesystem.read / write")
fs.mount     = err("love.filesystem.mount", "carts have no real filesystem")

-- ── love.window / love.event / love.system ─────────────────────────
love.window = {
  getWidth = function() return W end,
  getHeight = function() return H end,
  getDimensions = function() return W, H end,
  setTitle = function() end,
  setMode = function() return true end,
  getMode = function() return W, H, {} end,
  setIcon = function() return true end,     -- no window chrome on a console
  getDesktopDimensions = function() return W, H end,
  setVSync = function() end,
  setFullscreen = function() return true end,
}

love.event = {
  quit = function()
    wc.log("love.event.quit(): cartridge hosts do not exit; ignoring")
  end,
}

love.system = {
  getOS = function() return "wasmcart" end,
  getPowerInfo = function() return "unknown" end,
  openURL = function(url)
    wc.log("love.system.openURL ignored (cartridge sandbox): " .. tostring(url))
    return false
  end,
}

-- love.image: enough for the window-icon idiom (games load an ImageData
-- purely to hand to setIcon, which a cartridge has no use for). Real pixel
-- manipulation is not supported; getWidth/getHeight work because callers
-- often query them.
love.image = {
  newImageData = function(path)
    local ok, img = pcall(graphics.newImage, path)
    if not ok then return nil end
    return {
      _img = img,
      getWidth = function(self) return self._img.w end,
      getHeight = function(self) return self._img.h end,
      getDimensions = function(self) return self._img.w, self._img.h end,
      type = function() return "ImageData" end,
    }
  end,
}

-- love.sound / love.data: present so libraries that reference them load;
-- the operations that need a real decoder or zlib fail loudly.
-- love.sound.newSoundData: in LOVE this decodes a file into raw samples so
-- the same data can back many Sources. This engine's mixer already caches
-- decoded audio per path, so the "decoded handle" a caller needs IS the
-- path. Returning it keeps the newSource(newSoundData(p)) idiom working
-- (audio libraries like slam rely on it) without pretending to expose
-- sample buffers we don't have.
love.sound = {
  newSoundData = function(path)
    if type(path) ~= "string" then
      error("love.sound.newSoundData: only file paths are supported " ..
            "(raw sample buffers are not available in this engine)", 2)
    end
    return path
  end,
  newDecoder = function(path) return path end,
}

love.data = {
  decode = function(container, format, src)
    -- base64 shows up in map/save loaders; implement it rather than fail
    if format ~= "base64" then
      error("love.data.decode: only base64 is supported in this engine", 2)
    end
    local b = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
    src = tostring(src):gsub("[^" .. b .. "=]", "")
    return (src:gsub("=", ""):gsub(".", function(x)
      local r, f = "", (b:find(x) - 1)
      for i = 6, 1, -1 do r = r .. (f % 2 ^ i - f % 2 ^ (i - 1) > 0 and "1" or "0") end
      return r
    end):gsub("%d%d%d?%d?%d?%d?%d?%d?", function(x)
      if #x ~= 8 then return "" end
      local c = 0
      for i = 1, 8 do c = c + (x:sub(i, i) == "1" and 2 ^ (8 - i) or 0) end
      return string.char(c)
    end))
  end,
  decompress = err("love.data.decompress",
    "compressed data is not supported; export maps/assets uncompressed"),
}

love.thread = setmetatable({}, { __index = function(_, k)
  return err("love.thread." .. k, "the engine is a single wasm instance; no threads in v1")
end })

-- ── physics: Box2D v3 (wasm SIMD) ──────────────────────────────────
--
-- Two surfaces over the same engine, because real LOVE games use both:
--   love.physics.*  - the LOVE-shaped world/body/fixture API
--   wf / windfield  - the collider+collision-class API most games actually
--                     use (via the windfield library)
--
-- Bodies are integer handles in C, so a destroyed collider that Lua still
-- references raises a clean error instead of corrupting the heap.

local physics = {}
love.physics = physics

local STATIC, KINEMATIC, DYNAMIC = 0, 1, 2
local TYPE_NUM = { static = STATIC, kinematic = KINEMATIC, dynamic = DYNAMIC }
local TYPE_STR = { [STATIC] = "static", [KINEMATIC] = "kinematic", [DYNAMIC] = "dynamic" }

function physics.setMeter(m) b2.set_meter(m) end
function physics.getMeter() return b2.get_meter() end
function physics.newWorld(gx, gy) return love.physics.World(gx, gy) end

-- ── windfield-compatible Collider ──────────────────────────────────

-- windfield exposes the underlying LOVE Body as `collider.body`, and games
-- reach through it (`self.physics.body:getPosition()`). Our `body` field is
-- an integer handle into C, so a raw integer there breaks that idiom.
-- `Collider.body` is therefore a proxy object exposing the LOVE Body
-- methods, while the handle lives in `_h`.
local Body = {}
Body.__index = Body
function Body:getPosition() return b2.body_position(self._h) end
function Body:getX() local x = b2.body_position(self._h) return x end
function Body:getY() local _, y = b2.body_position(self._h) return y end
function Body:setPosition(x, y) b2.body_set_position(self._h, x, y) end
function Body:getAngle() return b2.body_angle(self._h) end
function Body:setAngle(a) b2.body_set_angle(self._h, a) end
function Body:getLinearVelocity() return b2.body_velocity(self._h) end
function Body:setLinearVelocity(x, y) b2.body_set_velocity(self._h, x, y) end
function Body:applyForce(x, y) b2.body_apply_force(self._h, x, y) end
function Body:applyLinearImpulse(x, y) b2.body_apply_impulse(self._h, x, y) end
function Body:getMass() return b2.body_mass(self._h) end
function Body:setFixedRotation(v) b2.body_set_fixed_rotation(self._h, v) end
function Body:setGravityScale(v) b2.body_set_gravity_scale(self._h, v) end
function Body:setLinearDamping(v) b2.body_set_linear_damping(self._h, v) end
function Body:isDestroyed() return not b2.body_alive(self._h) end
function Body:type() return "Body" end

local Collider = {}
Collider.__index = Collider

function Collider:getPosition() return b2.body_position(self._h) end
function Collider:getX() local x = b2.body_position(self._h) return x end
function Collider:getY() local _, y = b2.body_position(self._h) return y end

function Collider:setPosition(x, y) b2.body_set_position(self._h, x, y) end
function Collider:setX(x) local _, y = b2.body_position(self._h) b2.body_set_position(self._h, x, y) end
function Collider:setY(y) local x = b2.body_position(self._h) b2.body_set_position(self._h, x, y) end

function Collider:getLinearVelocity() return b2.body_velocity(self._h) end
function Collider:setLinearVelocity(x, y) b2.body_set_velocity(self._h, x, y) end

function Collider:applyForce(x, y) b2.body_apply_force(self._h, x, y) end
function Collider:applyLinearImpulse(x, y) b2.body_apply_impulse(self._h, x, y) end

function Collider:setType(t) b2.body_set_type(self._h, TYPE_NUM[t] or DYNAMIC) end
function Collider:getType() return TYPE_STR[self.btype] or "dynamic" end
function Collider:setFixedRotation(v) b2.body_set_fixed_rotation(self._h, v) end
function Collider:setLinearDamping(v) b2.body_set_linear_damping(self._h, v) end
function Collider:setGravityScale(v) b2.body_set_gravity_scale(self._h, v) end
function Collider:setBullet(v) b2.body_set_bullet(self._h, v) end
function Collider:getMass() return b2.body_mass(self._h) end

function Collider:getAngle() return b2.body_angle(self._h) end
function Collider:setAngle(a) b2.body_set_angle(self._h, a) end

function Collider:setObject(o) self.object = o end
function Collider:getObject() return self.object end

function Collider:setCollisionClass(name)
  local w = self.world
  local cls = w.classes[name]
  if not cls then
    error("collision class '" .. tostring(name) .. "' has not been defined", 2)
  end
  self.class = name
  b2.body_set_user(self._h, cls.index)
  for _, sh in ipairs(self.shapes) do
    b2.shape_filter(sh, cls.category, cls.mask)
  end
end

-- enter/exit/stay: windfield's per-frame collision queries. The world
-- drains Box2D's begin-touch events each update and files them by class.
function Collider:enter(class)
  local hits = self.enters
  if not hits then return false end
  return hits[class] ~= nil
end

function Collider:getEnterCollisionData(class)
  local e = self.enters and self.enters[class]
  if not e then return nil end
  return { collider = e }
end

function Collider:exit(class)
  local hits = self.exits
  if not hits then return false end
  return hits[class] ~= nil
end

function Collider:stay(class) return self:enter(class) end

function Collider:collisionEventsClear()
  self.enters, self.exits = nil, nil
end

function Collider:destroy()
  if self.destroyed then return end
  self.destroyed = true
  self.world.colliders[self._h] = nil
  b2.body_destroy(self._h)
end

-- ── World ──────────────────────────────────────────────────────────

local World = {}
World.__index = World

function love.physics.World(gx, gy)
  local wh = b2.world_new(gx or 0, gy or 0)
  return setmetatable({
    handle = wh,
    classes = {},      -- name -> {index, category, mask, ignores}
    colliders = {},    -- bodyHandle -> Collider
    nclass = 0,
    byIndex = {},      -- class index -> name
  }, World)
end

function World:setGravity(gx, gy) b2.world_gravity(self.handle, gx, gy) end

-- windfield collision classes become Box2D category/mask bits. Box2D
-- filters a pair if EITHER side excludes the other, so `ignores` on one
-- class is enough -- which matches windfield's semantics.
function World:addCollisionClass(name, def)
  if self.classes[name] then return end
  self.nclass = self.nclass + 1
  if self.nclass > 62 then
    error("too many collision classes (Box2D filter bits exhausted)", 2)
  end
  local idx = self.nclass
  self.classes[name] = {
    name = name,                     -- needed for symmetric ignore checks
    index = idx,
    category = 1 << (idx - 1),
    mask = -1,                       -- all bits; narrowed below
    ignores = (def and def.ignores) or {},
  }
  self.byIndex[idx] = name
  self:_rebuildMasks()
end

function World:_rebuildMasks()
  -- recompute every mask: a class added later may ignore an earlier one
  for _, cls in pairs(self.classes) do
    local mask = 0
    for otherName, other in pairs(self.classes) do
      local ignored = false
      for _, ig in ipairs(cls.ignores) do
        if ig == otherName then ignored = true break end
      end
      -- honor the OTHER class's ignore list too, so the relation is
      -- symmetric like windfield's
      for _, ig in ipairs(other.ignores) do
        if ig == cls.name then ignored = true break end
      end
      if not ignored then mask = mask | other.category end
    end
    cls.mask = mask
  end
  -- reapply to live colliders whose class predates a later addition
  for _, c in pairs(self.colliders) do
    if c.class and not c.destroyed then
      local cls = self.classes[c.class]
      if cls then
        for _, sh in ipairs(c.shapes) do b2.shape_filter(sh, cls.category, cls.mask) end
      end
    end
  end
end

local function register(world, handle, shapes)
  local c = setmetatable({
    world = world, body = setmetatable({ _h = handle }, Body),
    _h = handle, shapes = shapes,
    class = nil, object = nil, destroyed = false,
  }, Collider)
  world.colliders[handle] = c
  return c
end

-- windfield takes CENTER x,y for rect colliders (w,h are full extents)
function World:newRectangleCollider(x, y, w, h, settings)
  local body = b2.body_new(self.handle, x + w / 2, y + h / 2, DYNAMIC)
  local sh = b2.shape_box(body, w / 2, h / 2, settings)
  return register(self, body, { sh })
end

function World:newBSGRectangleCollider(x, y, w, h, cut, settings)
  -- "beveled" rectangle: an octagon with the corners cut. Used for player
  -- and enemy bodies so they don't catch on tile seams -- approximating it
  -- with a plain box would make movement snag, so build the real hull.
  local cx, cy = x + w / 2, y + h / 2
  local hw, hh = w / 2, h / 2
  cut = cut or 0
  local pts = {
    -hw + cut, -hh,   hw - cut, -hh,
     hw, -hh + cut,   hw,  hh - cut,
     hw - cut,  hh,  -hw + cut,  hh,
    -hw,  hh - cut,  -hw, -hh + cut,
  }
  local body = b2.body_new(self.handle, cx, cy, DYNAMIC)
  local sh = b2.shape_polygon(body, pts, settings)
  return register(self, body, { sh })
end

function World:newCircleCollider(x, y, r, settings)
  local body = b2.body_new(self.handle, x, y, DYNAMIC)
  local sh = b2.shape_circle(body, r, settings)
  return register(self, body, { sh })
end

function World:newPolygonCollider(verts, settings)
  local cx, cy = 0, 0
  local n = #verts / 2
  for i = 1, #verts, 2 do cx = cx + verts[i]; cy = cy + verts[i + 1] end
  cx, cy = cx / n, cy / n
  local local_pts = {}
  for i = 1, #verts, 2 do
    local_pts[#local_pts + 1] = verts[i] - cx
    local_pts[#local_pts + 1] = verts[i + 1] - cy
  end
  local body = b2.body_new(self.handle, cx, cy, DYNAMIC)
  local sh = b2.shape_polygon(body, local_pts, settings)
  return register(self, body, { sh })
end

function World:newLineCollider(x1, y1, x2, y2, settings)
  local body = b2.body_new(self.handle, 0, 0, STATIC)
  local sh = b2.shape_segment(body, x1, y1, x2, y2, settings)
  return register(self, body, { sh })
end

function World:update(dt)
  -- clear last frame's events, then drain Box2D's begin-touch list and
  -- file each hit on BOTH colliders keyed by the other's class name
  for _, c in pairs(self.colliders) do c.enters, c.exits = nil, nil end
  b2.world_step(self.handle, dt or (1 / 60), 4)

  for _, pair in ipairs(b2.contacts(self.handle)) do
    local a = self.colliders[pair[1]]
    local bb = self.colliders[pair[2]]
    if a and bb then
      if bb.class then
        a.enters = a.enters or {}
        a.enters[bb.class] = bb
      end
      if a.class then
        bb.enters = bb.enters or {}
        bb.enters[a.class] = a
      end
    end
  end
end

local function filterByClass(self, handles, class_names)
  local out = {}
  for _, h in ipairs(handles) do
    local c = self.colliders[h]
    if c and not c.destroyed then
      if not class_names then
        out[#out + 1] = c
      else
        for _, want in ipairs(class_names) do
          if c.class == want then out[#out + 1] = c break end
        end
      end
    end
  end
  return out
end

function World:queryCircleArea(x, y, radius, class_names)
  return filterByClass(self, b2.query_circle(self.handle, x, y, radius), class_names)
end

function World:queryRectangleArea(x, y, w, h, class_names)
  return filterByClass(self, b2.query_box(self.handle, x, y, w, h), class_names)
end

function World:queryPolygonArea(verts, class_names)
  -- Box2D's broad phase is AABB-based; use the polygon's bounding box.
  -- Callers use this to find "things near here", so a slightly generous
  -- result is acceptable -- but say so rather than implying exactness.
  local minx, miny = math.huge, math.huge
  local maxx, maxy = -math.huge, -math.huge
  for i = 1, #verts, 2 do
    minx = math.min(minx, verts[i]); maxx = math.max(maxx, verts[i])
    miny = math.min(miny, verts[i + 1]); maxy = math.max(maxy, verts[i + 1])
  end
  return filterByClass(self, b2.query_box(self.handle, minx, miny, maxx - minx, maxy - miny), class_names)
end

function World:destroy()
  for _, c in pairs(self.colliders) do c:destroy() end
  self.colliders = {}
end

-- debug draw: outline every live collider. Games gate this behind a key,
-- so it must exist even though it is off in normal play.
function World:draw(alpha)
  local a = (alpha or 255) / 255
  love.graphics.setColor(0.2, 1, 0.4, a)
  for _, c in pairs(self.colliders) do
    if not c.destroyed then
      local x, y = b2.body_position(c._h)
      love.graphics.circle("line", x, y, 12)
    end
  end
  love.graphics.setColor(1, 1, 1, 1)
end

function World:getColliderCount()
  local n = 0
  for _ in pairs(self.colliders) do n = n + 1 end
  return n
end

-- `wf` is what windfield-based games require; make it available directly
-- so a port needs no shim file of its own.
wf = {
  newWorld = function(gx, gy, sleep)
    return love.physics.World(gx, gy)
  end,
}
physics.stats = function() return b2.stats() end

-- ── Lua 5.1 compatibility ──────────────────────────────────────────
--
-- LOVE ships LuaJIT, so the entire LOVE library ecosystem is written
-- against the Lua 5.1 dialect. This engine runs Lua 5.4, where several
-- 5.1 globals were moved or removed. Providing them is what lets real
-- LOVE games and libraries load unmodified, and each is a faithful
-- implementation rather than a stub:
--
--   unpack      -> moved to table.unpack in 5.2
--   table.getn  -> removed in 5.1 in favour of the # operator
--   loadstring  -> merged into load() in 5.2
--   setfenv/getfenv -> replaced by _ENV upvalues in 5.2; emulated via
--                      debug.setupvalue, which is the standard shim
--   math.mod / table.foreach -> 5.0 leftovers some old libs still call

unpack = unpack or table.unpack
loadstring = loadstring or load

table.getn = table.getn or function(t) return #t end
table.setn = table.setn or function() end   -- was already a no-op in 5.1
table.foreach = table.foreach or function(t, f)
  for k, v in pairs(t) do
    local r = f(k, v)
    if r ~= nil then return r end
  end
end
table.foreachi = table.foreachi or function(t, f)
  for i, v in ipairs(t) do
    local r = f(i, v)
    if r ~= nil then return r end
  end
end

math.mod = math.mod or function(a, b) return a % b end
math.pow = math.pow or function(a, b) return a ^ b end
math.ldexp = math.ldexp or function(m, e) return m * 2 ^ e end
-- math.atan2 was removed in 5.3 (math.atan takes two args now). Game code
-- uses it constantly for aiming, so it must be present.
math.atan2 = math.atan2 or function(y, x) return math.atan(y, x) end
math.frexp = math.frexp or function(x)
  if x == 0 then return 0, 0 end
  local e = math.floor(math.log(math.abs(x), 2)) + 1
  return x / 2 ^ e, e
end
math.log10 = math.log10 or function(x) return math.log(x, 10) end

-- setfenv/getfenv over 5.4's _ENV. Function environments are upvalue 1
-- named "_ENV" when a function references any global.
function setfenv(fn, env)
  if type(fn) == "number" then
    error("setfenv by stack level is not supported; pass the function", 2)
  end
  local i = 1
  while true do
    local name = debug.getupvalue(fn, i)
    if not name then break end
    if name == "_ENV" then
      debug.upvaluejoin(fn, i, function() return env end, 1)
      return fn
    end
    i = i + 1
  end
  return fn
end

function getfenv(fn)
  if type(fn) == "number" or fn == nil then return _G end
  local i = 1
  while true do
    local name, val = debug.getupvalue(fn, i)
    if not name then break end
    if name == "_ENV" then return val end
    i = i + 1
  end
  return _G
end

-- ── os: deterministic stand-ins ────────────────────────────────────
--
-- The real `os` table is never opened (no wall clock, no processes, and it
-- would drag WASI imports). But libraries routinely call os.time() to seed
-- an RNG or os.clock() to profile, and dying on that is worse than
-- answering. These are frame-derived, so they stay reproducible: a cart
-- that seeds from os.time() still replays identically.
os = {
  time = function() return 1700000000 + frame_n end,
  clock = function() return frame_n * FIXED_DT end,
  date = function(fmt)
    -- fixed date; games use this for save-slot labels
    if fmt and fmt:sub(1, 1) == "*" then
      return { year = 2026, month = 1, day = 1, hour = 0, min = 0, sec = 0,
               wday = 5, yday = 1, isdst = false }
    end
    return "2026-01-01 00:00:00"
  end,
  difftime = function(a, b) return (a or 0) - (b or 0) end,
  getenv = function() return nil end,
  remove = function() return nil, "no filesystem" end,
  rename = function() return nil, "no filesystem" end,
  exit = function() wc.log("os.exit ignored: cartridges do not exit") end,
}

-- ── require: cart-asset module loader ──────────────────────────────
local loaded = {}
function require(name)
  if loaded[name] then return loaded[name] end
  local path = name:gsub("%.", "/")
  local candidates = { path .. ".lua", "lib/" .. path .. ".lua", path .. "/init.lua" }
  for _, p in ipairs(candidates) do
    local src = wc.asset_read(p)
    if src then
      local chunk, e = load(src, "@" .. p)
      if not chunk then error("error loading module '" .. name .. "': " .. e, 2) end
      -- Standard Lua passes the module name to the chunk as `...`. Real
      -- libraries rely on it to locate their own submodules
      -- (`local path = ...` then `require(path .. '.sub')`), so a loader
      -- that drops it breaks them in a way that looks like a nil-concat
      -- bug inside the library.
      local ok, res = pcall(chunk, name, p)
      if not ok then error("error running module '" .. name .. "': " .. tostring(res), 2) end
      if res == nil then res = true end
      loaded[name] = res
      return res
    end
  end
  error("module '" .. name .. "' not found in cart assets (tried: " ..
        table.concat(candidates, ", ") .. ")", 2)
end

-- ── debug helpers for the harness ──────────────────────────────────
love.debugValue = function(slot, v) wc.debug_set(slot, math.floor(v)) end
love.mark = function(id) wc.mark(id) end
love.log = function(...)
  local parts = {}
  for _, v in ipairs({ ... }) do parts[#parts + 1] = tostring(v) end
  wc.log(table.concat(parts, " "))
end
print = love.log

-- ── the frame driver (called from C) ───────────────────────────────

function __wasmcart_load()
  if love.load then love.load() end
end

function __wasmcart_frame(b1, lx1, ly1, rx1, ry1,
                          b2, lx2, ly2, rx2, ry2,
                          b3, lx3, ly3, rx3, ry3,
                          b4, lx4, ly4, rx4, ry4)
  local p
  p = pads[1]; p.prev = p.buttons; p.buttons = b1; p.lx = lx1; p.ly = ly1; p.rx = rx1; p.ry = ry1
  p = pads[2]; p.prev = p.buttons; p.buttons = b2; p.lx = lx2; p.ly = ly2; p.rx = rx2; p.ry = ry2
  p = pads[3]; p.prev = p.buttons; p.buttons = b3; p.lx = lx3; p.ly = ly3; p.rx = rx3; p.ry = ry3
  p = pads[4]; p.prev = p.buttons; p.buttons = b4; p.lx = lx4; p.ly = ly4; p.rx = rx4; p.ry = ry4

  -- edge callbacks (gamepad + the keyboard mapping)
  if love.gamepadpressed or love.keypressed then
    for n = 1, 4 do
      local pp = pads[n]
      for name, bit in pairs(BTN) do
        if (pp.buttons & bit) ~= 0 and (pp.prev & bit) == 0 then
          if love.gamepadpressed then love.gamepadpressed(joy_objs[n], name) end
          if n == 1 and love.keypressed then love.keypressed(name) end
        end
      end
    end
  end
  if love.gamepadreleased or love.keyreleased then
    for n = 1, 4 do
      local pp = pads[n]
      for name, bit in pairs(BTN) do
        if (pp.buttons & bit) == 0 and (pp.prev & bit) ~= 0 then
          if love.gamepadreleased then love.gamepadreleased(joy_objs[n], name) end
          if n == 1 and love.keyreleased then love.keyreleased(name) end
        end
      end
    end
  end

  frame_n = frame_n + 1
  update_vcursor()

  -- Pointer edges -> love.mousepressed / love.mousereleased. Menus are
  -- driven by these callbacks rather than by polling, so a poll-only
  -- implementation leaves games stuck on their title screen.
  do
    local px, py, buttons, active = wc.pointer(0)
    if not active then
      px, py = vcursor_x, vcursor_y
      -- gamepad fallback: R (or A) acts as the primary click
      local p1 = pads[1]
      buttons = (p1 and ((p1.buttons & BTN.r) ~= 0 or (p1.buttons & BTN.a) ~= 0)) and 1 or 0
    end
    for bit = 0, 2 do
      local mask = 1 << bit
      local now = (buttons & mask) ~= 0
      local was = (prev_mouse & mask) ~= 0
      if now and not was and love.mousepressed then
        love.mousepressed(px, py, bit + 1, false)
      elseif was and not now and love.mousereleased then
        love.mousereleased(px, py, bit + 1, false)
      end
    end
    prev_mouse = buttons
  end

  if love.update then love.update(FIXED_DT) end

  -- reset per-frame graphics state, then clear to the background color
  tx, ty, tsx, tsy, trot = 0, 0, 1, 1, 0
  for i = #tstack, 1, -1 do tstack[i] = nil end
  graphics.clear()
  wc.set_color(cr, cg, cb, ca)

  if love.draw then love.draw() end
end
