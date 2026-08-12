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

-- The REAL Lua type(), for engine-internal checks. The prelude installs a
-- type() at the very end that reports LOVE objects as "userdata" (see
-- there for why); engine code must not see that, since it works with those
-- objects AS tables.
local rawtype = type
-- Exposed so the embedded ffi shim (appended after this file) can see the
-- real type() too. Not part of the cart-facing API.
rawset(_G, "rawtype", rawtype)

local W, H = __WC_WIDTH, __WC_HEIGHT
__WC_WIDTH, __WC_HEIGHT = nil, nil

-- conf.lua may already have populated love (love.conf lives there); keep it.
love = love or {}
local love = love

-- ── internal state ─────────────────────────────────────────────────
local FIXED_DT = 1 / 60
local frame_n = 0

-- transform stack (Lua-side; C is a dumb rasterizer in world coords)
local tx, ty, tsx, tsy, trot = 0, 0, 1, 1, 0
-- Shear, kept as its own pair rather than folded into a full 3x3 matrix.
-- apply() runs for EVERY transformed vertex of every draw, so the cheap
-- representation earns its keep: the common case (no shear) costs one
-- branch, where a general matrix would cost four multiplies always.
local tkx, tky = 0, 0
local tstack = {}

local function apply(x, y)
  if tkx ~= 0 or tky ~= 0 then
    x, y = x + y * tkx, y + x * tky
  end
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
  if rawtype(r) == "table" then r, g, b, a = r[1], r[2], r[3], r[4] end
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
  if rawtype(r) == "table" then r, g, b = r[1], r[2], r[3] end
  bg[1], bg[2], bg[3] = r or 0, g or 0, b or 0
end

function graphics.getBackgroundColor() return bg[1], bg[2], bg[3] end

-- clear() | clear(r,g,b,a) | clear({r,g,b,a}, {r,g,b,a}, ...) |
-- clear(true, true, ...)
--
-- With multiple render targets bound, LOVE takes ONE argument per target:
-- a table to clear that attachment to a colour, or a boolean to clear it to
-- the background. A renderer clearing a g-buffer calls it that way, and
-- treating the first boolean as a red channel is an arithmetic error on a
-- line the cart author never wrote.
function graphics.clear(r, g, b, a)
  if rawtype(r) == "boolean" then
    -- Clear every bound target to the background. This engine clears the
    -- whole framebuffer at once rather than per attachment, which is the
    -- same result whenever the targets are cleared together -- and they are,
    -- since that is what a single clear() call means.
    wc.clear(math.floor(bg[1] * 255 + 0.5), math.floor(bg[2] * 255 + 0.5),
             math.floor(bg[3] * 255 + 0.5))
    return
  end
  if r then
    if rawtype(r) == "table" then r, g, b, a = r[1], r[2], r[3], r[4] end
    -- Alpha is forwarded, NOT dropped. Clearing a canvas to (0,0,0,0) is
    -- the standard way to get an empty texture to bake a sprite into, and
    -- swallowing the 4th argument here silently produced an opaque black
    -- background instead -- which then painted a black box over the board
    -- wherever the baked sprite was drawn.
    wc.clear(math.floor(r * 255 + 0.5), math.floor((g or 0) * 255 + 0.5),
             math.floor((b or 0) * 255 + 0.5),
             math.floor(((a == nil) and 1 or a) * 255 + 0.5))
  else
    wc.clear(math.floor(bg[1] * 255 + 0.5), math.floor(bg[2] * 255 + 0.5),
             math.floor(bg[3] * 255 + 0.5), math.floor((bg[4] or 1) * 255 + 0.5))
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

-- An ellipse is a circle with independent radii, so it cannot go through
-- wc.circle. Emitted as a polygon, which is what LOVE does internally too
-- (a circle IS an ellipse with rx == ry there). Segment count follows the
-- larger radius so a big ellipse does not turn into a visible polygon.
function graphics.ellipse(mode, x, y, rx, ry, segments)
  ry = ry or rx
  local n = segments or math.max(8, math.floor(math.max(rx, ry) / 2) + 8)
  -- Capped at the shared polygon limit (WCL_MAX_POLY_PTS, 256). Not a
  -- cosmetic clamp: a polygon the GL path refuses drops the WHOLE FRAME --
  -- 3D included -- to the software rasterizer for the rest of the run, with
  -- no error and nothing visibly different except the frame time tripling.
  -- The cap used to be 64 in the C layer, which a 120px ellipse exceeded.
  if n > 256 then n = 256 end
  local pts = {}
  for i = 0, n - 1 do
    local a = i / n * math.pi * 2
    pts[#pts + 1] = x + math.cos(a) * rx
    pts[#pts + 1] = y + math.sin(a) * ry
  end
  graphics.polygon(mode, pts)
end

function graphics.line(...)
  local pts = ...
  if rawtype(pts) ~= "table" then pts = { ... } end
  for i = 1, #pts - 3, 2 do
    local x1, y1 = apply(pts[i], pts[i + 1])
    local x2, y2 = apply(pts[i + 2], pts[i + 3])
    wc.line(x1, y1, x2, y2)
  end
end

function graphics.points(...)
  local pts = ...
  if rawtype(pts) ~= "table" then pts = { ... } end
  for i = 1, #pts - 1, 2 do
    local x, y = apply(pts[i], pts[i + 1])
    wc.point(x, y)
  end
end

-- Scratch buffer reused across every polygon() call.
--
-- This used to allocate a fresh `out` table per call and grow it one element
-- at a time with out[#out+1], which reallocates the array part as it doubles.
-- A cart drawing 48 filled polygons a frame (16 objects x 3 rings) threw away
-- ~50 KB PER FRAME on this line alone -- 3 MB/s of garbage for a shape whose
-- vertex count never changes. The transformed coordinates are handed straight
-- to wc.polygon and are dead the moment it returns, so a single buffer is
-- safe: nothing can hold a reference across calls, and the C side copies out
-- of it before returning. Not reentrant, which polygon() never is.
local poly_scratch = {}
local arc_scratch = {}

function graphics.polygon(mode, ...)
  local pts = ...
  if rawtype(pts) ~= "table" then pts = { ... } end
  local out = poly_scratch
  local n = 0
  for i = 1, #pts - 1, 2 do
    local x, y = apply(pts[i], pts[i + 1])
    out[n + 1] = x
    out[n + 2] = y
    n = n + 2
  end
  -- The C side reads rawlen, so a buffer left longer by a previous call would
  -- feed stale trailing vertices into this polygon. Truncate to this call's
  -- point count.
  for i = #out, n + 1, -1 do out[i] = nil end
  wc.polygon(mode == "fill" and 1 or 0, out)
end

-- Forward declaration: graphics.draw() dispatches on SpriteBatch, Mesh and
-- Mesh3D, all defined further down. Without this the reference inside draw
-- would bind to a global (nil) instead of the local table -- and since the
-- dispatch is `getmetatable(img) == Mesh3D`, a nil binding does not error,
-- it just never matches, and every 3D draw silently falls through to the
-- image path.

local SpriteBatch
local ParticleSystem
local Text
local Mesh
local Mesh3D
local new_mesh_3d
local new_mesh_generic
-- Canvas3D is a GPU render target, distinct from the CPU-backed Image-shaped
-- canvas. setCanvas dispatches on it, so it must be a local visible from
-- there rather than a global that a cart could shadow.
local Canvas3D

-- Image / Quad / Canvas objects
local Image = {}
Image.__index = Image
function Image:getWidth()  return self.w end
function Image:getHeight() return self.h end
function Image:getDimensions() return self.w, self.h end
function Image:type() return self.canvas and "Canvas" or "Image" end
-- Canvas introspection. A renderer clones a canvas by reading its settings
-- back and passing them to newCanvas, so these have to return values that
-- round-trip -- and the ordinary CPU-backed canvas is always plain 8-bit
-- RGBA with no mipmaps.
function Image:getFormat() return "normal" end
function Image:isReadable() return true end
function Image:getMSAA() return 0 end
function Image:getTextureType() return "2d" end
function Image:getMipmapMode() return "none" end

-- Canvas:renderTo(fn, ...) -- draw into this canvas, then restore.
--
-- The value over setCanvas/setCanvas() by hand is that the previous target
-- is RESTORED rather than reset to the screen. Nested renderTo is common
-- (a canvas built from another canvas), and a plain setCanvas() at the end
-- of the inner one would silently drop the outer target on the floor.
--
-- pcall so a throwing callback cannot leave the canvas bound for the rest
-- of the frame -- which paints every later draw into the wrong target and
-- looks like the screen froze.
function Image:renderTo(fn, ...)
  if type(fn) ~= "function" then return end
  local prev = graphics.getCanvas and graphics.getCanvas() or nil
  graphics.setCanvas(self)
  local ok, err = pcall(fn, ...)
  if prev then graphics.setCanvas(prev) else graphics.setCanvas() end
  if not ok then error(err, 2) end
end
function Image:getMipmapCount() return 1 end
function Image:getDepth() return 1 end
function Image:getLayerCount() return 1 end
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
  if rawtype(src) == "table" then
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

-- ── canvases ────────────────────────────────────────────────────────
--
-- There are TWO kinds, and which one you get depends on the settings:
--
--   newCanvas(w, h)                     -> the ordinary RGBA8 canvas
--   newCanvas(w, h, {format = "rgba16f", type = "cube", ...})
--                                       -> a GPU render target
--
-- The plain canvas is backed by an RGBA8 buffer in the cart's own memory,
-- which is what lets the software rasterizer draw into it and keeps the CPU
-- fallback exact. That design cannot represent a float target (no 8-bit
-- form), a depth target (no colour), or a cubemap (six faces, not one
-- buffer), so those live on the GPU only -- no readback, no software path.
--
-- Asking for a GPU-only canvas on a host with no GL is an error rather than
-- a silent downgrade to RGBA8: a renderer that thinks it has 16 bits of
-- headroom and gets 8 produces banding it will never explain.
local GPU_ONLY_FORMATS = {
  r16f = true, rg16f = true, rgba16f = true, r32f = true, rgba32f = true,
  depth16 = true, depth24 = true, depth32f = true, depth24stencil8 = true,
  r8 = true, rg8 = true,
}

Canvas3D = {}
Canvas3D.__index = Canvas3D
function Canvas3D:type() return "Canvas" end
function Canvas3D:typeOf(t) return t == "Canvas" or t == "Texture" or t == "Object" end
function Canvas3D:getWidth() return self.w end
function Canvas3D:getHeight() return self.h end
function Canvas3D:getDimensions() return self.w, self.h end
function Canvas3D:getFormat() return self.format end
function Canvas3D:getTextureType() return self.textype end
function Canvas3D:getLayerCount() return self.layers end
function Canvas3D:getMipmapCount() return self.mipmaps and 2 or 1 end
-- LOVE's newCanvas takes mipmaps as a MODE string, and getMipmapMode returns
-- it -- a renderer round-trips this to clone a canvas's settings, so the
-- value has to be one newCanvas accepts back.
function Canvas3D:getMipmapMode() return self.mipmaps and "manual" or "none" end
function Canvas3D:getDepth() return 1 end
function Canvas3D:getDepthSampleMode() return nil end
function Canvas3D:setDepthSampleMode() end
function Canvas3D:isReadable() return true end
function Canvas3D:getMSAA() return 0 end
function Canvas3D:generateMipmaps() wc.target_mipmaps(self.id) end
function Canvas3D:release()
  wc.target_free(self.id)
  self.id = -1
  return true
end
-- LOVE lets a Canvas be drawn like an Image. A GPU-only target has no CPU
-- pixels for the 2D path to sample, so say so rather than draw nothing.
function Canvas3D:getFilter() return self._filter or "linear", self._filter or "linear" end
function Canvas3D:setFilter(min)
  self._filter = (min == "nearest") and "nearest" or "linear"
  wc.image3d_filter(self.id, self._filter == "linear")
end
-- setWrap("repeat") applies to every axis, which is the common call; the
-- per-axis form is honoured when given.
function Canvas3D:setWrap(s, t, r)
  self._wrap = s or "clamp"
  wc.image3d_wrap(self.id, s == "repeat", (t or s) == "repeat",
                  (r or s) == "repeat")
end
function Canvas3D:getWrap()
  local w = self._wrap or "clamp"
  return w, w, w
end

function graphics.newCanvas(w, h, settings)
  w = w or W; h = h or H
  if rawtype(settings) == "table" then
    local fmt = settings.format or "normal"
    local textype = settings.type or "2d"
    if GPU_ONLY_FORMATS[fmt] or textype ~= "2d" then
      local id, why = wc.target_new(w, h, fmt, textype,
                                    settings.layers or 1,
                                    settings.mipmaps and settings.mipmaps ~= "none",
                                    settings.msaa or 0)
      if not id then
        if why == "nogl" then
          error("love.graphics.newCanvas: a '" .. tostring(fmt) .. "' " ..
                tostring(textype) .. " canvas is a GPU render target, and this " ..
                "run is on the software rasterizer. There is no CPU " ..
                "representation of it to fall back to.", 2)
        elseif why == "format" then
          error("love.graphics.newCanvas: unknown pixel format '" ..
                tostring(fmt) .. "'. Supported: normal/rgba8, r8, rg8, r16f, " ..
                "rg16f, rgba16f, r32f, rgba32f, depth16, depth24, depth32f, " ..
                "depth24stencil8.", 2)
        elseif why == "type" then
          error("love.graphics.newCanvas: unknown texture type '" ..
                tostring(textype) .. "'. Supported: 2d, cube, array, volume.", 2)
        end
        error("love.graphics.newCanvas: this driver cannot render to format '" ..
              tostring(fmt) .. "' at " .. w .. "x" .. h ..
              " (see the cart log)", 2)
      end
      local layers = (textype == "cube") and 6 or (settings.layers or 1)
      return setmetatable({ id = id, w = w, h = h, format = fmt,
                            textype = textype, layers = layers,
                            mipmaps = settings.mipmaps and settings.mipmaps ~= "none",
                            gpu = true }, Canvas3D)
    end
  end
  local id = wc.canvas_new(w, h)
  if not id then error("could not create canvas", 2) end
  return setmetatable({ id = id, w = w, h = h, canvas = true }, Image)
end

-- ── cube / array / volume images ────────────────────────────────────
--
-- The read-only twin of a GPU canvas: the same texture shapes, filled from
-- the cart's own images instead of rendered into. A skybox is a cube image,
-- a texture atlas that must not bleed between entries is an array image, and
-- a 3D lookup table is a volume image.
--
-- Sources may be paths, Images, or ImageData -- LOVE accepts all three and
-- ported code uses each. They are all resolved to a decoded 2D image first,
-- which means cube faces get the same PNG decoder and the same missing-asset
-- error as every other texture in the cart.
local function resolve_layer_source(src, what, index)
  if rawtype(src) == "string" then
    local img = graphics.newImage(src)
    return img
  end
  if rawtype(src) == "table" then
    -- an ImageData wrapper from love.image.newImageData
    if src._img then return src._img end
    if src.id then return src end
  end
  error(what .. ": source " .. index .. " must be a filename, an Image or " ..
        "an ImageData, got " .. rawtype(src), 3)
end

local function new_layered_image(what, textype, sources, settings)
  if rawtype(sources) ~= "table" or #sources < 1 then
    error(what .. ": expected a table of " ..
          (textype == "cube" and "6 faces" or "layers"), 3)
  end
  -- LOVE also accepts a single filename for a cube image laid out as a
  -- cross or a strip. That needs an image-slicing step this engine has no
  -- reason to guess at, so it is refused with the shape that does work.
  if textype == "cube" and #sources ~= 6 then
    error(what .. ": expected exactly 6 faces (+X, -X, +Y, -Y, +Z, -Z), got " ..
          #sources .. ". A single-image cube layout (cross/strip) is not " ..
          "supported; pass the six faces.", 3)
  end

  local imgs = {}
  for i = 1, #sources do
    imgs[i] = resolve_layer_source(sources[i], what, i)
  end
  local w, h = imgs[1].w, imgs[1].h

  settings = settings or {}
  local id, why = wc.image3d_new(w, h, textype, #imgs,
                                 settings.mipmaps and settings.mipmaps ~= "none")
  if not id then
    if why == "nogl" then
      error(what .. ": this run is on the software rasterizer, and a " ..
            textype .. " texture is GPU-only.", 3)
    end
    error(what .. ": the texture could not be created (" .. tostring(why) ..
          ")", 3)
  end
  for i = 1, #imgs do
    if not wc.image3d_upload_from(id, i - 1, imgs[i].id) then
      error(what .. ": face/layer " .. i .. " could not be uploaded. Every " ..
            "face must be the same size (" .. w .. "x" .. h .. ").", 3)
    end
  end
  wc.image3d_finish(id)

  return setmetatable({ id = id, w = w, h = h, format = "rgba8",
                        textype = textype, layers = #imgs,
                        mipmaps = settings.mipmaps and settings.mipmaps ~= "none",
                        gpu = true, image = true }, Canvas3D)
end

function graphics.newCubeImage(faces, settings)
  return new_layered_image("love.graphics.newCubeImage", "cube", faces, settings)
end

function graphics.newArrayImage(layers, settings)
  return new_layered_image("love.graphics.newArrayImage", "array", layers, settings)
end

function graphics.newVolumeImage(slices, settings)
  return new_layered_image("love.graphics.newVolumeImage", "volume", slices, settings)
end

-- love.graphics.getCanvasFormats() - which formats this driver can render
-- to. PROBED, not hardcoded: on WebGL2 the float formats are renderable only
-- with EXT_color_buffer_float, so the honest answer is a runtime one. A
-- renderer picks its g-buffer format from this table (3DreamEngine does
-- exactly that), and a wrong answer here means an incomplete framebuffer
-- much later, with nothing pointing back to the cause.
function graphics.getCanvasFormats()
  local out = {}
  for _, f in ipairs({ "normal", "rgba8", "r8", "rg8", "r16f", "rg16f",
                       "rgba16f", "r32f", "rgba32f", "depth16", "depth24",
                       "depth32f", "depth24stencil8" }) do
    out[f] = wc.target_supported(f) and true or false
  end
  -- The plain RGBA8 canvas exists even with no GL at all.
  out.normal = true
  out.rgba8 = true
  return out
end

function graphics.getSystemLimits()
  return {
    -- LOVE calls the MRT limit "multicanvas".
    multicanvas = wc.gl_limit(0),
    texturesize = wc.gl_limit(1),
    cubetexturesize = wc.gl_limit(2),
    volumetexturesize = wc.gl_limit(3),
    texturelayers = wc.gl_limit(4),
    multisample = wc.gl_limit(5),
    anisotropy = 1,
    pointsize = 1,
  }
end

-- The canvas is TRACKED, not just forwarded, because getCanvas is a real
-- part of the API rather than a convenience: a renderer has to know whether
-- it is drawing to the screen or to an off-screen target, since an FBO's
-- origin is bottom-left against the screen's top-left and the projection
-- has to flip to match. g3d asks on every model draw
-- (`shader:send("isCanvasEnabled", love.graphics.getCanvas() ~= nil)`), and
-- without it the whole scene renders vertically mirrored.
local cur_canvas = nil
-- Forward declaration: graphics.push saves the bound shader, and push is
-- defined above the Shader section. Without this it would bind to a global.
local cur_shader = nil

-- setCanvas() | setCanvas(canvas) | setCanvas({t1, t2, ..., depthstencil = d})
-- and LOVE's per-target form  setCanvas({{canvas, face = n}, ...})
--
-- The multi-target call is what a deferred renderer's geometry pass is: one
-- draw writing colour, normals and depth to separate attachments at once,
-- which is the difference between one geometry pass and three.
function graphics.setCanvas(a, ...)
  -- setCanvas() with no arguments: back to the screen.
  if a == nil then
    wc.set_canvas(nil)
    wc.target_bind(nil, nil, nil)
    cur_canvas = nil
    return
  end

  -- A single ordinary canvas keeps the original CPU-backed path.
  if getmetatable(a) == Image then
    wc.target_bind(nil, nil, nil)      -- drop any GPU target first
    wc.set_canvas(a.id)
    cur_canvas = a
    return
  end

  -- A single GPU target.
  if getmetatable(a) == Canvas3D then
    wc.set_canvas(nil)
    if not wc.target_bind({ a.id }, { 0 }, nil) then
      error("love.graphics.setCanvas: the framebuffer could not be completed " ..
            "(see the cart log)", 2)
    end
    cur_canvas = a
    return
  end

  if rawtype(a) ~= "table" then
    error("love.graphics.setCanvas: expected a Canvas, a table of Canvases, " ..
          "or no arguments, got " .. rawtype(a), 2)
  end

  -- The table form. Entries are either a Canvas or {Canvas, face = n,
  -- layer = n, mipmap = n}; `depthstencil` names the depth target.
  local handles, layers = {}, {}
  local list = a
  for i = 1, #list do
    local e = list[i]
    local canvas, layer = e, 0
    if getmetatable(e) ~= Canvas3D and getmetatable(e) ~= Image and rawtype(e) == "table" then
      canvas = e[1]
      -- LOVE spells the cube face "face" and the array slice "layer"; both
      -- are 1-based in its API and 0-based in GL.
      layer = (e.face and e.face - 1) or (e.layer and e.layer - 1) or 0
    end
    if getmetatable(canvas) == Image then
      error("love.graphics.setCanvas: an ordinary canvas cannot be one of " ..
            "several render targets -- it is a CPU buffer, not a GPU " ..
            "attachment. Create it with a format (e.g. {format=\"rgba16f\"}) " ..
            "to get a target that can.", 2)
    end
    if getmetatable(canvas) ~= Canvas3D then
      error("love.graphics.setCanvas: entry " .. i .. " is not a Canvas", 2)
    end
    handles[#handles + 1] = canvas.id
    layers[#layers + 1] = layer
  end

  local depth, dlayer = nil, 0
  local ds = list.depthstencil
  if ds ~= nil and ds ~= false then
    -- `depthstencil = true` asks for a depth buffer without naming one.
    -- This engine has no implicit depth attachment to hand out, so say so
    -- rather than silently render without depth testing.
    if ds == true then
      error("love.graphics.setCanvas: depthstencil=true asks this engine for " ..
            "an implicit depth buffer, which it does not have. Create one " ..
            "explicitly -- newCanvas(w, h, {format=\"depth24\"}) -- and pass " ..
            "it as depthstencil.", 2)
    end
    local dc = ds
    if getmetatable(dc) ~= Canvas3D and rawtype(dc) == "table" then
      dc = ds[1]
      dlayer = (ds.face and ds.face - 1) or (ds.layer and ds.layer - 1) or 0
    end
    if getmetatable(dc) ~= Canvas3D then
      error("love.graphics.setCanvas: depthstencil is not a Canvas", 2)
    end
    depth = dc.id
  end

  if #handles == 0 and not depth then
    wc.set_canvas(nil)
    wc.target_bind(nil, nil, nil)
    cur_canvas = nil
    return
  end

  wc.set_canvas(nil)
  if not wc.target_bind(handles, layers, depth, dlayer) then
    error("love.graphics.setCanvas: the framebuffer could not be completed. " ..
          "Every target must be the same size, the driver must support " ..
          "rendering to each format, and there must be no more targets than " ..
          "getSystemLimits().multicanvas (" .. tostring(wc.gl_limit(0)) ..
          "). See the cart log.", 2)
  end
  cur_canvas = list[1]
  return
end

function graphics.getCanvas() return cur_canvas end

-- ── capability queries ─────────────────────────────────────────────
--
-- Libraries probe these ON STARTUP to pick a code path, so a missing one
-- is a nil-index crash before the game draws a single frame. They are
-- cheap to answer honestly and expensive to be wrong about: claiming a
-- feature we lack sends a library down a path that then fails obscurely.

function graphics.getSupported()
  return {
    -- real, and exercised by test/shaders and test/gpu3d
    glsl3          = true,
    instancing     = true,
    multicanvas    = true,
    fullnpot       = true,
    pixelshaderhighp = true,
    -- NOT implemented. Said plainly so a library picks its fallback
    -- rather than discovering the gap mid-render.
    clampzero      = false,
    lighten        = false,
    texelbuffer    = false,
    indexbuffer32  = false,
    copybuffer     = false,
    copytexturetobuffer = false,
    glsl4          = false,
  }
end

function graphics.getTextureTypes()
  return { ["2d"] = true, cube = true, array = false, volume = false }
end

function graphics.getImageFormats()
  -- Mirrors getCanvasFormats for the plain readable formats a cart can
  -- actually create an image in.
  return { normal = true, rgba8 = true, r8 = true, rg8 = true,
           rgba16f = true, rgba32f = true }
end

-- Gamma-correct rendering is OFF: the framebuffer is plain 8-bit sRGB and
-- nothing linearises on write. love.math.gammaToLinear exists for carts
-- that want to do it themselves.
function graphics.isGammaCorrect() return false end

-- Wireframe is not implemented. Report it rather than pretending, and
-- make the setter a no-op instead of an error so a debug key that toggles
-- it does not take the game down.
local wireframe = false
function graphics.setWireframe(v) wireframe = v and true or false end
function graphics.isWireframe() return false end

-- discard() hints that the current target's contents can be thrown away
-- instead of loaded. That is a bandwidth optimisation for tiled GPUs; here
-- there is nothing to discard, and a no-op is CORRECT rather than a stub --
-- the visible result is identical either way.
function graphics.discard() end

-- flushBatch forces pending draws out. Nothing is buffered across calls
-- here, so this is likewise genuinely a no-op.
function graphics.flushBatch() end

-- draw(image, [quad], x, y, r, sx, sy, ox, oy)
-- Drawing a GPU render target as an IMAGE.
--
-- A deferred renderer composites by drawing its canvases: the AO pass draws
-- the depth canvas, the blur passes draw each other, and the final pass
-- draws the colour canvas to the screen. Every one of those is
-- love.graphics.draw(canvas, ...) on a target this engine keeps on the GPU.
--
-- Without this, all of that silently does nothing -- every pass runs, no
-- error is raised, and the screen stays black. That is exactly the failure
-- this hit: shaders compiled, meshes drew, and the composite never landed.
--
-- Implemented as a textured quad through the 3D pipeline, because that is
-- the path that can sample a GPU texture at all. The quad is built once and
-- its vertices rewritten per draw.
local blit_quad, blit_shader

local function draw_gpu_target(target, x, y, r, sx, sy, ox, oy)
  x = x or 0; y = y or 0; r = r or 0
  sx = sx or 1; sy = sy or (sx or 1); ox = ox or 0; oy = oy or 0

  if not blit_shader then
    blit_shader = graphics.newShader([[
      extern Image wc_blit_tex;
      vec4 effect(vec4 c, Image t, vec2 uv, vec2 sc) {
        return texture(wc_blit_tex, uv) * c;
      }
    ]], [[
      vec4 position(mat4 tp, vec4 vp) {
        vec3 n = VertexNormal;    // selects the 3D prologue
        return vec4(vp.xy, 0.0, 1.0);
      }
    ]])
    blit_quad = new_mesh_generic({
      { "VertexPosition", "float", 3 },
      { "VertexTexCoord", "float", 2 },
      { "VertexNormal",   "float", 3 },
      { "VertexColor",    "byte",  4 },
    }, {
      {0,0,0, 0,0, 0,0,1, 255,255,255,255},
      {0,0,0, 1,0, 0,0,1, 255,255,255,255},
      {0,0,0, 1,1, 0,0,1, 255,255,255,255},
      {0,0,0, 0,0, 0,0,1, 255,255,255,255},
      {0,0,0, 1,1, 0,0,1, 255,255,255,255},
      {0,0,0, 0,1, 0,0,1, 255,255,255,255},
    }, "triangles")
  end

  -- Destination rect in the CURRENT target's pixel space, composed with the
  -- transform stack exactly as an image draw is.
  local w, h = target.w * sx, target.h * sy
  local px, py = apply(x - ox * sx, y - oy * sy)
  local dw, dh = w * tsx, h * tsy

  -- Pixels -> clip space. The destination may be the screen or another GPU
  -- target; W/H here is whichever is bound, which graphics.getWidth tracks.
  local vw, vh = graphics.getWidth(), graphics.getHeight()
  local cur = graphics.getCanvas()
  if cur and cur.w then vw, vh = cur.w, cur.h end
  local function clip(qx, qy)
    return (qx / vw) * 2 - 1, 1 - (qy / vh) * 2
  end
  local x0, y0 = clip(px, py)
  local x1, y1 = clip(px + dw, py + dh)

  local vs = {
    {x0,y0,0, 0,0, 0,0,1, 255,255,255,255},
    {x1,y0,0, 1,0, 0,0,1, 255,255,255,255},
    {x1,y1,0, 1,1, 0,0,1, 255,255,255,255},
    {x0,y0,0, 0,0, 0,0,1, 255,255,255,255},
    {x1,y1,0, 1,1, 0,0,1, 255,255,255,255},
    {x0,y1,0, 0,1, 0,0,1, 255,255,255,255},
  }
  blit_quad:setVertices(vs)

  -- A composite blit is a 2D operation wearing 3D clothes: it must not be
  -- depth-tested (its quad has no meaningful z against the scene's depths)
  -- and must not be culled (its winding is whatever the quad happens to
  -- be). A renderer leaves both enabled from its geometry pass, so the
  -- final draw would be discarded entirely -- which is a black screen after
  -- a pipeline that ran perfectly.
  local prevDepth, prevWrite = graphics.getDepthMode()
  local prevCull = graphics.getMeshCullMode()
  graphics.setDepthMode()
  graphics.setMeshCullMode("none")

  local prev = graphics.getShader()
  blit_shader:send("wc_blit_tex", target)
  graphics.setShader(blit_shader)
  wc.mesh3d_draw(blit_quad.id)
  graphics.setShader(prev)

  graphics.setDepthMode(prevDepth, prevWrite)
  graphics.setMeshCullMode(prevCull)
end

function graphics.draw(img, a, b, c, d, e, f, g, h)
  if not img then return end

  -- A GPU render target drawn as an image. Must come BEFORE the Image case:
  -- a Canvas3D has an `id` too, and the 2D path would look it up in the
  -- image table and find someone else's texture.
  if getmetatable(img) == Canvas3D then
    draw_gpu_target(img, a, b, c, d, e, f, g)
    return
  end

  -- a Text object draws its own pre-wrapped lines
  if getmetatable(img) == Text then
    img:draw(a, b, c, d, e, f, g)
    return
  end

  -- a ParticleSystem draws its own live particles, each with its own
  -- colour and size, so it cannot go through the image path below
  if getmetatable(img) == ParticleSystem then
    img:draw(a, b)
    return
  end

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

  -- A 3D mesh takes NO placement arguments: its transform is the cart's own
  -- matrices inside its vertex shader, and the 2D transform stack has no
  -- meaning in a perspective projection. Silently applying tx/ty here would
  -- shift a model by the stack's leftover translation, which reads as a
  -- camera bug in the cart rather than an engine one.
  if getmetatable(img) == Mesh3D then
    if not wc.mesh3d_draw(img.id) then
      error("love.graphics.draw(mesh): the 3D mesh could not be drawn. Either " ..
            "this run is on the software rasterizer (3D is GL-only), or no " ..
            "shader is bound -- a 3D draw REQUIRES love.graphics.setShader " ..
            "with a vertex stage, because that is where the model/view/" ..
            "projection transform lives. See the cart log.", 2)
    end
    return
  end

  -- a Mesh carries its own geometry; draw() supplies only the placement,
  -- composed with the transform stack exactly as it is for an image
  if getmetatable(img) == Mesh then
    local x, y, r, sx, sy, ox, oy = a, b, c, d, e, f, g
    x = x or 0; y = y or 0; r = r or 0
    sx = sx or 1; sy = sy or sx; ox = ox or 0; oy = oy or 0
    local px, py = apply(x, y)
    if not wc.mesh_draw(img.id, px, py, r + trot, sx * tsx, sy * tsy, ox, oy) then
      error("love.graphics.draw(mesh): this run is on the software rasterizer " ..
            "(no GL context, or a draw used a feature the GL backend does not " ..
            "implement), so the mesh could not be rendered. See the cart log.", 2)
    end
    return
  end

  if not img.id then return end
  local quad, x, y, r, sx, sy, ox, oy
  if rawtype(a) == "table" and a.getViewport then
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
  if rawtype(a) == "table" and a.getViewport then
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
  if rawtype(a) == "table" and a.getViewport then
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

-- ParticleSystem
--
-- The gap with a real user: Jewels hand-rolled a pooled emitter precisely
-- because this was missing, and every LOVE game that ports over expects it
-- for smoke, sparks, fire and dust.
--
-- POOLED AND FIXED-SIZE, deliberately. LOVE's newParticleSystem takes a
-- buffer size and never exceeds it; emitting past the cap recycles the
-- OLDEST particle rather than growing. That is what keeps a system that
-- emits 500/sec from allocating forever, and it is also the behaviour
-- games tune against -- a system that quietly grew would look different.
--
-- Particles are drawn individually rather than through a SpriteBatch,
-- because SpriteBatch:setColor is a no-op here and per-particle colour is
-- most of what makes a particle system look like anything.
ParticleSystem = {}          -- declared up top; see the forward-decl note
ParticleSystem.__index = ParticleSystem

local function lerp(a, b, t) return a + (b - a) * t end

-- Sample a LOVE-style keyframe list: {c1, c2, c3} interpolated across the
-- particle's life. Colours are flat groups of 4 in LOVE's own format.
local function sampleColor(list, t)
  local n = #list / 4
  if n < 1 then return 1, 1, 1, 1 end
  if n == 1 then return list[1], list[2], list[3], list[4] end
  local pos = t * (n - 1)
  local i = math.floor(pos)
  if i >= n - 1 then i = n - 2 end
  local f = pos - i
  local a, b = i * 4, (i + 1) * 4
  return lerp(list[a+1], list[b+1], f), lerp(list[a+2], list[b+2], f),
         lerp(list[a+3], list[b+3], f), lerp(list[a+4], list[b+4], f)
end

local function sampleScalar(list, t)
  local n = #list
  if n < 1 then return 1 end
  if n == 1 then return list[1] end
  local pos = t * (n - 1)
  local i = math.floor(pos)
  if i >= n - 1 then i = n - 2 end
  return lerp(list[i + 1], list[i + 2], pos - i)
end

function graphics.newParticleSystem(img, buffer)
  local ps = setmetatable({
    image = img,
    max = math.max(1, math.floor(buffer or 1000)),
    parts = {}, live = 0,
    emitting = false,
    rate = 0, emitAccum = 0,
    life1 = 1, life2 = 1,
    dir = 0, spread = 0,
    speed1 = 0, speed2 = 0,
    lacc1 = 0, lacc2 = 0, aaccX = 0, aaccY = 0,
    radial1 = 0, radial2 = 0,
    tanAcc1 = 0, tanAcc2 = 0,
    damping1 = 0, damping2 = 0,
    sizes = { 1 }, sizeVar = 0,
    rot1 = 0, rot2 = 0, spin1 = 0, spin2 = 0, spinVar = 0,
    colors = { 1, 1, 1, 1 },
    offX = nil, offY = nil,
    areaDist = "none", areaX = 0, areaY = 0, areaAngle = 0, areaRel = false,
    x = 0, y = 0,
    relativeRotation = false,
    insertMode = "top",
    quads = nil,
  }, ParticleSystem)
  for i = 1, ps.max do
    ps.parts[i] = { alive = false, x = 0, y = 0, vx = 0, vy = 0,
                    life = 0, maxLife = 1, rot = 0, spin = 0,
                    sizeSeed = 0, quad = 1 }
  end
  return ps
end

local function psRand(a, b) return a + (b - a) * love.math.random() end

-- Bring one particle to life at the emitter.
local function psSpawn(ps)
  local p
  if ps.live < ps.max then
    ps.live = ps.live + 1
    p = ps.parts[ps.live]
  else
    -- Buffer full: recycle the OLDEST, which is what LOVE does. Growing
    -- instead would change how a tuned system looks under load.
    p = ps.parts[1]
    table.remove(ps.parts, 1)
    ps.parts[ps.max] = p
  end
  p.alive = true
  p.maxLife = psRand(ps.life1, ps.life2)
  if p.maxLife <= 0 then p.maxLife = 0.0001 end
  p.life = p.maxLife

  local ox, oy = ps.x, ps.y
  -- emission area
  if ps.areaDist == "uniform" then
    ox = ox + psRand(-ps.areaX, ps.areaX)
    oy = oy + psRand(-ps.areaY, ps.areaY)
  elseif ps.areaDist == "normal" then
    ox = ox + love.math.randomNormal(ps.areaX, 0)
    oy = oy + love.math.randomNormal(ps.areaY, 0)
  elseif ps.areaDist == "ellipse" then
    local a = love.math.random() * math.pi * 2
    local r = math.sqrt(love.math.random())
    ox = ox + math.cos(a) * ps.areaX * r
    oy = oy + math.sin(a) * ps.areaY * r
  elseif ps.areaDist == "borderellipse" then
    local a = love.math.random() * math.pi * 2
    ox = ox + math.cos(a) * ps.areaX
    oy = oy + math.sin(a) * ps.areaY
  end
  p.x, p.y = ox, oy

  local ang = ps.dir + psRand(-ps.spread / 2, ps.spread / 2)
  local sp = psRand(ps.speed1, ps.speed2)
  p.vx, p.vy = math.cos(ang) * sp, math.sin(ang) * sp
  p.rot = psRand(ps.rot1, ps.rot2)
  p.spin = psRand(ps.spin1, ps.spin2)
  p.sizeSeed = love.math.random()
  p.radial = psRand(ps.radial1, ps.radial2)
  p.tangential = psRand(ps.tanAcc1, ps.tanAcc2)
  p.damping = psRand(ps.damping1, ps.damping2)
  p.lacc = psRand(ps.lacc1, ps.lacc2)
  if ps.quads then p.quad = love.math.random(#ps.quads) end
end

function ParticleSystem:update(dt)
  -- emit
  if self.emitting and self.rate > 0 then
    self.emitAccum = self.emitAccum + dt * self.rate
    while self.emitAccum >= 1 do
      self.emitAccum = self.emitAccum - 1
      psSpawn(self)
    end
  end

  local i = 1
  while i <= self.live do
    local p = self.parts[i]
    p.life = p.life - dt
    if p.life <= 0 then
      p.alive = false
      -- swap-remove keeps the live prefix dense with no allocation
      self.parts[i], self.parts[self.live] = self.parts[self.live], self.parts[i]
      self.live = self.live - 1
    else
      -- radial acceleration: away from the emitter
      local dx, dy = p.x - self.x, p.y - self.y
      local d = math.sqrt(dx * dx + dy * dy)
      if d > 1e-6 and (p.radial ~= 0 or p.tangential ~= 0) then
        local nx, ny = dx / d, dy / d
        p.vx = p.vx + nx * p.radial * dt
        p.vy = p.vy + ny * p.radial * dt
        -- tangential: perpendicular, which is what makes a vortex
        p.vx = p.vx - ny * p.tangential * dt
        p.vy = p.vy + nx * p.tangential * dt
      end
      p.vx = p.vx + self.aaccX * dt
      p.vy = p.vy + self.aaccY * dt
      if p.damping ~= 0 then
        local f = 1 - p.damping * dt
        if f < 0 then f = 0 end
        p.vx, p.vy = p.vx * f, p.vy * f
      end
      p.x = p.x + p.vx * dt
      p.y = p.y + p.vy * dt
      p.rot = p.rot + p.spin * dt
      i = i + 1
    end
  end
end

function ParticleSystem:draw(px, py)
  local img = self.image
  if not img then return end
  local ox = self.offX
  local oy = self.offY
  if not ox then
    local w, h = img:getWidth(), img:getHeight()
    ox, oy = w / 2, h / 2
  end
  local bx, by = px or 0, py or 0
  local pr, pg, pb, pa = graphics.getColor()
  for i = 1, self.live do
    local p = self.parts[i]
    local t = 1 - (p.life / p.maxLife)          -- 0 at birth, 1 at death
    local cr, cg, cb, ca = sampleColor(self.colors, t)
    local s = sampleScalar(self.sizes, t)
    if self.sizeVar > 0 then
      s = s * (1 + (p.sizeSeed * 2 - 1) * self.sizeVar)
    end
    graphics.setColor(cr * pr, cg * pg, cb * pb, ca * pa)
    graphics.draw(img, bx + p.x, by + p.y, p.rot, s, s, ox, oy)
  end
  graphics.setColor(pr, pg, pb, pa)
end

function ParticleSystem:start()  self.emitting = true end
function ParticleSystem:stop()   self.emitting = false; self:reset() end
function ParticleSystem:pause()  self.emitting = false end
function ParticleSystem:isActive() return self.emitting end
function ParticleSystem:isPaused() return not self.emitting end
function ParticleSystem:isStopped() return not self.emitting and self.live == 0 end
function ParticleSystem:reset()
  for i = 1, self.max do self.parts[i].alive = false end
  self.live, self.emitAccum = 0, 0
end
function ParticleSystem:emit(n)
  for _ = 1, (n or 1) do psSpawn(self) end
end
function ParticleSystem:getCount() return self.live end
function ParticleSystem:setBufferSize(n)
  n = math.max(1, math.floor(n))
  for i = self.max + 1, n do
    self.parts[i] = { alive = false, x = 0, y = 0, vx = 0, vy = 0,
                      life = 0, maxLife = 1, rot = 0, spin = 0,
                      sizeSeed = 0, quad = 1 }
  end
  self.max = n
  if self.live > n then self.live = n end
end
function ParticleSystem:getBufferSize() return self.max end

function ParticleSystem:setEmissionRate(r) self.rate = r end
function ParticleSystem:getEmissionRate() return self.rate end
function ParticleSystem:setParticleLifetime(a, b)
  self.life1, self.life2 = a, b or a
end
function ParticleSystem:getParticleLifetime() return self.life1, self.life2 end
function ParticleSystem:setDirection(d) self.dir = d end
function ParticleSystem:getDirection() return self.dir end
function ParticleSystem:setSpread(s) self.spread = s end
function ParticleSystem:getSpread() return self.spread end
function ParticleSystem:setSpeed(a, b) self.speed1, self.speed2 = a, b or a end
function ParticleSystem:getSpeed() return self.speed1, self.speed2 end
function ParticleSystem:setLinearAcceleration(x1, y1, x2, y2)
  -- LOVE takes a min/max BOX; the mean is what actually reads on screen,
  -- and per-axis randomisation across the box adds nothing a spread does
  -- not already give you.
  self.aaccX = ((x1 or 0) + (x2 or x1 or 0)) / 2
  self.aaccY = ((y1 or 0) + (y2 or y1 or 0)) / 2
  self.lacc1, self.lacc2 = 0, 0
end
function ParticleSystem:getLinearAcceleration()
  return self.aaccX, self.aaccY, self.aaccX, self.aaccY
end
function ParticleSystem:setRadialAcceleration(a, b)
  self.radial1, self.radial2 = a, b or a
end
function ParticleSystem:getRadialAcceleration() return self.radial1, self.radial2 end
function ParticleSystem:setTangentialAcceleration(a, b)
  self.tanAcc1, self.tanAcc2 = a, b or a
end
function ParticleSystem:getTangentialAcceleration() return self.tanAcc1, self.tanAcc2 end
function ParticleSystem:setLinearDamping(a, b)
  self.damping1, self.damping2 = a, b or a
end
function ParticleSystem:getLinearDamping() return self.damping1, self.damping2 end
function ParticleSystem:setSizes(...)
  self.sizes = { ... }
  if #self.sizes == 0 then self.sizes = { 1 } end
end
function ParticleSystem:getSizes() return unpack(self.sizes) end
function ParticleSystem:setSizeVariation(v) self.sizeVar = v end
function ParticleSystem:getSizeVariation() return self.sizeVar end
function ParticleSystem:setRotation(a, b) self.rot1, self.rot2 = a, b or a end
function ParticleSystem:getRotation() return self.rot1, self.rot2 end
function ParticleSystem:setSpin(a, b) self.spin1, self.spin2 = a, b or a end
function ParticleSystem:getSpin() return self.spin1, self.spin2 end
function ParticleSystem:setSpinVariation(v) self.spinVar = v end
function ParticleSystem:getSpinVariation() return self.spinVar end
function ParticleSystem:setColors(...)
  local a = { ... }
  -- LOVE accepts either flat numbers or a list of {r,g,b,a} tables
  if rawtype(a[1]) == "table" then
    local flat = {}
    for _, c in ipairs(a) do
      flat[#flat+1] = c[1]; flat[#flat+1] = c[2]
      flat[#flat+1] = c[3]; flat[#flat+1] = c[4] or 1
    end
    a = flat
  end
  if #a == 0 then a = { 1, 1, 1, 1 } end
  self.colors = a
end
function ParticleSystem:getColors() return unpack(self.colors) end
function ParticleSystem:setPosition(x, y) self.x, self.y = x, y end
function ParticleSystem:getPosition() return self.x, self.y end
function ParticleSystem:moveTo(x, y) self.x, self.y = x, y end
function ParticleSystem:setOffset(x, y) self.offX, self.offY = x, y end
function ParticleSystem:getOffset()
  if self.offX then return self.offX, self.offY end
  if self.image then return self.image:getWidth()/2, self.image:getHeight()/2 end
  return 0, 0
end
function ParticleSystem:setEmissionArea(dist, dx, dy, angle, rel)
  self.areaDist = dist or "none"
  self.areaX, self.areaY = dx or 0, dy or 0
  self.areaAngle, self.areaRel = angle or 0, rel or false
end
function ParticleSystem:getEmissionArea()
  return self.areaDist, self.areaX, self.areaY, self.areaAngle, self.areaRel
end
function ParticleSystem:setTexture(img) self.image = img end
function ParticleSystem:getTexture() return self.image end
function ParticleSystem:setQuads(...)
  local q = { ... }
  if rawtype(q[1]) == "table" and not q[1].getViewport then q = q[1] end
  self.quads = (#q > 0) and q or nil
end
function ParticleSystem:getQuads() return self.quads or {} end
function ParticleSystem:setRelativeRotation(v) self.relativeRotation = v end
function ParticleSystem:hasRelativeRotation() return self.relativeRotation end
function ParticleSystem:setInsertMode(m) self.insertMode = m end
function ParticleSystem:getInsertMode() return self.insertMode end
function ParticleSystem:clone()
  local c = graphics.newParticleSystem(self.image, self.max)
  for k, v in pairs(self) do
    if k ~= "parts" and k ~= "live" then c[k] = v end
  end
  c.sizes = { unpack(self.sizes) }
  c.colors = { unpack(self.colors) }
  return c
end
function ParticleSystem:type() return "ParticleSystem" end

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
  if rawtype(a) == "string" then
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

-- setNewFont(...) == setFont(newFont(...)), and RETURNS the font. Games
-- use it as a one-liner at startup; the return value is what they keep to
-- measure text with later.
function graphics.setNewFont(a, b)
  local f = graphics.newFont(a, b)
  cur_font = f
  return f
end
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

-- ── Text objects ──────────────────────────────────────────────────
--
-- love.graphics.newText holds a string and draws it as one object. In real
-- LOVE it is a genuine optimisation: the glyph quads are baked once into a
-- vertex buffer instead of re-laid-out every frame.
--
-- Here the win is smaller but real -- the WRAP is computed once at set()
-- time rather than per draw, and printf's greedy word-wrap over a long
-- string is the expensive part, not the blit. More importantly, games use
-- Text objects for their measurement API (getWidth/getHeight on wrapped
-- text is otherwise painful to compute) and for setf's alignment.
Text = {}                    -- declared up top; see the forward-decl note
Text.__index = Text

-- Lay a string out into lines, honouring an optional wrap limit. Shared
-- with printf's algorithm deliberately: two different wrap rules in one
-- engine means text measured with one and drawn with the other disagrees.
local function layoutText(font, str, limit, align)
  local lines = {}
  str = tostring(str)
  if limit then
    for paragraph in (str .. "\n"):gmatch("([^\n]*)\n") do
      if paragraph == "" then
        lines[#lines + 1] = ""
      else
        local cur = nil
        for word in paragraph:gmatch("%S+") do
          local try = cur and (cur .. " " .. word) or word
          if wc.text_size(try, font.id, font.scale) <= limit or not cur then
            cur = try
          else
            lines[#lines + 1] = cur
            cur = word
          end
        end
        if cur then lines[#lines + 1] = cur end
      end
    end
  else
    for line in (str .. "\n"):gmatch("([^\n]*)\n") do
      lines[#lines + 1] = line
    end
  end
  return lines
end

local function textRemeasure(t)
  local w = 0
  for _, line in ipairs(t.lines) do
    local lw = wc.text_size(line, t.font.id, t.font.scale)
    if lw > w then w = lw end
  end
  t.w = t.limit or w
  t.rawW = w
  t.h = math.max(1, #t.lines) * (t.font.px * 1.2)
end

function graphics.newText(font, str)
  local t = setmetatable({ font = font or graphics.getFont(),
                           lines = {}, w = 0, rawW = 0, h = 0,
                           limit = nil, align = "left" }, Text)
  if str then t:set(str) end
  return t
end

function Text:set(str)
  if str == nil or str == "" then
    self.lines, self.limit, self.align = {}, nil, "left"
    self.w, self.rawW, self.h = 0, 0, 0
    return
  end
  self.limit, self.align = nil, "left"
  self.lines = layoutText(self.font, str, nil, "left")
  textRemeasure(self)
end

function Text:setf(str, limit, align)
  if str == nil or str == "" then
    self.lines = {}
    self.w, self.rawW, self.h = 0, 0, 0
    return
  end
  self.limit, self.align = limit, align or "left"
  self.lines = layoutText(self.font, str, limit, self.align)
  textRemeasure(self)
end

function Text:add(str, x, y)
  -- LOVE appends and returns an index. Appending as its own line keeps
  -- measurement honest; a game using add() for inline runs would need the
  -- real vertex-buffer version, and this at least does not lie about size.
  local extra = layoutText(self.font, str, nil, "left")
  for _, l in ipairs(extra) do self.lines[#self.lines + 1] = l end
  textRemeasure(self)
  return #self.lines
end
Text.addf = Text.add

function Text:clear()
  self.lines = {}
  self.w, self.rawW, self.h = 0, 0, 0
end

function Text:getWidth()  return self.w end
function Text:getHeight() return self.h end
function Text:getDimensions() return self.w, self.h end
function Text:getFont() return self.font end
function Text:setFont(f)
  self.font = f
  textRemeasure(self)
end
function Text:type() return "Text" end

function Text:draw(x, y, r, sx, sy, ox, oy)
  local f = self.font
  local lineH = f.px * 1.2
  local limit = self.limit
  for i, line in ipairs(self.lines) do
    local lw = wc.text_size(line, f.id, f.scale)
    local px = x or 0
    if limit then
      if self.align == "center" then px = px + (limit - lw) / 2
      elseif self.align == "right" then px = px + limit - lw end
    end
    local ax, ay = apply(px, y or 0)
    wc.print(line, ax, ay + (i - 1) * lineH, f.id, f.scale)
  end
end

-- transform stack
-- push([stacktype]) / pop()
--
-- LOVE's stack saves the TRANSFORM, and with push("all") the whole render
-- state -- canvas, shader, colour, blend mode, scissor. A renderer leans on
-- that hard: 3DreamEngine's final pass is `love.graphics.pop()` followed by
-- a draw, and the pop is what puts rendering back on the SCREEN after its
-- passes have each bound their own canvas.
--
-- Saving only the transform (what this used to do) made that pop a no-op,
-- so the final composite drew into whichever 640x360 bloom canvas was still
-- bound. The screen stayed black and every pass had "worked". Restoring the
-- canvas is what makes the picture arrive.
--
-- The state is captured on EVERY push, not only push("all"): a renderer
-- that pushes plain and pops expecting its canvas back is relying on
-- LOVE's actual behaviour, and the cost is one small table.
-- The transform stack REUSES its frames.
--
-- push() used to build a fresh mixed table (5 array slots + 7 hash keys) on
-- every call, and a mixed table is the most expensive shape Lua allocates.
-- Both carts and the 3D renderer push/pop many times a frame, so this was
-- pure per-frame garbage for a structure whose shape never changes. The
-- frames are private to this stack and never escape -- pop() copies the
-- values straight back into locals -- so keeping them around is safe.
-- tdepth is the live depth; frames above it are stale but harmless.
local tdepth = 0

function graphics.push(stacktype)
  local d = tdepth + 1
  tdepth = d
  local t = tstack[d]
  if not t then
    t = { 0, 0, 0, 0, 0, 0, 0, canvas = false, shader = false,
          r = 0, g = 0, b = 0, a = 0, all = false }
    tstack[d] = t
  end
  t[1], t[2], t[3], t[4], t[5] = tx, ty, tsx, tsy, trot
  -- shear is part of the transform and MUST be saved with it, or a push/pop
  -- pair silently leaks a shear into whatever drew next
  t[6], t[7] = tkx, tky
  t.canvas = cur_canvas
  t.shader = cur_shader
  t.r, t.g, t.b, t.a = cr, cg, cb, ca
  t.all = stacktype == "all"
end

-- Internal: drop the stack to empty without discarding the pooled frames.
function graphics.__resetStack() tdepth = 0 end

function graphics.pop()
  local d = tdepth
  if d < 1 then return end
  tdepth = d - 1
  local t = tstack[d]
  if not t then return end
  tx, ty, tsx, tsy, trot = t[1], t[2], t[3], t[4], t[5]
  tkx, tky = t[6] or 0, t[7] or 0
  -- Restore the render target only when it actually changed, so a pop does
  -- not re-bind (and re-clear the clip scale for) the target already bound.
  if t.canvas ~= cur_canvas then
    graphics.setCanvas(t.canvas)
  end
  if t.all then
    if t.shader ~= cur_shader then graphics.setShader(t.shader) end
    graphics.setColor(t.r / 255, t.g / 255, t.b / 255, t.a / 255)
  end
end

-- translate composes THROUGH the current rotation+scale, so a translate
-- after a rotate moves along the rotated axes (matrix order, like LOVE).
function graphics.translate(x, y)
  local dx, dy = apply(x or 0, y or 0)
  tx, ty = dx, dy
end
function graphics.scale(sx, sy) tsx = tsx * (sx or 1); tsy = tsy * (sy or sx or 1) end
function graphics.rotate(r) trot = trot + (r or 0) end
function graphics.origin() tx, ty, tsx, tsy, trot = 0, 0, 1, 1, 0; tkx, tky = 0, 0 end

function graphics.shear(kx, ky)
  tkx = tkx + (kx or 0)
  tky = tky + (ky or 0)
end

-- Map a point through the CURRENT transform, and back again. Games use
-- these to turn a mouse position into world space, which is otherwise
-- guesswork once the camera has scaled or rotated.
function graphics.transformPoint(x, y) return apply(x, y) end

function graphics.inverseTransformPoint(x, y)
  -- undo in reverse order: translate, rotate, scale, shear
  x, y = x - tx, y - ty
  if trot ~= 0 then
    local c, sn = math.cos(-trot), math.sin(-trot)
    x, y = x * c - y * sn, x * sn + y * c
  end
  if tsx ~= 0 then x = x / tsx end
  if tsy ~= 0 then y = y / tsy end
  if tkx ~= 0 or tky ~= 0 then
    -- invert [1 kx; ky 1], determinant 1 - kx*ky
    local det = 1 - tkx * tky
    if det ~= 0 then
      x, y = (x - y * tkx) / det, (y - x * tky) / det
    end
  end
  return x, y
end

-- love.math.Transform objects. applyTransform composes one onto the
-- current state; replaceTransform overwrites it outright.
function graphics.applyTransform(t)
  graphics.translate(t.tx, t.ty)
  graphics.rotate(t.rot)
  graphics.scale(t.sx, t.sy)
  graphics.shear(t.kx, t.ky)
end

function graphics.replaceTransform(t)
  tx, ty = t.tx, t.ty
  tsx, tsy = t.sx, t.sy
  trot = t.rot
  tkx, tky = t.kx, t.ky
end

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

-- Clip to the INTERSECTION of the current scissor and this rect.
--
-- The nesting primitive: a UI library clips to a panel, then a widget
-- inside it clips to its own bounds and must not be able to draw outside
-- the panel. Plain setScissor would let the inner one widen the clip,
-- which is the bug that puts a dropdown outside its own window.
--
-- With no scissor set, LOVE treats this as a plain setScissor.
function graphics.intersectScissor(x, y, w, h)
  if not x then return graphics.setScissor() end
  if not sc_rect then return graphics.setScissor(x, y, w, h) end
  local ax, ay, aw, ah = sc_rect[1], sc_rect[2], sc_rect[3], sc_rect[4]
  local x1 = math.max(ax, x)
  local y1 = math.max(ay, y)
  local x2 = math.min(ax + aw, x + w)
  local y2 = math.min(ay + ah, y + h)
  -- A non-overlapping intersection is EMPTY, not "unclipped". Clamping to
  -- zero rather than letting w/h go negative is what stops a disjoint
  -- clip from drawing the whole screen.
  graphics.setScissor(x1, y1, math.max(0, x2 - x1), math.max(0, y2 - y1))
end

-- Line width is not implemented by the rasterizer (all lines are 1px), but
-- games set it constantly. Store and report it so round-trips are honest
-- rather than erroring mid-draw.
local line_width = 1
function graphics.setLineWidth(w) line_width = w or 1 end
function graphics.getLineWidth() return line_width end
function graphics.setLineStyle() end
function graphics.getLineStyle() return "rough" end

-- Paired getters for state a cart can set.
--
-- A getter whose setter exists is not decoration: LOVE code reads state to
-- restore it (set, draw, set back), and a missing getter turns that idiom
-- into a nil arithmetic error deep in someone's library. These store what
-- was asked for and report it back truthfully, including where the
-- rasterizer cannot honour it -- an honest round-trip beats a hard failure.
local line_join = "miter"
function graphics.setLineJoin(j) line_join = j or "miter" end
function graphics.getLineJoin() return line_join end

local point_size = 1
function graphics.setPointSize(sz) point_size = sz or 1 end
function graphics.getPointSize() return point_size end

local default_min, default_mag, default_aniso = "linear", "linear", 1
function graphics.setDefaultFilter(mi, ma, an)
  default_min = mi or "linear"
  default_mag = ma or default_min
  default_aniso = an or 1
end
function graphics.getDefaultFilter()
  return default_min, default_mag, default_aniso
end

local blend_mode, blend_alpha = "alpha", "alphamultiply"
function graphics.setBlendMode(mode, alphamode)
  blend_mode = mode or "alpha"
  blend_alpha = alphamode or "alphamultiply"
  wc.set_blend(mode == "add" and 1 or 0)
end
function graphics.getBlendMode() return blend_mode, blend_alpha end

-- Pixel vs logical size. A cart's framebuffer IS its pixel buffer -- there
-- is no OS DPI scaling between them -- so these agree with the logical size
-- and the scale is 1. Reported rather than omitted because libraries branch
-- on them to decide whether to draw at 2x.
function graphics.getDPIScale() return 1 end
function graphics.getPixelWidth() return graphics.getWidth() end
function graphics.getPixelHeight() return graphics.getHeight() end
function graphics.getPixelDimensions()
  return graphics.getWidth(), graphics.getHeight()
end

-- Renderer identity. Real LOVE returns the GL strings; a cart cannot see
-- the GL context (that is the whole point of the boundary), so this names
-- the engine truthfully instead of pretending to be desktop OpenGL.
function graphics.getRendererInfo()
  return "wasmcart-lua", "1.0", "wasmcart", "GLES3/WebGL2"
end

function graphics.getStackDepth() return tdepth end

-- Stencil is not implemented. Report it OFF rather than erroring, so the
-- common "read it, set it, restore it" idiom round-trips.
function graphics.getStencilTest() return "always", 0 end

-- ── shaders ────────────────────────────────────────────────────────
--
-- LOVE's shader shape, on the GL2D renderer's own program contract.
--
-- A cart writes only the LOVE-shaped body:
--
--   local s = love.graphics.newShader[[
--     vec4 effect(vec4 color, Image tex, vec2 texture_coords, vec2 screen_coords) {
--       vec4 px = Texel(tex, texture_coords);
--       return vec4(1.0 - px.rgb, px.a) * color;
--     }
--   ]]
--
-- Everything around it -- the "#version 300 es" line, the ins and outs, the
-- Texel/love_ScreenSize predefines, and a main() that reproduces the
-- engine's own solid/texture/glyph/circle dispatch -- is synthesized in C
-- (see SHADER_FRAG_PROLOGUE in render2d_gl.c). That is what lets a shader
-- apply uniformly to rectangles, sprites, text and circles instead of only
-- to textured draws.
--
-- The surface is WebGL2 / GLES 3.0. A shader that writes its own #version,
-- or uses GLSL ES 1.00 spellings (gl_FragColor, texture2D, varying,
-- attribute) or anything from GLES 3.1+, is refused BY NAME rather than
-- handed to the driver, because a driver error points at line numbers in
-- generated code the cart author never wrote.

local Shader = {}
Shader.__index = Shader
function Shader:type() return "Shader" end
function Shader:getHandle() return self.id end

-- Shader:send(name, value, ...)
--
-- Accepts what LOVE's does minus the types this engine has no object for:
--   number, number...      -> float / vecN
--   { a, b, c }            -> vecN (N = #table, up to 4)
--   { {..},{..},{..},{..} } -> mat4
--   boolean...             -> bool / bvecN
--   Image or Canvas        -> sampler2D
--
-- A name that is not in the LINKED program returns false rather than
-- erroring: GLSL compilers remove uniforms that do not affect the output, so
-- a live uniform can vanish the moment a cart comments out a line, and
-- throwing there would be worse than useless. A name that was never written
-- at all is the same case and is reported the same way.
function Shader:send(name, a, ...)
  if rawtype(name) ~= "string" then
    error("Shader:send: the uniform name must be a string", 2)
  end
  if rawtype(a) == "table" then
    -- A GPU render target binds as a sampler of its own texture TYPE
    -- (samplerCube / sampler2DArray / sampler3D / sampler2D), which the 2D
    -- image path has no uniform for. Each gets its own texture unit,
    -- assigned per shader in order of first use, because unit 0 belongs to
    -- the 2D batcher and would be clobbered by the next sprite.
    if getmetatable(a) == Canvas3D then
      self._units = self._units or {}
      local unit = self._units[name]
      if not unit then
        self._next_unit = (self._next_unit or 0) + 1
        unit = self._next_unit
        if unit > 15 then
          error("Shader:send('" .. name .. "'): out of texture units (15 max " ..
                "per shader)", 2)
        end
        self._units[name] = unit
      end
      return wc.target_send(self.id, name, a.id, unit)
    end
    if a.id and (a.w or a.canvas) then          -- an Image or a Canvas
      return wc.shader_send_image(self.id, name, a.id)
    end
    return wc.shader_send(self.id, name, a)
  end
  if rawtype(a) == "boolean" then
    return wc.shader_send_bool(self.id, name, a, ...)
  end
  if rawtype(a) == "number" then
    return wc.shader_send(self.id, name, a, ...)
  end
  error("Shader:send('" .. name .. "'): unsupported value type '" ..
        rawtype(a) .. "'. Supported: number(s), a table of 1-4 numbers, a 4x4 " ..
        "table-of-tables, boolean(s), an Image or a Canvas.", 2)
end

-- LOVE's introspection helper, and a READ-ONLY one. The obvious shortcut --
-- "try sending 0 and see whether it took" -- writes the uniform, so merely
-- asking whether a uniform exists would zero it. Games call hasUniform to
-- guard an optional uniform immediately before setting it, where that
-- clobber is invisible right up until it is not.
function Shader:hasUniform(name)
  return wc.shader_has_uniform(self.id, name)
end

function Shader:release() return true end

-- newShader(pixelcode) | newShader(pixelcode, vertexcode)
--
-- LOVE also accepts filenames. A cart's GLSL lives in its asset bundle, so a
-- string that names an existing asset is read from it; anything else is
-- treated as source, which is how the inline [[...]] idiom works.
local function shader_source(s)
  if rawtype(s) ~= "string" then return nil end
  if not s:find("[\n{;]") and wc.asset_exists(s) then
    local src = wc.asset_read(s)
    if not src then error("could not read shader asset: " .. s, 3) end
    return src
  end
  return s
end

-- Which of the two arguments is the pixel shader and which is the vertex one
-- is decided by CONTENT, not position: LOVE allows either order, and games
-- pass them either way.
-- NOTE: ipairs is wrong here. newShader(nil, vertexcode) is legal -- a
-- vertex-only shader keeps the engine's default fragment stage -- and
-- ipairs({nil, v}) stops at index 1, so the vertex source was dropped and
-- the call failed with "no shader source given". Index explicitly.
local function shader_split(a, b)
  -- ONE source containing BOTH stages is the standard LOVE shader file:
  -- the whole thing is compiled twice, guarded by `#ifdef VERTEX` and
  -- `#ifdef PIXEL`, and the engine's prologues define exactly one of those
  -- per stage. Assigning such a source to a single stage (which this used to
  -- do, since it matched `position()` first) drops the other half entirely
  -- and reports "a 3D shader must supply a vertex stage" about a file that
  -- plainly contains one.
  if a and not b then
    local has_pos = a:find("vec4%s+position%s*%(")
    local has_eff = a:find("effect%s*%(")
    if has_pos and has_eff then return a, a end
  end

  local pixel, vertex
  for i = 1, 2 do
    local s = (i == 1) and a or b
    if s then
      if s:find("vec4%s+position%s*%(") then vertex = s
      elseif s:find("effect%s*%(") then pixel = s
      elseif not pixel then pixel = s
      else vertex = s end
    end
  end
  return pixel, vertex
end

-- Is this a 3D shader?
--
-- LOVE has no flag for it: the same newShader builds both, and which vertex
-- ATTRIBUTES exist is decided by the mesh the shader is later drawn with.
-- This engine has to choose a prologue at compile time, so it infers from
-- the source, and the signal is VertexNormal -- the one attribute that
-- exists only in the 3D layout. Every LOVE 3D shader declares or reads it
-- (g3d's `attribute vec3 VertexNormal;` is the canonical line), and no 2D
-- shader mentions it, since there is nothing to mention.
--
-- A cart can force the choice with a `#pragma wasmcart 3d` line, which is
-- the escape hatch for a 3D shader that genuinely ignores normals. The
-- pragma is stripped before the source reaches GL.
local function shader_is_3d(pixel, vertex)
  for _, s in ipairs({ vertex or "", pixel or "" }) do
    if s:find("#pragma%s+wasmcart%s+3d") then return true end
    if s:find("#pragma%s+wasmcart%s+mrt") then return true end
    if s:find("VertexNormal") then return true end
  end
  return false
end

function graphics.newShader(a, b)
  local pixel, vertex = shader_split(shader_source(a), shader_source(b))
  if not pixel and not vertex then
    error("love.graphics.newShader: no shader source given", 2)
  end
  local is3d = shader_is_3d(pixel, vertex)

  -- `#pragma wasmcart mrt N` selects the multi-output fragment scaffold: the
  -- shader then defines `void effect2(out vec4 c0, ..., out vec4 cN-1)` and
  -- writes one colour per bound render target. This is how a deferred
  -- renderer's geometry pass fills a g-buffer in a single pass.
  local mrt = 0
  for _, s in ipairs({ pixel or "", vertex or "" }) do
    local n = s:match("#pragma%s+wasmcart%s+mrt%s+(%d+)")
    if n then mrt = math.floor(tonumber(n)) break end
  end

  -- LOVE's OWN multi-target form: `void effect()` writing love_Canvases[i].
  -- Real LOVE renderers are written this way (3DreamEngine's sky and
  -- g-buffer shaders both are), so it is detected rather than requiring the
  -- pragma. The target count is the highest index the source touches, plus
  -- one -- there is nothing else in the shader that states it.
  if mrt == 0 and pixel and pixel:find("love_Canvases") then
    local highest = -1
    for idx in pixel:gmatch("love_Canvases%s*%[%s*(%d+)%s*%]") do
      local i = tonumber(idx)
      if i > highest then highest = i end
    end
    if highest >= 0 then
      -- Negative marks the love_Canvases form for the C side.
      mrt = -(highest + 1)
    end
  end
  if mrt ~= 0 then
    is3d = true                       -- MRT implies the 3D vertex layout
    if math.abs(mrt) > 8 then
      error("love.graphics.newShader: at most 8 render targets (asked for " ..
            math.abs(mrt) .. ")", 2)
    end
  end

  -- GLSL has no #pragma of ours; strip them rather than let the driver warn
  -- about an unknown one on every compile.
  local function strip(s)
    if not s then return nil end
    s = s:gsub("#pragma%s+wasmcart%s+3d", "")
    s = s:gsub("#pragma%s+wasmcart%s+mrt%s+%d+", "")
    return s
  end
  if is3d or mrt ~= 0 then pixel, vertex = strip(pixel), strip(vertex) end

  local id = wc.shader_new(pixel, vertex, is3d, mrt)
  if not id then
    -- the GL info log has already gone to wc_log with the real compiler
    -- message; this is the Lua-visible failure so a cart cannot carry on
    -- believing it has a shader
    error("love.graphics.newShader: the shader did not compile or link " ..
          "(the GL info log was written to the cart log)", 2)
  end
  return setmetatable({ id = id }, Shader)
end


-- setShader(shader) | setShader() to revert to the engine's default program
function graphics.setShader(s)
  if s == nil then
    cur_shader = nil
    wc.shader_use(nil)
    return
  end
  if getmetatable(s) ~= Shader then
    error("love.graphics.setShader: expected a Shader from " ..
          "love.graphics.newShader", 2)
  end
  if not wc.shader_use(s.id) then
    error("love.graphics.setShader: this run is on the software rasterizer " ..
          "(no GL context, or a draw used a feature the GL backend does not " ..
          "implement), so a shader cannot be applied. See the cart log.", 2)
  end
  cur_shader = s
end

function graphics.getShader() return cur_shader end

-- ── meshes ─────────────────────────────────────────────────────────
--
-- LOVE's default vertex format, and ONLY that format:
--
--   { x, y, u, v, r, g, b, a }   -- colour 0..1, defaulting to opaque white
--
-- which is not a shortcut, it is the whole reason meshes fit this engine.
-- The GL backend's vertex is
--     struct { float x, y, u, v, r, g, b, a, rad; }
-- so a LOVE default-format vertex IS an engine vertex (rad is the circle
-- rule's extra, zero for a mesh). No repacking, no second vertex layout, no
-- second VAO.
--
-- A CUSTOM vertex format -- newMesh(vertexformat, ...) -- is refused, not
-- approximated. The vertex layout is fixed at engine init by one shared VAO
-- that every program, including every cart shader, is bound against; an
-- arbitrary attribute set would need its own buffer, its own VAO and a
-- shader contract this engine does not have. Saying so is better than
-- silently dropping the cart's extra attributes and rendering something
-- that looks nearly right.
--
-- The vertices live in C (see l_mesh_new in runtime.c), not in this table: a
-- mesh is redrawn every frame and marshalling its floats across the boundary
-- each time would cost more than the draw.
--
-- Meshes are GL-only. There is no software path that rasterizes a textured,
-- per-vertex-coloured triangle, and rather than invent an approximate one
-- that disagrees with GL, newMesh refuses on a host with no GL -- the same
-- rule newShader follows.

Mesh = {}
Mesh.__index = Mesh
function Mesh:type() return "Mesh" end

local MESH_MODES = { fan = 0, strip = 1, triangles = 2 }

-- setVertex(i, x, y, u, v, r, g, b, a) | setVertex(i, {x, y, u, v, r, g, b, a})
function Mesh:setVertex(i, a, b, c, d, e, f, g, h)
  if rawtype(a) == "table" then
    a, b, c, d, e, f, g, h = a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]
  end
  wc.mesh_set_vertex(self.id, i - 1, a or 0, b or 0, c or 0, d or 0,
                     e or 1, f or 1, g or 1, h or 1)
end

function Mesh:getVertex(i)
  return wc.mesh_get_vertex(self.id, i - 1)
end

function Mesh:getVertexCount() return self.n end

-- setVertices(vertices, [startindex])
function Mesh:setVertices(verts, start)
  start = start or 1
  for k = 1, #verts do
    local v = verts[k]
    local i = start + k - 1
    if i > self.n then break end
    wc.mesh_set_vertex(self.id, i - 1, v[1] or 0, v[2] or 0, v[3] or 0, v[4] or 0,
                       v[5] or 1, v[6] or 1, v[7] or 1, v[8] or 1)
  end
end

function Mesh:setTexture(img)
  if img == nil then
    self.tex = nil
    wc.mesh_set_texture(self.id, nil)
    return
  end
  if rawtype(img) ~= "table" or not img.id then
    error("Mesh:setTexture: expected an Image or a Canvas, got " .. rawtype(img), 2)
  end
  self.tex = img
  wc.mesh_set_texture(self.id, img.id)
end

function Mesh:getTexture() return self.tex end

-- setVertexMap(map) | setVertexMap(a, b, c, ...) -- LOVE's indices are
-- 1-BASED and C's are 0-based, so the conversion happens once, here.
function Mesh:setVertexMap(a, ...)
  if a == nil then
    wc.mesh_set_map(self.id, nil)
    return
  end
  local src = (rawtype(a) == "table") and a or { a, ... }
  local out = {}
  for i = 1, #src do out[i] = src[i] - 1 end
  wc.mesh_set_map(self.id, out)
end

function Mesh:getVertexMap()
  local m = wc.mesh_get_map(self.id)
  if not m then return nil end
  for i = 1, #m do m[i] = m[i] + 1 end
  return m
end

-- setDrawRange(start, count) | setDrawRange() to clear
function Mesh:setDrawRange(start, count)
  if start == nil then
    wc.mesh_set_range(self.id, nil)
    return
  end
  wc.mesh_set_range(self.id, start - 1, count)
end

function Mesh:getDrawRange()
  local s, c = wc.mesh_get_range(self.id)
  if not s then return nil end
  return s + 1, c
end

function Mesh:getDrawMode() return self.mode end
function Mesh:release()
  wc.mesh_release(self.id)
  return true
end

-- Attributes are a custom-vertex-format concept, so they are refused for the
-- same reason the format itself is.
Mesh.attachAttribute = err("Mesh:attachAttribute",
  "this engine's vertex layout is fixed (x,y,u,v,r,g,b,a), so there are no " ..
  "extra attributes to attach")
Mesh.getVertexFormat = function()
  return { {"VertexPosition", "float", 2},
           {"VertexTexCoord", "float", 2},
           {"VertexColor",    "byte",  4} }
end

-- ── 3D meshes ───────────────────────────────────────────────────────
--
-- A separate object from Mesh, not a mode of it. The two share a name in
-- LOVE's API and nothing else: this one's vertices live in a GPU buffer that
-- is written once, it has no transform arguments on draw (the cart's shader
-- does that), and it is drawn against the depth buffer. Merging them would
-- mean every 2D mesh method needed a "which kind am I" branch.
--
-- Vertex layout, matching what the C side reads (mesh3d_read_vertex):
--     {x, y, z, u, v, nx, ny, nz, r, g, b, a}
-- Everything past z is optional. This is exactly g3d's vertexFormat order,
-- which is not a coincidence -- that order is the de-facto LOVE 3D layout.
Mesh3D = {}
Mesh3D.__index = Mesh3D
function Mesh3D:type() return "Mesh" end
function Mesh3D:typeOf(t) return t == "Mesh" or t == "Object" end
function Mesh3D:getVertexCount() return self.n end
function Mesh3D:getDrawMode() return "triangles" end

function Mesh3D:setVertices(verts)
  -- A ByteData of already-interleaved vertex bytes. This is the fast path a
  -- renderer uses when it packed the buffer itself (through ffi), and it
  -- goes straight to the GPU with no per-vertex marshalling at all.
  if rawtype(verts) == "table" and verts._bytes then
    if not wc.mesh3d_set_bytes(self.id, verts:getString()) then
      error("Mesh:setVertices: the byte upload failed. The data must not be " ..
            "larger than the buffer allocated at newMesh (" .. self.n ..
            " vertices).", 2)
    end
    return
  end
  if rawtype(verts) ~= "table" then
    error("Mesh:setVertices: expected a table of vertices, or a ByteData of " ..
          "packed vertex bytes", 2)
  end
  -- A DECLARED-format mesh has its own stride and attribute set, so its
  -- vertices cannot go through the fixed-layout updater -- that one writes
  -- 12 floats per vertex regardless, which overruns a smaller stride and
  -- fails with GL_INVALID_VALUE (or, worse, silently scrambles geometry).
  if self.generic then
    if not wc.mesh3d_set_vertices_format(self.id, self.fmt, verts) then
      error("Mesh:setVertices: the update failed. A declared-format mesh's " ..
            "buffer is allocated at newMesh and cannot grow past " ..
            self.n .. " vertices.", 2)
    end
    self.n = #verts
    return
  end
  if not wc.mesh3d_set_vertices(self.id, verts) then
    error("Mesh:setVertices: the update failed. A 3D mesh's vertex buffer is " ..
          "allocated at newMesh and cannot grow, so the new list must not be " ..
          "longer than the original (" .. self.n .. ").", 2)
  end
  self.n = #verts
end

function Mesh3D:setTexture(img)
  if img == nil then
    wc.mesh3d_set_texture(self.id, nil)
    self.tex = nil
    return
  end
  if rawtype(img) ~= "table" or not img.id then
    error("Mesh:setTexture: expected an Image or a Canvas, got " ..
          rawtype(img), 2)
  end
  if not wc.mesh3d_set_texture(self.id, img.id) then
    error("Mesh:setTexture: the texture could not be uploaded", 2)
  end
  self.tex = img
end
function Mesh3D:getTexture() return self.tex end

-- setVertexMap(indices) | setVertexMap(a, b, c, ...)
-- setVertexMap(indices) | setVertexMap(a, b, c, ...) |
-- setVertexMap(byteData, "uint16"|"uint32")
--
-- The ByteData form is how a renderer that built its index buffer through
-- ffi hands it over -- already packed, no per-index marshalling. LOVE's
-- datatype argument names the element width.
function Mesh3D:setVertexMap(a, b, ...)
  if rawtype(a) == "table" and a._bytes then
    local width = (b == "uint16") and 2 or 4
    if not wc.mesh3d_set_map_bytes(self.id, a:getString(), width) then
      error("Mesh:setVertexMap: the packed index upload failed", 2)
    end
    self.map_n = #a._bytes // width
    return
  end
  local map = a
  if rawtype(a) == "number" then map = { a, b, ... } end
  if map ~= nil and rawtype(map) ~= "table" then
    error("Mesh:setVertexMap: expected a table of indices, or a ByteData of " ..
          "packed indices", 2)
  end
  wc.mesh3d_set_map(self.id, map)
  self.map_n = map and #map or 0
end

function Mesh3D:release()
  wc.mesh3d_release(self.id)
  self.id = -1
  return true
end

-- The methods a 3D mesh genuinely cannot answer, refused by name rather than
-- returning a plausible wrong value. Vertices live only in the GPU buffer
-- here (that is the point), so there is nothing on this side to read back.
local function mesh3d_err(name, why)
  return function()
    error("Mesh:" .. name .. ": " .. why, 2)
  end
end
Mesh3D.getVertex = mesh3d_err("getVertex",
  "a 3D mesh keeps its vertices only in the GPU buffer, so they cannot be " ..
  "read back. Keep the source table if the cart needs to inspect them.")
Mesh3D.setVertex = mesh3d_err("setVertex",
  "a 3D mesh has no per-vertex write path. Rebuild the vertex list and call " ..
  "Mesh:setVertices, which replaces the buffer in one upload.")
Mesh3D.setVertexAttribute = mesh3d_err("setVertexAttribute",
  "not supported; use Mesh:setVertices.")
Mesh3D.attachAttribute = mesh3d_err("attachAttribute",
  "this engine has one fixed 3D vertex layout, so there is no second buffer " ..
  "to attach.")
Mesh3D.setDrawRange = mesh3d_err("setDrawRange",
  "not supported on a 3D mesh; use a vertex map to select geometry.")

-- A mesh with a fully DECLARED vertex format: arbitrary named attributes,
-- in the cart's own order. This is what a real renderer needs -- normal
-- mapping wants a tangent, PBR wants material terms, and neither fits the
-- fixed layout.
--
-- The vertices arrive as flat arrays of components in FORMAT ORDER (LOVE's
-- convention), and the C side packs them to the format's stride. A
-- table-of-named-fields form is also accepted, since 3DreamEngine builds
-- its vertices that way.
new_mesh_generic = function(fmt, verts, mode, usage)
  -- newMesh(format, COUNT, ...) allocates an empty buffer to be filled by
  -- Mesh:setVertices later, which is how a renderer that packs its own
  -- interleaved bytes builds geometry: it wants the buffer first and writes
  -- the ByteData into it. 3DreamEngine does exactly this.
  -- ByteData of ALREADY-PACKED interleaved vertices. This is the path a
  -- renderer that owns its own memory takes: it built the buffer through
  -- ffi to the exact format stride, so there is nothing to marshal -- the
  -- bytes go straight to the GPU. 3DreamEngine's meshFormat:create returns
  -- precisely this.
  if rawtype(verts) == "table" and verts._bytes then
    local stride = 0
    for _, attr in ipairs(fmt) do
      stride = stride + ((attr[2] == "byte") and 4 or (attr[3] * 4))
    end
    local count = math.floor(#verts._bytes / stride)
    if count < 1 then
      error("love.graphics.newMesh: the packed vertex data is smaller than " ..
            "one vertex (" .. #verts._bytes .. " bytes, stride " .. stride ..
            ")", 3)
    end
    local id, why = wc.mesh3d_new_format_empty(fmt, count)
    if not id then
      if why == "nogl" then
        error("love.graphics.newMesh: this run is on the software rasterizer " ..
              "(no GL context), and meshes with a declared vertex format are " ..
              "GL-only.", 3)
      end
      error("love.graphics.newMesh: the mesh could not be created (" ..
            tostring(why) .. ")", 3)
    end
    wc.mesh3d_set_bytes(id, verts:getString())
    return setmetatable({ id = id, n = count, fmt = fmt, generic = true },
                        Mesh3D)
  end

  -- A count may also arrive as an OBJECT that knows its own size.
  if rawtype(verts) == "table" and #verts == 0 and rawtype(verts.getSize) == "function" then
    verts = verts:getSize()
  end
  if rawtype(verts) == "number" then
    local count = math.floor(verts)
    if count < 1 then
      error("love.graphics.newMesh: vertex count must be >= 1", 3)
    end
    local id, why = wc.mesh3d_new_format_empty(fmt, count)
    if not id then
      if why == "nogl" then
        error("love.graphics.newMesh: this run is on the software rasterizer " ..
              "(no GL context), and meshes with a declared vertex format are " ..
              "GL-only.", 3)
      end
      error("love.graphics.newMesh: the mesh could not be created (" ..
            tostring(why) .. ")", 3)
    end
    return setmetatable({ id = id, n = count, fmt = fmt, generic = true },
                        Mesh3D)
  end
  if rawtype(verts) ~= "table" or #verts < 1 then
    error("love.graphics.newMesh: the vertex list is empty", 3)
  end
  if mode ~= nil and mode ~= "triangles" then
    error("love.graphics.newMesh: a mesh with a declared vertex format must " ..
          "use the \"triangles\" draw mode (got \"" .. tostring(mode) ..
          "\").", 3)
  end

  -- Named-field vertices ({VertexPositionX = ..., ...}) are flattened into
  -- component order here, so the C packer sees one shape.
  local flat = verts
  if rawtype(verts[1]) == "table" and verts[1][1] == nil then
    flat = {}
    local SUF = { "X", "Y", "Z", "W" }
    for i = 1, #verts do
      local v, row = verts[i], {}
      for _, attr in ipairs(fmt) do
        local nm, _, comps = attr[1], attr[2], attr[3]
        if attr[2] == "byte" then comps = 4 end
        for c = 1, comps do
          -- LOVE names a 1-component attribute plainly and a multi-component
          -- one with an X/Y/Z/W suffix.
          local key = (comps == 1) and nm or (nm .. SUF[c])
          row[#row + 1] = v[key] or 0
        end
      end
      flat[i] = row
    end
  end

  local id, why = wc.mesh3d_new_format(fmt, flat)
  if not id then
    if why == "nogl" then
      error("love.graphics.newMesh: this run is on the software rasterizer " ..
            "(no GL context), and meshes with a declared vertex format are " ..
            "GL-only.", 3)
    elseif why == "attribs" then
      error("love.graphics.newMesh: a vertex format may declare at most 8 " ..
            "attributes", 3)
    end
    error("love.graphics.newMesh: the mesh could not be created (" ..
          tostring(why) .. ")", 3)
  end
  return setmetatable({ id = id, n = #flat, fmt = fmt, generic = true }, Mesh3D)
end

function new_mesh_3d(fmt, verts, mode, usage)
  if rawtype(verts) == "number" then
    error("love.graphics.newMesh: a 3D mesh must be created from a vertex " ..
          "list, not a vertex count -- its buffer is uploaded once at " ..
          "creation and there is no per-vertex write path to fill it in " ..
          "afterwards.", 3)
  end
  if rawtype(verts) ~= "table" or #verts < 1 then
    error("love.graphics.newMesh: the vertex list is empty", 3)
  end
  -- LOVE defaults to "fan"; a 3D mesh is always a triangle list. Refuse the
  -- others rather than silently reinterpreting the cart's geometry.
  if mode ~= nil and mode ~= "triangles" then
    error("love.graphics.newMesh: a 3D mesh must use the \"triangles\" draw " ..
          "mode (got \"" .. tostring(mode) .. "\"). Fans and strips are 2D " ..
          "conveniences; 3D geometry arrives already triangulated.", 3)
  end
  if usage ~= nil and usage ~= "static" and usage ~= "dynamic" and usage ~= "stream" then
    error("love.graphics.newMesh: unknown usage '" .. tostring(usage) .. "'", 3)
  end
  local id, why = wc.mesh3d_new(verts)
  if not id then
    if why == "nogl" then
      error("love.graphics.newMesh: this run is on the software rasterizer " ..
            "(no GL context, or a draw used a feature the GL backend does " ..
            "not implement), and 3D meshes are GL-only. See the cart log.", 3)
    elseif why == "toobig" then
      error("love.graphics.newMesh: too many vertices (200000 max per mesh)", 3)
    elseif why == "slots" then
      error("love.graphics.newMesh: out of 3D mesh slots (64 max)", 3)
    end
    error("love.graphics.newMesh: the mesh could not be created (" ..
          tostring(why) .. ")", 3)
  end
  return setmetatable({ id = id, n = #verts, fmt = fmt }, Mesh3D)
end

-- ── custom vertex formats, and the 3D path ──────────────────────────
--
-- newMesh(vertexformat, vertices, mode) is LOVE's 3D door, and the only one:
-- there is no love.graphics.setProjection, no camera, no model type. A 3D
-- library declares a format with a 3-component VertexPosition and a
-- VertexNormal, writes a vertex shader that multiplies by its own matrices,
-- and turns the depth test on. g3d, 3DreamEngine and every other LOVE 3D
-- engine are that shape. Supporting this signature IS supporting 3D.
--
-- WHICH FORMATS ARE ACCEPTED. Not arbitrary ones. The engine has exactly two
-- vertex layouts compiled into it, each with its own VAO and shader
-- prologue: the 2D one (x,y,u,v,r,g,b,a) and the 3D one
-- (x,y,z, u,v, nx,ny,nz, r,g,b,a). A declared format is matched against
-- those two by its ATTRIBUTE NAMES AND SIZES, and anything else is refused
-- by name. The alternative -- accepting a format and silently dropping the
-- attributes that do not fit -- produces a model that renders, wrongly, in a
-- way no error message ever explains.
--
-- The 3D format is recognised by VertexPosition having 3 components. That is
-- the one signal that actually distinguishes 2D from 3D in LOVE's API.
local function classify_vertex_format(fmt)
  local pos_n, has_normal, names = nil, false, {}
  for i = 1, #fmt do
    local attr = fmt[i]
    if rawtype(attr) ~= "table" or rawtype(attr[1]) ~= "string" then
      return nil, "attribute " .. i .. " is not a {name, datatype, components} table"
    end
    local name, _, comps = attr[1], attr[2], attr[3]
    names[#names + 1] = name
    if name == "VertexPosition" then pos_n = comps
    elseif name == "VertexNormal" then has_normal = true
    elseif name ~= "VertexTexCoord" and name ~= "VertexColor" then
      -- A custom attribute (VertexTangent, VertexMaterial, InstancePosition
      -- ...). Not an error: it selects the GENERIC path, where the declared
      -- format drives the buffer layout and the shader's attribute bindings.
      return "generic"
    end
  end
  if not pos_n then
    return nil, "the vertex format declares no VertexPosition"
  end
  if pos_n == 3 then return "3d" end
  if pos_n == 2 then
    if has_normal then
      return nil, "a VertexNormal needs a 3-component VertexPosition " ..
                  "(this engine's 3D layout); with a 2-component position " ..
                  "the normal has nowhere to go."
    end
    return "2d"
  end
  -- A 4-component position is what a renderer uses when it packs something
  -- extra into w. The generic path carries it verbatim.
  if pos_n == 4 then return "generic" end
  return nil, "VertexPosition must have 2 components (2D), 3 (3D) or 4, got " ..
              tostring(pos_n)
end

-- newMesh(vertices) | newMesh(vertexcount) | newMesh(..., mode[, usage])
-- newMesh(vertexformat, vertices | vertexcount, mode[, usage])
function graphics.newMesh(a, b, c, d)
  if rawtype(a) == "table" and rawtype(a[1]) == "table" and rawtype(a[1][1]) == "string" then
    local kind, why = classify_vertex_format(a)
    if not kind then
      error("love.graphics.newMesh: " .. why, 2)
    end
    if kind == "3d" then
      return new_mesh_3d(a, b, c, d)
    end
    if kind == "generic" then
      return new_mesh_generic(a, b, c, d)
    end
    -- A 2D custom format that matches the built-in layout: shift the
    -- arguments down and fall through to the default path, which already
    -- IS that layout.
    a, b, c = b, c, d
  end

  local verts, count
  if rawtype(a) == "number" then
    count = math.floor(a)
    if count < 1 then error("love.graphics.newMesh: vertex count must be >= 1", 2) end
  elseif rawtype(a) == "table" then
    verts = a
    count = #verts
    if count < 1 then
      error("love.graphics.newMesh: the vertex list is empty", 2)
    end
  else
    error("love.graphics.newMesh: expected a vertex table or a vertex count, " ..
          "got " .. rawtype(a), 2)
  end

  local modename = b or "fan"
  if modename == "points" then
    error("love.graphics.newMesh: the \"points\" draw mode is not supported " ..
          "by this engine (the renderer draws triangles; there is no point " ..
          "primitive on the batched path). Use love.graphics.points, or a " ..
          "\"triangles\" mesh of small quads.", 2)
  end
  local mode = MESH_MODES[modename]
  if not mode then
    error("love.graphics.newMesh: unknown draw mode '" .. tostring(modename) ..
          "'. Supported: \"fan\" (the default), \"strip\", \"triangles\".", 2)
  end
  -- `usage` (c) is "static"/"dynamic"/"stream", a GPU buffer hint. Every mesh
  -- here uploads its vertices on the draw that uses them, so the hint has
  -- nothing to select; accepted and ignored rather than erroring, because
  -- ported code passes it constantly.
  if c ~= nil and c ~= "static" and c ~= "dynamic" and c ~= "stream" then
    error("love.graphics.newMesh: unknown usage '" .. tostring(c) ..
          "'. Supported: \"static\", \"dynamic\", \"stream\".", 2)
  end

  local id, why = wc.mesh_new(count, mode)
  if not id then
    if why == "nogl" then
      error("love.graphics.newMesh: this run is on the software rasterizer " ..
            "(no GL context, or a draw used a feature the GL backend does " ..
            "not implement), and meshes are GL-only -- there is no software " ..
            "path that rasterizes a textured, per-vertex-coloured triangle. " ..
            "See the cart log.", 2)
    end
    error("love.graphics.newMesh: out of mesh slots (32 max), or the vertex " ..
          "count exceeds this engine's 4096-per-mesh limit", 2)
  end
  local m = setmetatable({ id = id, n = count, mode = modename }, Mesh)
  if verts then m:setVertices(verts) end
  return m
end

-- ── depth and face culling ──────────────────────────────────────────
--
-- The other half of 3D. A cart that builds a perspective projection but
-- never turns the depth test on gets painter's-order rendering: far geometry
-- drawn after near geometry covers it, so a model looks inside-out and the
-- bug reads as a broken projection matrix.
--
-- These are LOVE's exact signatures. g3d's whole depth setup is the single
-- line `love.graphics.setDepthMode("lequal", true)`.

-- GL compare-function enums. Kept here rather than in C so the name->enum
-- mapping is visible next to the error that lists the valid names.
local DEPTH_COMPARE = {
  never = 0x0200, less = 0x0201, equal = 0x0202, lequal = 0x0203,
  greater = 0x0204, notequal = 0x0205, gequal = 0x0206, always = 0x0207,
}
-- LOVE spells two of these with an extra word; accept both spellings.
DEPTH_COMPARE.lessequal = DEPTH_COMPARE.lequal
DEPTH_COMPARE.greaterequal = DEPTH_COMPARE.gequal

local depth_mode_name, depth_write_flag = "always", false

-- setDepthMode() with no arguments is LOVE's reset to "no depth testing",
-- which is this engine's 2D resting state.
function graphics.setDepthMode(comparemode, write)
  if comparemode == nil then
    depth_mode_name, depth_write_flag = "always", false
    wc.depth_mode(0, false)
    return
  end
  local e = DEPTH_COMPARE[comparemode]
  if not e then
    error("love.graphics.setDepthMode: unknown compare mode '" ..
          tostring(comparemode) .. "'. Valid: never, less, equal, lequal, " ..
          "greater, notequal, gequal, always.", 2)
  end
  -- "always" with no write is exactly "depth off", and passing it as a live
  -- compare would cost a depth test per fragment for nothing.
  if comparemode == "always" and not write then
    depth_mode_name, depth_write_flag = "always", false
    wc.depth_mode(0, false)
    return
  end
  depth_mode_name, depth_write_flag = comparemode, not not write
  wc.depth_mode(e, write and true or false)
end

function graphics.getDepthMode()
  return depth_mode_name, depth_write_flag
end

-- drawInstanced(mesh, count) - the same geometry `count` times in ONE draw
-- call, with gl_InstanceID varying so the vertex shader can place each copy.
-- This is the difference between 1000 draw calls and 1. Per-instance data
-- goes in a uniform array indexed by gl_InstanceID; there is no per-instance
-- attribute buffer, matching what LOVE's own drawInstanced offers.
function graphics.drawInstanced(mesh, count, ...)
  if getmetatable(mesh) ~= Mesh3D then
    error("love.graphics.drawInstanced: expected a 3D Mesh (one created with " ..
          "a 3-component VertexPosition vertex format)", 2)
  end
  count = math.floor(count or 1)
  if count < 1 then return end
  if not wc.mesh3d_draw_instanced(mesh.id, count) then
    error("love.graphics.drawInstanced: the mesh could not be drawn. Either " ..
          "this run is on the software rasterizer (3D is GL-only), or no " ..
          "shader is bound -- an instanced 3D draw REQUIRES a vertex stage, " ..
          "which is where gl_InstanceID is read.", 2)
  end
end

-- love.graphics.reset() - back to the default state, all of it. Renderers
-- call this between passes to guarantee they are not inheriting whatever the
-- previous pass left set, which is exactly the bug it prevents: a leftover
-- colour mask or depth mode makes the NEXT pass render wrongly, far from the
-- code that set it.
function graphics.reset()
  graphics.setColor(1, 1, 1, 1)
  graphics.setBackgroundColor(0, 0, 0, 1)
  graphics.setShader()
  graphics.setCanvas()
  graphics.setBlendMode("alpha")
  graphics.setColorMask()
  graphics.setDepthMode()
  graphics.setMeshCullMode("none")
  graphics.setFrontFaceWinding("ccw")
  graphics.setScissor()
  graphics.setLineWidth(1)
  graphics.origin()
end

-- validateShader(gles, pixel, vertex) -> ok, err
-- LOVE's dry run: compile without keeping the program. 3DreamEngine uses it
-- to pick a shader variant the driver will actually accept.
function graphics.validateShader(gles, pixel, vertex)
  local ok, err = pcall(graphics.newShader, pixel, vertex)
  if ok then
    if err and err.release then err:release() end
    return true, nil
  end
  return false, tostring(err)
end

-- isActive: is there a graphics device to draw on at all.
function graphics.isActive() return true end
-- present(): the host owns the swap; a cart's frame ends when wc_render
-- returns. Accepted as a no-op so a renderer's main loop runs unchanged.
function graphics.present() end

-- arc(drawmode, [arctype], x, y, radius, angle1, angle2, [segments])
-- Built from the polygon path rather than a new primitive: an arc IS a fan
-- of points on a circle, and reusing polygon() keeps it consistent with the
-- engine's existing fill/line rules.
function graphics.arc(mode, a, b, c, d, e, f, g)
  local arctype, x, y, r, a1, a2, segs
  if rawtype(a) == "string" then
    arctype, x, y, r, a1, a2, segs = a, b, c, d, e, f, g
  else
    arctype, x, y, r, a1, a2, segs = "pie", a, b, c, d, e, f
  end
  if not (x and y and r and a1 and a2) then
    error("love.graphics.arc: expected (mode, [arctype], x, y, radius, " ..
          "angle1, angle2)", 2)
  end
  segs = math.max(3, math.floor(segs or math.max(8, r / 2)))
  -- Reused across calls for the same reason polygon()'s buffer is: these
  -- points are consumed by polygon() before this returns and can never be
  -- referenced afterwards. This is a SEPARATE buffer from poly_scratch --
  -- polygon() is about to read this one while writing that one.
  local pts = arc_scratch
  local n = 0
  -- "pie" closes through the centre; "open"/"closed" trace only the rim.
  if arctype == "pie" then pts[1] = x; pts[2] = y; n = 2 end
  for i = 0, segs do
    local t = a1 + (a2 - a1) * (i / segs)
    pts[n + 1] = x + math.cos(t) * r
    pts[n + 2] = y + math.sin(t) * r
    n = n + 2
  end
  for i = #pts, n + 1, -1 do pts[i] = nil end
  graphics.polygon(mode, pts)
end

-- captureScreenshot: LOVE hands the image to a callback or a thread channel.
-- A cart cannot write files and has no threads, so this is refused by name
-- rather than silently doing nothing -- a screenshot that never arrives and
-- never errors is the worst of both.
graphics.captureScreenshot = err("love.graphics.captureScreenshot",
  "a cart cannot write files. The HOST owns screenshots (romdev's frame " ..
  "capture, or the player's own key binding)")

function graphics.getStats()
  return {
    drawcalls = 0, canvasswitches = 0, texturememory = 0, images = 0,
    canvases = 0, fonts = 0, shaderswitches = 0, drawcallsbatched = 0,
  }
end

-- setColorMask() with no arguments enables every channel, which is LOVE's
-- reset. A renderer uses this to write depth without touching colour.
function graphics.setColorMask(r, g, b, a)
  if r == nil then r, g, b, a = true, true, true, true end
  wc.color_mask(r and true or false, g and true or false,
                b and true or false, a and true or false)
end
function graphics.getColorMask() return wc.get_color_mask() end

local CULL_MODES = { none = 0, back = 1, front = 2 }
local cull_mode_name = "none"

function graphics.setMeshCullMode(mode)
  local m = CULL_MODES[mode or "none"]
  if not m then
    error("love.graphics.setMeshCullMode: unknown cull mode '" ..
          tostring(mode) .. "'. Valid: none, back, front.", 2)
  end
  cull_mode_name = mode or "none"
  wc.cull_mode(m)
end
function graphics.getMeshCullMode() return cull_mode_name end

-- LOVE's default is counter-clockwise, matching GL's.
local front_face_name = "ccw"
function graphics.setFrontFaceWinding(winding)
  local w = winding or "ccw"
  if w ~= "ccw" and w ~= "cw" then
    error("love.graphics.setFrontFaceWinding: expected \"ccw\" or \"cw\", " ..
          "got '" .. tostring(winding) .. "'", 2)
  end
  front_face_name = w
  wc.front_face(w == "cw")
end
function graphics.getFrontFaceWinding() return front_face_name end

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

-- Scancodes. There is no physical keyboard here -- keys are pad buttons
-- wearing key names -- so a scancode and a key are the same thing and the
-- two conversions are identity. Reported rather than omitted because
-- LOVE games routinely call isScancodeDown for layout independence, and a
-- missing function there means the game simply never sees input.
function keyboard.isScancodeDown(...) return keyboard.isDown(...) end
function keyboard.getScancodeFromKey(key) return key end
function keyboard.getKeyFromScancode(sc) return sc end

-- Key repeat and text input are host concerns a cart cannot influence.
-- Store what was asked so the getter round-trips, and report the honest
-- answer for the capability queries.
local key_repeat, text_input = false, false
function keyboard.setKeyRepeat(enable) key_repeat = enable and true or false end
function keyboard.hasKeyRepeat() return key_repeat end
function keyboard.setTextInput(enable) text_input = enable and true or false end
function keyboard.hasTextInput() return text_input end

-- No on-screen keyboard: a cart cannot summon one, and claiming otherwise
-- would have a game wait forever for text that never arrives.
function keyboard.hasScreenKeyboard() return false end

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
  if rawtype(n) == "string" then n, b = 1, n end
  return has(pads[n].buttons, b)
end
function pad.wasPressed(n, b)
  if rawtype(n) == "string" then n, b = 1, n end
  local p = pads[n]
  return has(p.buttons, b) and not has(p.prev, b)
end
function pad.wasReleased(n, b)
  if rawtype(n) == "string" then n, b = 1, n end
  local p = pads[n]
  return not has(p.buttons, b) and has(p.prev, b)
end
function pad.axis(n, which)
  if rawtype(n) == "string" then n, which = 1, n end
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
  if rawtype(pn) ~= "number" or pn < 1 or pn > 4 then return false end
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

-- ── love.net ───────────────────────────────────────────────────────
--
-- LOVE has no networking, so this is not a LOVE API being mirrored: it is
-- the wasmcart peer ABI given a LOVE-SHAPED surface. Polling functions on
-- love.net, callbacks assigned as love.net.<event>, same as love.update.
--
-- There is ONE primitive: a connection to a peer. What it runs over is the
-- host's business and deliberately invisible here - a WebSocket, a data
-- channel, a LAN socket and a serial cable all arrive as the same peer.
-- There is no client/server split either; which end dialed is a host-side
-- fact. A cart that wants to be a server just behaves like one.
--
-- Payloads are BYTES. Lua strings carry arbitrary bytes including NUL, so
-- they are what send/broadcast take and what the message callback hands
-- back. Text framing, JSON, whatever structure the game wants on top is the
-- cart's job: the ABI moves bytes and nothing else.
--
-- Reaching the network needs BOTH halves of the gate: the engine sets
-- WC_FLAG_NET_PEER for you, and whoever packs the cart must grant the
-- domains in the manifest's `net` object. Neither half can be asserted from
-- Lua, which is the point - a cart cannot grant itself network reach.
local net = {}
love.net = net

local STATE_NAMES = { [0] = "connecting", [1] = "open", [2] = "closing", [3] = "closed" }

-- Transport PROPERTIES, not a transport name. There is deliberately no way
-- to ask "am I on WebRTC": branching on the implementation is exactly what
-- this design hides. A host that does not characterize its transport
-- reports none of these, and a cart must cope with that.
local TRANSPORT = { reliable = 0x01, ordered = 0x02, lowlatency = 0x04 }

-- open(address) -> peer id, or nil when the host refuses.
--
-- The address grammar belongs to the HOST, not to this engine and not to
-- the spec. "wss://example.com/lobby", "room:ABCD", "192.168.1.7:9000" are
-- all plausible; a host that does not understand one fails the open. Treat
-- nil as normal and recoverable, never as an error worth crashing on: an
-- offline device is a supported configuration.
function net.open(address)
  if rawtype(address) ~= "string" then return nil end
  return wc.peer_open(address)
end

function net.close(peer)
  if rawtype(peer) ~= "number" then return end
  wc.peer_close(peer)
end

-- send(peer, data) -> bytes sent, or nil if the host refused.
-- Returning nil rather than 0 keeps "refused" distinguishable from "sent an
-- empty message", which is a legal thing for a cart to do.
function net.send(peer, data)
  if rawtype(peer) ~= "number" or rawtype(data) ~= "string" then return nil end
  local n = wc.peer_send(peer, data)
  if n < 0 then return nil end
  return n
end

-- broadcast(data) -> number of peers it reached.
function net.broadcast(data)
  if rawtype(data) ~= "string" then return 0 end
  local n = wc.peer_broadcast(data)
  if n < 0 then return 0 end
  return n
end

-- state(peer) -> "connecting" | "open" | "closing" | "closed".
-- An unknown peer reads as "closed", which is the truth from the cart's
-- point of view and saves every caller a nil check.
function net.state(peer)
  if rawtype(peer) ~= "number" then return "closed" end
  return STATE_NAMES[wc.peer_state(peer)] or "closed"
end

function net.isOpen(peer)
  return net.state(peer) == "open"
end

-- peers() -> array of peer ids, in host order.
function net.peers()
  local out = {}
  for i = 0, wc.peer_count() - 1 do
    local id = wc.peer_id(i)
    if id then out[#out + 1] = id end
  end
  return out
end

function net.count()
  return wc.peer_count()
end

-- name(peer) -> display string, or nil.
--
-- DISPLAY-ONLY, and the source of a real bug class. The name comes from a
-- remote machine, so it is attacker-controlled text: it is not unique, not
-- stable across sessions, not necessarily valid UTF-8, and not a handle.
-- Draw it, and nothing else. The PEER ID is the handle - key player tables
-- on that. The engine already bounds the length the host may write; what it
-- cannot do is stop a cart from trusting the contents.
function net.name(peer)
  if rawtype(peer) ~= "number" then return nil end
  return wc.peer_name(peer)
end

-- transport(peer) -> table of properties. All false is the normal answer
-- from a host that does not characterize its transport, so a cart must not
-- read "not reliable" as "unreliable" - it means "unknown, assume nothing".
function net.transport(peer)
  if rawtype(peer) ~= "number" then return { reliable = false, ordered = false, lowLatency = false } end
  local bits = wc.peer_transport(peer)
  return {
    reliable   = (bits & TRANSPORT.reliable) ~= 0,
    ordered    = (bits & TRANSPORT.ordered) ~= 0,
    lowLatency = (bits & TRANSPORT.lowlatency) ~= 0,
  }
end

-- Drain the engine's event queue into the cart's callbacks. Called at the
-- top of the frame, before love.update, so a handler runs with the same
-- world state the rest of the frame sees rather than halfway through it.
local function dispatch_net()
  local dropped = wc.peer_dropped()
  if dropped > 0 and net.overflow then net.overflow(dropped) end
  while true do
    local kind, peer, payload = wc.peer_poll()
    if not kind then break end
    if kind == 0 then
      if net.connected then net.connected(peer, payload) end
    elseif kind == 1 then
      if net.message then net.message(peer, payload) end
    elseif kind == 2 then
      if net.disconnected then net.disconnected(peer) end
    else
      if net.error then net.error(peer) end
    end
  end
end

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

local relative_mode = false
-- setRelativeMode: LOVE captures the cursor so an FPS camera gets unbounded
-- deltas. A cart cannot capture anything -- the host owns the pointer -- so
-- this records the flag and reports it back rather than lying either way.
-- Camera code that checks isRelativeMode() before using deltas still works;
-- code that assumes capture succeeded gets the pointer's real position.
function mouse.setRelativeMode(on) relative_mode = not not on end
function mouse.isRelativeMode() return relative_mode end

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

-- ── love.touch ─────────────────────────────────────────────────────
--
-- The pointer ABI already carries TEN slots -- slot 0 is the mouse, 1-9 are
-- fingers -- and love.mouse only ever reads slot 0. So multi-touch was
-- always there; what was missing was the standard API for reaching it, and
-- without that a LOVE game written for a phone has no way to ask.
--
-- Touch IDs are the slot numbers. LOVE only promises they are opaque and
-- stable while a finger is down, which slots are.
local touch = {}
love.touch = touch

function touch.getTouches()
  local out = {}
  for slot = 1, 9 do
    local _, _, buttons, active = wc.pointer(slot)
    if active and buttons ~= 0 then out[#out + 1] = slot end
  end
  return out
end

function touch.getPosition(id)
  local x, y, buttons, active = wc.pointer(id)
  if not (active and buttons ~= 0) then
    error("love.touch.getPosition: no touch with id " .. tostring(id), 2)
  end
  return x, y
end

-- No pressure sensor in the ABI. LOVE returns 1 for a plain touch, which is
-- what a device without pressure reports anyway.
function touch.getPressure(id)
  local _, _, buttons, active = wc.pointer(id)
  if not (active and buttons ~= 0) then
    error("love.touch.getPressure: no touch with id " .. tostring(id), 2)
  end
  return 1
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

-- Transform objects. Deliberately a TRS+shear record rather than a 3x3
-- matrix: it is what graphics.applyTransform consumes, and the engine's own
-- transform state has the same shape, so the two compose without a matrix
-- decomposition step that could not always succeed anyway.
local Transform = {}
Transform.__index = Transform

function lmath.newTransform(x, y, rot, sx, sy)
  return setmetatable({ tx = x or 0, ty = y or 0, rot = rot or 0,
                        sx = sx or 1, sy = sy or sx or 1,
                        kx = 0, ky = 0 }, Transform)
end

function Transform:translate(x, y) self.tx = self.tx + x; self.ty = self.ty + y; return self end
function Transform:rotate(r) self.rot = self.rot + r; return self end
function Transform:scale(sx, sy) self.sx = self.sx * sx; self.sy = self.sy * (sy or sx); return self end
function Transform:shear(kx, ky) self.kx = self.kx + kx; self.ky = self.ky + ky; return self end
function Transform:reset()
  self.tx, self.ty, self.rot, self.sx, self.sy, self.kx, self.ky = 0, 0, 0, 1, 1, 0, 0
  return self
end
function Transform:clone()
  local t = lmath.newTransform(self.tx, self.ty, self.rot, self.sx, self.sy)
  t.kx, t.ky = self.kx, self.ky
  return t
end
function Transform:transformPoint(x, y)
  if self.kx ~= 0 or self.ky ~= 0 then x, y = x + y * self.kx, y + x * self.ky end
  x, y = x * self.sx, y * self.sy
  if self.rot ~= 0 then
    local c, sn = math.cos(self.rot), math.sin(self.rot)
    x, y = x * c - y * sn, x * sn + y * c
  end
  return x + self.tx, y + self.ty
end
function Transform:inverseTransformPoint(x, y)
  x, y = x - self.tx, y - self.ty
  if self.rot ~= 0 then
    local c, sn = math.cos(-self.rot), math.sin(-self.rot)
    x, y = x * c - y * sn, x * sn + y * c
  end
  if self.sx ~= 0 then x = x / self.sx end
  if self.sy ~= 0 then y = y / self.sy end
  return x, y
end

-- Bezier curves. Pure maths, no host involvement, and games use them for
-- paths and easing curves.
local Bezier = {}
Bezier.__index = Bezier

function lmath.newBezierCurve(...)
  local a = ...
  local pts = (type(a) == "table") and a or { ... }
  return setmetatable({ pts = pts }, Bezier)
end

function Bezier:getControlPointCount() return #self.pts / 2 end
function Bezier:getDegree() return #self.pts / 2 - 1 end

-- de Casteljau: numerically stable, and the standard way to evaluate.
function Bezier:evaluate(t)
  local n = #self.pts / 2
  if n < 2 then error("BezierCurve:evaluate: need at least 2 control points", 2) end
  local x, y = {}, {}
  for i = 1, n do x[i], y[i] = self.pts[i*2-1], self.pts[i*2] end
  for k = 1, n - 1 do
    for i = 1, n - k do
      x[i] = x[i] * (1 - t) + x[i+1] * t
      y[i] = y[i] * (1 - t) + y[i+1] * t
    end
  end
  return x[1], y[1]
end

function Bezier:render(depth)
  depth = depth or 5
  local steps = 2 ^ depth
  local out = {}
  for i = 0, steps do
    local px, py = self:evaluate(i / steps)
    out[#out + 1] = px
    out[#out + 1] = py
  end
  return out
end

-- Colour helpers. Pure arithmetic, and the reason they matter is that
-- LOVE 11 changed colours from 0-255 to 0-1: every port of an older game
-- reaches for these, and without them the game draws in the wrong colours
-- rather than erroring.
function lmath.colorFromBytes(r, g, b, a)
  if a ~= nil then a = a / 255 end
  return r / 255, g / 255, b / 255, a
end

function lmath.colorToBytes(r, g, b, a)
  local function q(v) return math.floor(math.min(1, math.max(0, v)) * 255 + 0.5) end
  if a ~= nil then a = q(a) end
  return q(r), q(g), q(b), a
end

-- sRGB <-> linear. The standard piecewise transfer function, not the 2.2
-- approximation: a shader doing correct lighting needs the real curve.
local function _g2l(c)
  if c <= 0.04045 then return c / 12.92 end
  return ((c + 0.055) / 1.055) ^ 2.4
end
local function _l2g(c)
  if c <= 0.0031308 then return c * 12.92 end
  return 1.055 * (c ^ (1 / 2.4)) - 0.055
end
function lmath.gammaToLinear(r, g, b, a)
  if g == nil then return _g2l(r) end
  return _g2l(r), _g2l(g), _g2l(b), a
end
function lmath.linearToGamma(r, g, b, a)
  if g == nil then return _l2g(r) end
  return _l2g(r), _l2g(g), _l2g(b), a
end

-- Normally-distributed random, via Box-Muller on the host RNG so it stays
-- deterministic with everything else.
function lmath.randomNormal(stddev, mean)
  stddev = stddev or 1; mean = mean or 0
  local u1 = math.max(1e-12, wc.rand())
  local u2 = wc.rand()
  return math.sqrt(-2 * math.log(u1)) * math.cos(2 * math.pi * u2) * stddev + mean
end

-- Is this polygon convex? Sign of the cross product must not change as the
-- winding walks the vertices.
function lmath.isConvex(...)
  local a = ...
  local v = (type(a) == "table") and a or { ... }
  local n = #v / 2
  if n < 3 then return false end
  local sign = 0
  for i = 0, n - 1 do
    local x1, y1 = v[i * 2 + 1], v[i * 2 + 2]
    local x2, y2 = v[((i + 1) % n) * 2 + 1], v[((i + 1) % n) * 2 + 2]
    local x3, y3 = v[((i + 2) % n) * 2 + 1], v[((i + 2) % n) * 2 + 2]
    local cross = (x2 - x1) * (y3 - y2) - (y2 - y1) * (x3 - x2)
    if cross ~= 0 then
      local sgn = cross > 0 and 1 or -1
      if sign == 0 then sign = sgn elseif sgn ~= sign then return false end
    end
  end
  return true
end

-- Ear-clipping triangulation. love.graphics.polygon only fans convex
-- shapes, so a cart with a concave polygon needs this to draw it at all.
function lmath.triangulate(...)
  local a = ...
  local v = (type(a) == "table") and a or { ... }
  local n = #v / 2
  if n < 3 then error("love.math.triangulate: need at least 3 vertices", 2) end
  local idx = {}
  for i = 1, n do idx[i] = i end
  -- ensure counter-clockwise winding
  local area = 0
  for i = 1, n do
    local j = i % n + 1
    area = area + (v[i*2-1] * v[j*2] - v[j*2-1] * v[i*2])
  end
  if area < 0 then
    local r = {}
    for i = n, 1, -1 do r[#r + 1] = idx[i] end
    idx = r
  end
  local function pt(k) return v[idx[k]*2-1], v[idx[k]*2] end
  local tris, guard = {}, 0
  while #idx > 3 and guard < 10000 do
    guard = guard + 1
    local clipped = false
    for i = 1, #idx do
      local i0 = (i - 2) % #idx + 1
      local i1 = i
      local i2 = i % #idx + 1
      local ax, ay = pt(i0); local bx, by = pt(i1); local cx, cy = pt(i2)
      local cross = (bx - ax) * (cy - by) - (by - ay) * (cx - bx)
      if cross > 0 then                        -- convex corner
        local ok = true
        for k = 1, #idx do
          if k ~= i0 and k ~= i1 and k ~= i2 then
            local px, py = pt(k)
            -- point-in-triangle by barycentric sign
            local d1 = (px-bx)*(ay-by) - (ax-bx)*(py-by)
            local d2 = (px-cx)*(by-cy) - (bx-cx)*(py-cy)
            local d3 = (px-ax)*(cy-ay) - (cx-ax)*(py-ay)
            local neg = (d1 < 0) or (d2 < 0) or (d3 < 0)
            local pos = (d1 > 0) or (d2 > 0) or (d3 > 0)
            if not (neg and pos) then ok = false; break end
          end
        end
        if ok then
          tris[#tris + 1] = { ax, ay, bx, by, cx, cy }
          table.remove(idx, i1)
          clipped = true
          break
        end
      end
    end
    if not clipped then break end             -- degenerate; stop rather than spin
  end
  if #idx == 3 then
    local ax, ay = pt(1); local bx, by = pt(2); local cx, cy = pt(3)
    tris[#tris + 1] = { ax, ay, bx, by, cx, cy }
  end
  return tris
end

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

local asset_index = nil        -- { [dir] = { name, ... } }

local function build_asset_index()
  if asset_index then return asset_index end
  asset_index = {}
  local raw = wc.asset_read("assets.index")
  if not raw then return asset_index end
  for line in raw:gmatch("([^\n]+)") do
    local path = line:gsub("%s+$", "")
    if #path > 0 then
      -- Every ancestor directory gets an entry, so listing an intermediate
      -- directory finds its subdirectories as well as its files -- which is
      -- what a recursive walk (utils.lua does one) expects.
      local dir, name = path:match("^(.*)/([^/]+)$")
      if not dir then dir, name = "", path end
      local seen = {}
      local t = asset_index[dir]
      if not t then t = {}; asset_index[dir] = t end
      for _, v in ipairs(t) do seen[v] = true end
      if not seen[name] then t[#t + 1] = name end
      -- register this directory inside its own parent
      while dir ~= "" do
        local parent, dname = dir:match("^(.*)/([^/]+)$")
        if not parent then parent, dname = "", dir end
        local pt = asset_index[parent]
        if not pt then pt = {}; asset_index[parent] = pt end
        local found = false
        for _, v in ipairs(pt) do if v == dname then found = true break end end
        if not found then pt[#pt + 1] = dname end
        dir = parent
      end
    end
  end
  return asset_index
end

function fs.getInfo(path, arg)
  -- LOVE's second argument is either a filtertype string or a table to fill.
  local want = rawtype(arg) == "string" and arg or nil
  local out = rawtype(arg) == "table" and arg or {}
  if wc.asset_exists(path) then
    if want and want ~= "file" then return nil end
    out.type = "file"
    out.size = #(wc.asset_read(path) or "")
    return out
  end
  -- A DIRECTORY has no asset of its own, so it is only knowable from the
  -- index. Libraries check getInfo(...).type before recursing into a path,
  -- and answering nil for a real directory stops that walk dead.
  local idx = build_asset_index()
  local key = tostring(path or ""):gsub("^%./", ""):gsub("/+$", "")
  if idx[key] then
    if want and want ~= "directory" then return nil end
    out.type = "directory"
    return out
  end
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

-- A cart has an asset BUNDLE, not a filesystem. The wasmcart ABI can look an
-- asset up by path (wc_asset_size / wc_load_asset) but cannot enumerate one,
-- so directory listing is not a thing this engine is hiding -- it does not
-- exist at the layer below.
--
-- Returning {} would be worse than erroring: a library that lists a
-- directory to discover its own resources would find nothing and conclude
-- the resources are missing, which sends the author looking in the wrong
-- place entirely.
-- Directory listing, from an INDEX the cart carries.
--
-- The wasmcart ABI looks an asset up by path and cannot enumerate one, so
-- there is nothing under this to walk. But a cart knows its own contents at
-- pack time, and a great many real libraries discover their modules by
-- listing a directory -- 3DreamEngine finds its classes, shaders, jobs and
-- loaders that way, in ten places, and simply cannot load without it.
--
-- So: if the cart ships `assets.index` (one path per line, which
-- tools/gen-asset-index.sh writes), this answers from it. Without the index
-- the honest answer is still an error, because returning {} would tell a
-- library its resources are missing and send the author hunting in the
-- wrong place.
function fs.getDirectoryItems(dir)
  local idx = build_asset_index()
  local key = tostring(dir or ""):gsub("^%./", ""):gsub("/+$", "")
  local items = idx[key]
  if items then
    -- A copy: a caller that sorts or mutates the result must not corrupt
    -- the index for the next call.
    local out = {}
    for i, v in ipairs(items) do out[i] = v end
    table.sort(out)
    return out
  end
  if next(idx) == nil then
    error("love.filesystem.getDirectoryItems('" .. tostring(dir) .. "'): a " ..
          "cart carries an asset bundle, not a filesystem, and the wasmcart " ..
          "ABI can only look an asset up BY PATH. Ship an `assets.index` " ..
          "file (one asset path per line -- see tools/gen-asset-index.sh) " ..
          "and this returns its contents.", 2)
  end
  -- The index exists and this directory is not in it: genuinely empty.
  return {}
end

-- Writing: a cart gets ONE save blob, not a directory tree. These exist so a
-- library that calls them fails with the reason rather than a nil index.
function fs.createDirectory()
  error("love.filesystem.createDirectory: a cart has no writable filesystem. " ..
        "love.filesystem.write(name, data) persists a single save blob " ..
        "through the host.", 2)
end
function fs.remove()
  error("love.filesystem.remove: a cart has no writable filesystem; there is " ..
        "one save blob, which love.filesystem.write replaces.", 2)
end
function fs.getSaveDirectory() return "save:" end
function fs.getRealDirectory() return "cart:" end
function fs.getIdentity() return "wasmcart" end
function fs.setIdentity() end
function fs.newFile(path)
  -- A read-only File object over a cart asset. Enough for the common
  -- open/read/close idiom; writing goes through the save blob instead.
  local content, pos = nil, 1
  local file = {}
  function file:open(mode)
    if mode == "w" or mode == "a" then
      return false, "a cart asset is read-only; use love.filesystem.write " ..
                    "for save data"
    end
    content = wc.asset_read(path)
    pos = 1
    return content ~= nil, content and nil or ("could not open " .. tostring(path))
  end
  function file:read(n)
    if not content then return nil, "file is not open" end
    local chunk = n and content:sub(pos, pos + n - 1) or content:sub(pos)
    pos = pos + #chunk
    return chunk, #chunk
  end
  function file:lines()
    if not content then return function() return nil end end
    return content:gmatch("([^\n]*)\n?")
  end
  function file:close() content = nil return true end
  function file:getSize() return content and #content or (wc.asset_read(path) or ""):len() end
  function file:isOpen() return content ~= nil end
  function file:eof() return not content or pos > #content end
  function file:type() return "File" end
  return file
end

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
  -- A cart is always "fullscreen" in the only sense it can observe: it fills
  -- whatever surface the host gave it and cannot resize itself.
  getFullscreen = function() return true, "desktop" end,
  updateMode = function() return true end,
  isOpen = function() return true end,
  hasFocus = function() return true end,
  hasMouseFocus = function() return true end,
  isVisible = function() return true end,
  getDPIScale = function() return 1 end,
  toPixels = function(v) return v end,
  fromPixels = function(v) return v end,
  requestAttention = function() end,
  maximize = function() end,
  minimize = function() end,
  restore = function() end,
  focus = function() end,
}

love.event = {
  quit = function()
    wc.log("love.event.quit(): cartridge hosts do not exit; ignoring")
  end,
  -- The HOST runs the event loop and calls wc_render once per frame; a cart
  -- never pumps its own. These exist so a library that drives its own loop
  -- (3DreamEngine's job system does) runs without special-casing.
  pump = function() end,
  poll = function() return function() return nil end end,
  push = function() end,
  clear = function() end,
}

-- love.font.newRasterizer: in LOVE this exposes glyph rasterization so a
-- caller can build its own font atlas. This engine bakes TTF glyphs in C
-- (stb_truetype) and hands back a Font, so the rasterizer is not a thing a
-- cart can hold. Return the Font instead, which is what callers do with the
-- rasterizer anyway (newFont(rasterizer)).
love.font = {
  newRasterizer = function(path, size)
    return graphics.newFont(path, size)
  end,
}

love.system = {
  getOS = function() return "wasmcart" end,
  getPowerInfo = function() return "unknown" end,
  -- ONE. A cart is a single wasm instance with no threads, and libraries
  -- size their worker pools from this -- answering the host's real core
  -- count would have them spawn workers that can never exist.
  getProcessorCount = function() return 1 end,
  getClipboardText = function() return "" end,
  setClipboardText = function() end,
  vibrate = function() end,
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
  -- A cart's textures are PNGs decoded by the engine's own stb_image, so
  -- there is no compressed-texture container to inspect. Answering "no"
  -- honestly is what sends a loader down its uncompressed path.
  isCompressed = function() return false end,
  newCompressedData = function()
    error("love.image.newCompressedData: compressed texture containers (DDS/" ..
          "KTX/PVR) are not supported; ship PNGs, which the engine decodes.", 2)
  end,
  -- newImageData(path) | newImageData(width, height) | (w, h, format, data)
  --
  -- Both forms are real: the path form decodes an asset, and the size form
  -- allocates zeroed CPU pixels a cart can write with setPixel. The size
  -- form is not a nicety -- it is how a library builds a placeholder texture
  -- (3DreamEngine's sky fallback is six 2x2 ImageDatas), and returning nil
  -- for it makes a cubemap arrive with zero faces.
  newImageData = function(a, b, format, data)
    local img
    if rawtype(a) == "number" then
      local w, h = math.max(1, math.floor(a)), math.max(1, math.floor(b or a))
      local id = wc.image_blank(w, h)
      if not id then return nil end
      img = setmetatable({ id = id, w = w, h = h }, Image)
      -- LOVE's 4th argument is initial pixel bytes; honour it when the
      -- string is the right size, since a caller passing data expects it.
      if rawtype(data) == "string" and #data >= w * h * 4 then
        local i = 1
        for y = 0, h - 1 do
          for x = 0, w - 1 do
            wc.image_pixel(id, x, y,
                           data:byte(i) / 255, data:byte(i + 1) / 255,
                           data:byte(i + 2) / 255, data:byte(i + 3) / 255)
            i = i + 4
          end
        end
      end
    else
      local ok, loaded_img = pcall(graphics.newImage, a)
      if not ok then return nil end
      img = loaded_img
    end
    return {
      _img = img,
      id = img.id,           -- so newCubeImage can consume it directly
      w = img.w, h = img.h,
      getWidth = function(self) return self._img.w end,
      getHeight = function(self) return self._img.h end,
      getDimensions = function(self) return self._img.w, self._img.h end,
      getPixel = function(self, x, y) return wc.image_pixel(self._img.id, x, y) end,
      setPixel = function(self, x, y, r, g, b, a)
        wc.image_pixel(self._img.id, x, y, r, g, b, a == nil and 1 or a)
      end,
      -- mapPixel(fn): LOVE's per-pixel transform. Real games use it to build
      -- gradients and masks at load time.
      mapPixel = function(self, fn)
        for y = 0, self._img.h - 1 do
          for x = 0, self._img.w - 1 do
            local r, g, bb, aa = wc.image_pixel(self._img.id, x, y)
            local nr, ng, nb, na = fn(x, y, r, g, bb, aa)
            wc.image_pixel(self._img.id, x, y, nr or r, ng or g, nb or bb,
                           na == nil and aa or na)
          end
        end
      end,
      release = function() return true end,
      type = function() return "ImageData" end,
      typeOf = function(_, t) return t == "ImageData" or t == "Data" end,
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
    if rawtype(path) ~= "string" then
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
  compress = err("love.data.compress",
    "compressed data is not supported; export maps/assets uncompressed"),
}

-- love.data.newByteData / pack / unpack / hash.
--
-- LOVE's ByteData is a handle to raw bytes with an FFI pointer behind it.
-- There is no FFI here (that is LuaJIT; this is PUC Lua 5.4), so a ByteData
-- is backed by a Lua string, and getFFIPointer is refused BY NAME rather
-- than returning something that would be dereferenced.
--
-- This matters for a real reason: g3d's model:compress() and 3DreamEngine's
-- mesh packing both take an ffi path when it exists and a plain-table path
-- when it does not. Refusing getFFIPointer is what keeps them on the path
-- that works.
-- Backed by a table of BYTES rather than a string, because a ByteData is
-- written through: ffi.cast gives out a typed view over this same table, and
-- a string would make every field write an allocation and a copy. getString
-- materializes on demand for the callers that want bytes out.
local ByteData = {}
ByteData.__index = ByteData
function ByteData:type() return "ByteData" end
function ByteData:typeOf(t) return t == "ByteData" or t == "Data" or t == "Object" end
function ByteData:getSize() return #self._bytes end
function ByteData:getString()
  local out = {}
  for i = 1, #self._bytes do out[i] = string.char(self._bytes[i] or 0) end
  return table.concat(out)
end
function ByteData:clone()
  local b = {}
  for i = 1, #self._bytes do b[i] = self._bytes[i] end
  return setmetatable({ _bytes = b }, ByteData)
end
-- A "pointer" here is the byte table itself, which is exactly what this
-- engine's ffi.cast consumes. Handing it out is what lets the ffi shim be a
-- VIEW over the same storage rather than a copy.
function ByteData:getPointer() return self end
ByteData.getFFIPointer = ByteData.getPointer

love.data.newByteData = function(src)
  if rawtype(src) == "number" then
    local n, b = math.floor(src), {}
    for i = 1, n do b[i] = 0 end
    return setmetatable({ _bytes = b }, ByteData)
  end
  if rawtype(src) == "table" and src._bytes then return src:clone() end
  local s = tostring(src)
  local b = {}
  for i = 1, #s do b[i] = s:byte(i) end
  return setmetatable({ _bytes = b }, ByteData)
end

-- string.pack/unpack are Lua 5.4 built-ins, so love.data.pack is a thin
-- wrapper rather than a reimplementation.
love.data.pack = function(container, fmt, ...)
  local s = string.pack(fmt, ...)
  if container == "data" then return love.data.newByteData(s) end
  return s
end
love.data.unpack = function(fmt, data, pos)
  local s = (rawtype(data) == "table" and data.getString and data:getString()) or tostring(data)
  return string.unpack(fmt, s, pos)
end
love.data.getPackedSize = function(fmt) return string.packsize(fmt) end

-- A non-cryptographic hash. LOVE offers md5/sha1/... for content addressing
-- (3DreamEngine keys its shader cache on one); the VALUE only has to be
-- stable within a run, and saying which algorithm it really is beats
-- claiming md5 and returning something else.
-- Returns RAW BYTES, as LOVE's does -- callers pipe the result straight into
-- love.data.encode("string", "hex", ...) to get something printable, and a
-- hex string here would come back double-encoded.
--
-- This is FNV-1a, not md5/sha1, whatever algorithm name is passed. The
-- callers that matter use a hash to KEY a cache (3DreamEngine names its
-- vertex structs this way), which needs determinism and good dispersion,
-- not a specific digest. Claiming md5 and returning something else would be
-- worse only if a cart compared the value against a real md5 computed
-- elsewhere -- which a cart has no way to do.
love.data.hash = function(a, b)
  local s = b == nil and a or b
  s = (rawtype(s) == "table" and s.getString and s:getString()) or tostring(s)
  local h = 0xcbf29ce484222325
  for i = 1, #s do
    h = h ~ s:byte(i)
    h = (h * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
  end
  -- 8 raw bytes, big-endian, so the hex encoding of it reads left to right.
  local out = {}
  for i = 7, 0, -1 do out[#out + 1] = string.char((h >> (i * 8)) & 0xFF) end
  return table.concat(out)
end

-- The inverse of love.data.decode above, same alphabet, same restriction.
local B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
love.data.encode = function(container, format, src)
  local s = (rawtype(src) == "table" and src.getString and src:getString()) or tostring(src)
  -- hex is the other format LOVE supports, and it is what callers use to
  -- turn a hash into a printable identifier.
  if format == "hex" then
    local out = s:gsub(".", function(c) return string.format("%02x", c:byte()) end)
    if container == "data" then return love.data.newByteData(out) end
    return out
  end
  if format ~= "base64" then
    error("love.data.encode: supported formats are 'base64' and 'hex', got '" ..
          tostring(format) .. "'", 2)
  end
  local out = {}
  for i = 1, #s, 3 do
    local a, b, c = s:byte(i), s:byte(i + 1), s:byte(i + 2)
    local n = a * 65536 + (b or 0) * 256 + (c or 0)
    local c1 = (n >> 18) & 63
    local c2 = (n >> 12) & 63
    local c3 = (n >> 6) & 63
    local c4 = n & 63
    out[#out + 1] = B64:sub(c1 + 1, c1 + 1) .. B64:sub(c2 + 1, c2 + 1) ..
                    (b and B64:sub(c3 + 1, c3 + 1) or "=") ..
                    (c and B64:sub(c4 + 1, c4 + 1) or "=")
  end
  local str = table.concat(out)
  if container == "data" then return love.data.newByteData(str) end
  return str
end

-- ── love.thread ─────────────────────────────────────────────────────
--
-- A cart is ONE wasm instance. There is no second thread and there cannot
-- be one. But refusing the whole module outright turned out to be the wrong
-- call, because of what libraries actually use threads FOR: a background
-- worker fed by a Channel, usually to load assets without stalling the
-- frame. That pattern decomposes into two halves, and only one of them
-- needs a thread.
--
--   * Channels are just queues. They work perfectly in one thread, and
--     implementing them means a library's produce/consume code runs
--     unchanged.
--   * The worker is the part that cannot exist. A Thread here starts
--     nothing.
--
-- So Channels are REAL and Threads are inert. A library that pushes jobs and
-- polls for results gets an empty result queue -- which is the same thing it
-- sees when its workers are merely busy, so it keeps running rather than
-- breaking. Work that must actually happen has to happen on the main
-- thread; a cart doing async asset streaming should load synchronously
-- instead.
--
-- Thread:start() logs ONCE, so a cart author can see why their background
-- work never completes, without 60 lines a second.
local channels = {}
local Channel = {}
Channel.__index = Channel

function Channel:push(v) self.q[#self.q + 1] = v end
function Channel:supply(v) self.q[#self.q + 1] = v return true end
function Channel:pop()
  if #self.q == 0 then return nil end
  return table.remove(self.q, 1)
end
function Channel:peek() return self.q[1] end
-- demand() BLOCKS in LOVE. Blocking here would hang the frame forever, since
-- nothing else can ever run to fill the queue -- so it returns nil instead.
function Channel:demand(timeout)
  return self:pop()
end
function Channel:getCount() return #self.q end
function Channel:clear() self.q = {} end
function Channel:performAtomic(fn, ...) return fn(self, ...) end
function Channel:release() return true end
function Channel:type() return "Channel" end

local warned_thread = false
local Thread = {}
Thread.__index = Thread
function Thread:start()
  if not warned_thread then
    warned_thread = true
    wc.log("love.thread: this engine is a single wasm instance, so a Thread " ..
           "never runs. Channels ARE real queues, so producer/consumer code " ..
           "works -- but anything that must actually execute has to run on " ..
           "the main thread. (Asset streaming: load synchronously instead.)")
  end
  return true
end
function Thread:wait() return true end
function Thread:isRunning() return false end
function Thread:getError() return nil end
function Thread:release() return true end
function Thread:type() return "Thread" end

love.thread = {
  newThread = function() return setmetatable({}, Thread) end,
  getChannel = function(name)
    local c = channels[name]
    if not c then
      c = setmetatable({ q = {}, name = name }, Channel)
      channels[name] = c
    end
    return c
  end,
  newChannel = function() return setmetatable({ q = {} }, Channel) end,
}

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

-- ── the OFFICIAL LOVE physics API ──────────────────────────────────
--
-- Same Box2D underneath as the windfield-style colliders below -- this is
-- a SHAPE translation, not a second engine. It exists because the API
-- status table scored love.physics at 14% while the engine had full Box2D:
-- we only ever exposed the windfield spelling, so a game written against
-- real LOVE could not find newBody/newFixture and simply failed.
--
-- The structural difference that makes this more than aliasing: in LOVE a
-- SHAPE is a standalone geometry description created with no body, and a
-- FIXTURE is what binds shape to body and carries the material. Our
-- bindings attach a shape to a body at creation. So a LOVE Shape here is a
-- DEFERRED description -- it records what to build -- and newFixture is
-- where the real b2 shape finally gets made.

local Body, Shape, Fixture = {}, {}, {}
Body.__index, Shape.__index, Fixture.__index = Body, Shape, Fixture

function physics.newBody(world, x, y, btype)
  local h = b2.body_new(world.handle, x or 0, y or 0,
                        TYPE_NUM[btype or "static"] or STATIC)
  return setmetatable({ handle = h, world = world, fixtures = {} }, Body)
end

-- Shapes: geometry only, no body yet. LOVE takes rectangles as (w, h)
-- centred on the body origin, optionally offset.
function physics.newRectangleShape(a, b, c, d, e)
  if c then  -- (x, y, w, h, angle)
    return setmetatable({ kind = "box", x = a, y = b, hw = c / 2, hh = d / 2,
                          angle = e or 0 }, Shape)
  end
  return setmetatable({ kind = "box", x = 0, y = 0, hw = a / 2, hh = b / 2,
                        angle = 0 }, Shape)
end

function physics.newCircleShape(a, b, c)
  if c then return setmetatable({ kind = "circle", x = a, y = b, r = c }, Shape) end
  return setmetatable({ kind = "circle", x = 0, y = 0, r = a }, Shape)
end

function physics.newPolygonShape(...)
  local a = ...
  local pts = (type(a) == "table") and a or { ... }
  return setmetatable({ kind = "polygon", pts = pts }, Shape)
end

function physics.newEdgeShape(x1, y1, x2, y2)
  return setmetatable({ kind = "edge", x1 = x1, y1 = y1, x2 = x2, y2 = y2 }, Shape)
end

-- A chain is a run of edges. Built as segments at fixture time, which is
-- what the binding can express.
function physics.newChainShape(loop, ...)
  local a = ...
  local pts = (type(a) == "table") and a or { ... }
  return setmetatable({ kind = "chain", loop = loop and true or false,
                        pts = pts }, Shape)
end

function Shape:getType() return self.kind == "box" and "polygon" or self.kind end
function Shape:getRadius() return self.r or 0 end

-- THE BINDING STEP. Everything above was a description; this is where the
-- b2 shape is actually created on the body.
function physics.newFixture(body, shape, density)
  local set = { density = density or 1 }
  local sh
  if shape.kind == "box" then
    -- our shape_box is body-centred, so a shape offset has to move the box
    sh = b2.shape_box(body.handle, shape.hw, shape.hh, set)
  elseif shape.kind == "circle" then
    sh = b2.shape_circle(body.handle, shape.r, set)
  elseif shape.kind == "polygon" then
    sh = b2.shape_polygon(body.handle, shape.pts, set)
  elseif shape.kind == "edge" then
    sh = b2.shape_segment(body.handle, shape.x1, shape.y1, shape.x2, shape.y2, set)
  elseif shape.kind == "chain" then
    -- one segment per span; the loop variant closes back to the start
    local pts, n = shape.pts, #shape.pts / 2
    local last
    for i = 1, n - 1 do
      last = b2.shape_segment(body.handle, pts[i*2-1], pts[i*2],
                              pts[i*2+1], pts[i*2+2], set)
    end
    if shape.loop and n > 2 then
      last = b2.shape_segment(body.handle, pts[n*2-1], pts[n*2], pts[1], pts[2], set)
    end
    sh = last
  else
    error("love.physics.newFixture: unknown shape kind " .. tostring(shape.kind), 2)
  end
  local f = setmetatable({ handle = sh, body = body, shape = shape,
                           density = set.density }, Fixture)
  body.fixtures[#body.fixtures + 1] = f
  return f
end

-- Body: the accessors games actually call.
function Body:getPosition() return b2.body_position(self.handle) end
function Body:getX() local x = b2.body_position(self.handle) return x end
function Body:getY() local _, y = b2.body_position(self.handle) return y end
function Body:setPosition(x, y) b2.body_set_position(self.handle, x, y) end
function Body:getAngle() return b2.body_angle(self.handle) end
function Body:setAngle(a) b2.body_set_angle(self.handle, a) end
function Body:getLinearVelocity() return b2.body_velocity(self.handle) end
function Body:setLinearVelocity(vx, vy) b2.body_set_velocity(self.handle, vx, vy) end
function Body:getMass() return b2.body_mass(self.handle) end
function Body:applyForce(fx, fy) b2.body_apply_force(self.handle, fx, fy) end
function Body:applyLinearImpulse(ix, iy) b2.body_apply_impulse(self.handle, ix, iy) end
function Body:setType(t) b2.body_set_type(self.handle, TYPE_NUM[t] or STATIC) end
function Body:getType()
  return TYPE_STR[b2.body_set_type and self._type or DYNAMIC] or "dynamic"
end
function Body:setBullet(v) b2.body_set_bullet(self.handle, v and true or false) end
function Body:setFixedRotation(v) b2.body_set_fixed_rotation(self.handle, v and true or false) end
function Body:setLinearDamping(d) b2.body_set_linear_damping(self.handle, d) end
function Body:setGravityScale(g) b2.body_set_gravity_scale(self.handle, g) end
function Body:isDestroyed() return not b2.body_alive(self.handle) end
function Body:destroy() b2.body_destroy(self.handle) end
function Body:getFixtures() return self.fixtures end
-- LOVE reports the body's own centre; ours is the body origin.
function Body:getWorldCenter() return b2.body_position(self.handle) end

function Fixture:getBody() return self.body end
function Fixture:getShape() return self.shape end
function Fixture:getDensity() return self.density end
function Fixture:setFriction(f) self.friction = f end
function Fixture:getFriction() return self.friction or 0.2 end
function Fixture:setRestitution(r) self.restitution = r end
function Fixture:getRestitution() return self.restitution or 0 end
function Fixture:destroy() end

function physics.getDistance(a, b)
  local ax, ay = b2.body_position(a.body.handle)
  local bx, by = b2.body_position(b.body.handle)
  local dx, dy = bx - ax, by - ay
  return math.sqrt(dx * dx + dy * dy)
end

-- ── joints ─────────────────────────────────────────────────────────
--
-- LOVE's joint API was designed against Box2D 2.x, which had eleven joint
-- types. This engine runs Box2D 3.2, whose rewrite consolidated them into
-- seven. So the mapping is not one-to-one, and where it is not, this says
-- so rather than papering over it:
--
--   revolute, prismatic, distance, weld, motor, wheel
--       direct -- a real b2 joint of that type
--   rope
--       a distance joint with its limit enabled and its spring slack.
--       2.x's rope joint was FOLDED INTO the distance joint in v3; this is
--       upstream's own replacement, not an approximation.
--   friction
--       a motor joint with zero target velocity and a capped force, so all
--       it can do is brake. Same construction v3 recommends.
--   mouse
--       a motor joint with a linear spring, anchored to a body the caller
--       moves. Box2D 3.2's OWN SAMPLES implement mouse dragging exactly
--       this way (samples/sample.cpp, Sample::MouseDown), spring constants
--       included, so this is the sanctioned substitute.
--   gear, pulley
--       NOT IMPLEMENTED. v3 removed both and offers no primitive to build
--       them on. They could be faked in Lua by reading one joint each step
--       and driving the other, but a constraint solved outside the solver
--       drifts under load and fights the very bodies it constrains -- it
--       would be wrong precisely when a game leans on it. A joint that is
--       subtly wrong is worse than one that is honestly missing, so these
--       raise a clear error naming the reason.

local Joint = {}
Joint.__index = Joint

local function wrapJoint(h, kind)
  return setmetatable({ handle = h, kind = kind }, Joint)
end

local function jointBodies(a, b)
  -- LOVE takes Body objects; our C layer takes integer handles.
  return a.handle, b.handle
end

function physics.newRevoluteJoint(a, b, x, y, collide)
  local ha, hb = jointBodies(a, b)
  return wrapJoint(b2.joint_revolute(a.world.handle, ha, hb, x, y, collide), "revolute")
end

function physics.newDistanceJoint(a, b, x1, y1, x2, y2, collide)
  local ha, hb = jointBodies(a, b)
  -- LOVE names both anchors; the rest length is the distance between them.
  local dx, dy = (x2 or x1) - x1, (y2 or y1) - y1
  local len = math.sqrt(dx * dx + dy * dy)
  return wrapJoint(b2.joint_distance(a.world.handle, ha, hb, x1, y1, len, collide),
                   "distance")
end

function physics.newPrismaticJoint(a, b, x, y, ax, ay, collide)
  local ha, hb = jointBodies(a, b)
  return wrapJoint(b2.joint_prismatic(a.world.handle, ha, hb, x, y, ax, ay, collide),
                   "prismatic")
end

function physics.newWeldJoint(a, b, x, y, collide)
  local ha, hb = jointBodies(a, b)
  return wrapJoint(b2.joint_weld(a.world.handle, ha, hb, x, y, collide), "weld")
end

function physics.newMotorJoint(a, b, correction, collide)
  local ha, hb = jointBodies(a, b)
  return wrapJoint(b2.joint_motor(a.world.handle, ha, hb, collide), "motor")
end

function physics.newWheelJoint(a, b, x, y, ax, ay, collide)
  local ha, hb = jointBodies(a, b)
  return wrapJoint(b2.joint_wheel(a.world.handle, ha, hb, x, y, ax, ay, collide),
                   "wheel")
end

function physics.newRopeJoint(a, b, x1, y1, x2, y2, maxLength, collide)
  local ha, hb = jointBodies(a, b)
  return wrapJoint(b2.joint_rope(a.world.handle, ha, hb, x1, y1, maxLength, collide),
                   "rope")
end

function physics.newFrictionJoint(a, b, x, y, collide)
  local ha, hb = jointBodies(a, b)
  return wrapJoint(b2.joint_friction(a.world.handle, ha, hb, x, y, 100, 100, collide),
                   "friction")
end

-- LOVE's signature is newMouseJoint(body, x, y) -- ONE body, dragged toward
-- a world point. The anchor body the motor joint needs is created here and
-- kept alive on the joint, so a cart never sees the difference.
function physics.newMouseJoint(body, x, y)
  local world = body.world
  local anchor = b2.body_new(world.handle, x, y, KINEMATIC)
  local h = b2.joint_mouse(world.handle, anchor, body.handle, x, y, 1000)
  local j = wrapJoint(h, "mouse")
  j.anchor = anchor
  return j
end

function physics.newGearJoint()
  error("love.physics.newGearJoint is not available: Box2D 3.x removed the " ..
        "gear joint and offers no primitive to build one on. Drive the two " ..
        "joints from your own update instead -- see API_STATUS.md.", 2)
end

function physics.newPulleyJoint()
  error("love.physics.newPulleyJoint is not available: Box2D 3.x removed the " ..
        "pulley joint and offers no primitive to build one on. Two rope " ..
        "joints with a shared length budget are the usual stand-in -- see " ..
        "API_STATUS.md.", 2)
end

-- ── Joint methods ──────────────────────────────────────────────────

function Joint:getType() return self.kind end
function Joint:type() return "Joint" end

function Joint:destroy()
  if self.handle then
    b2.joint_destroy(self.handle)
    -- a mouse joint owns its anchor body; leaking one per drag would fill
    -- the body table over a long session
    if self.anchor then b2.body_destroy(self.anchor); self.anchor = nil end
    self.handle = nil
  end
end
Joint.release = Joint.destroy

function Joint:isDestroyed() return self.handle == nil end

function Joint:getReactionForce()  return b2.joint_force(self.handle) end
function Joint:getReactionTorque() return b2.joint_torque(self.handle) end

-- Motor. LOVE spells these per joint type; they all land on the same C call.
function Joint:enableMotor(on) b2.joint_set_motor(self.handle, on, self._speed or 0,
                                                  self._maxMotor or 1000) end
function Joint:isMotorEnabled() return self._motorOn or false end
function Joint:setMotorSpeed(v)
  self._speed = v
  b2.joint_set_motor(self.handle, true, v, self._maxMotor or 1000)
  self._motorOn = true
end
function Joint:getMotorSpeed() return self._speed or 0 end
function Joint:setMaxMotorForce(v)
  self._maxMotor = v
  b2.joint_set_motor(self.handle, self._motorOn or false, self._speed or 0, v)
end
Joint.setMaxMotorTorque = Joint.setMaxMotorForce
function Joint:getMaxMotorForce() return self._maxMotor or 1000 end
Joint.getMaxMotorTorque = Joint.getMaxMotorForce

-- Limits.
function Joint:enableLimit(on)
  self._limitOn = on and true or false
  b2.joint_set_limits(self.handle, self._limitOn, self._lower or 0, self._upper or 0)
end
function Joint:isLimitEnabled() return self._limitOn or false end
function Joint:setLimits(lo, hi)
  self._lower, self._upper = lo, hi
  b2.joint_set_limits(self.handle, true, lo, hi)
  self._limitOn = true
end
function Joint:getLimits() return self._lower or 0, self._upper or 0 end
function Joint:setLowerLimit(v) self:setLimits(v, self._upper or 0) end
function Joint:setUpperLimit(v) self:setLimits(self._lower or 0, v) end
function Joint:getLowerLimit() return self._lower or 0 end
function Joint:getUpperLimit() return self._upper or 0 end

-- Spring (LOVE calls it stiffness/damping on several joint types).
function Joint:setSpringFrequency(hz)
  self._hz = hz
  b2.joint_set_spring(self.handle, hz > 0, hz, self._damp or 0.7)
end
function Joint:getSpringFrequency() return self._hz or 0 end
function Joint:setSpringDampingRatio(d)
  self._damp = d
  b2.joint_set_spring(self.handle, (self._hz or 0) > 0, self._hz or 4, d)
end
function Joint:getSpringDampingRatio() return self._damp or 0.7 end

-- Distance/rope length.
function Joint:getLength() return b2.joint_length(self.handle) end
function Joint:setLength(v) b2.joint_set_length(self.handle, v) end
function Joint:setMaxLength(v)
  self._upper = v
  b2.joint_set_limits(self.handle, true, 0, v)
end
function Joint:getMaxLength() return self._upper or 0 end

function Joint:getJointAngle()       return b2.joint_angle(self.handle) end
function Joint:getJointTranslation() return b2.joint_translation(self.handle) end

-- Mouse joint target.
function Joint:setTarget(x, y) b2.joint_set_target(self.handle, x, y) end
function Joint:getTarget()
  if not self.anchor then return 0, 0 end
  return b2.body_position(self.anchor)
end

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

-- Release the world AND its C slot.
--
-- This used to clear the collider table and stop, leaking the underlying
-- b2 world every time. The engine allows 4 -- deliberately, since a cart
-- needing five simultaneous physics worlds has a design problem -- so a
-- game that builds a fresh world per level died on the fifth with
-- "too many worlds (max 4)" and no clue why, having dutifully called
-- destroy() each time.
function World:destroy()
  for _, c in pairs(self.colliders) do c:destroy() end
  self.colliders = {}
  if self.handle then
    b2.world_destroy(self.handle)
    self.handle = nil
  end
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

-- The LuaJIT `bit` library. Lua 5.3 added native bitwise OPERATORS and
-- dropped the library, but the library is what LuaJIT-era code calls -- and
-- since LOVE has always shipped LuaJIT, that is most of the ecosystem.
-- Implemented on the native operators, so it is exact rather than a
-- floating-point emulation.
--
-- Semantics follow LuaJIT's: 32-bit, results normalized to a SIGNED 32-bit
-- integer. Returning the raw 64-bit value would differ the moment a high bit
-- is set, which is exactly where bit twiddling lives.
local function tobit32(x)
  x = math.floor(x) & 0xFFFFFFFF
  if x >= 0x80000000 then x = x - 0x100000000 end
  return x
end
bit = bit or {
  tobit = tobit32,
  band = function(a, b, ...)
    local r = math.floor(a) & math.floor(b)
    for _, v in ipairs({ ... }) do r = r & math.floor(v) end
    return tobit32(r)
  end,
  bor = function(a, b, ...)
    local r = math.floor(a) | math.floor(b)
    for _, v in ipairs({ ... }) do r = r | math.floor(v) end
    return tobit32(r)
  end,
  bxor = function(a, b, ...)
    local r = math.floor(a) ~ math.floor(b)
    for _, v in ipairs({ ... }) do r = r ~ math.floor(v) end
    return tobit32(r)
  end,
  bnot = function(a) return tobit32(~math.floor(a)) end,
  lshift = function(a, n) return tobit32((math.floor(a) & 0xFFFFFFFF) << (n & 31)) end,
  -- LOGICAL right shift: the operand is masked to 32 bits first, so a
  -- negative input shifts in zeros rather than sign bits, which is what
  -- LuaJIT's rshift does and what packing code depends on.
  rshift = function(a, n) return tobit32((math.floor(a) & 0xFFFFFFFF) >> (n & 31)) end,
  arshift = function(a, n)
    local v = tobit32(a)
    return tobit32(v >> (n & 31) | (v < 0 and ~(0xFFFFFFFF >> (n & 31)) or 0))
  end,
  rol = function(a, n)
    local v = math.floor(a) & 0xFFFFFFFF
    n = n & 31
    return tobit32(((v << n) | (v >> (32 - n))) & 0xFFFFFFFF)
  end,
  ror = function(a, n)
    local v = math.floor(a) & 0xFFFFFFFF
    n = n & 31
    return tobit32(((v >> n) | (v << (32 - n))) & 0xFFFFFFFF)
  end,
  tohex = function(a, n) return string.format("%0" .. math.abs(n or 8) .. "x",
                                              math.floor(a) & 0xFFFFFFFF) end,
}
math.frexp = math.frexp or function(x)
  if x == 0 then return 0, 0 end
  local e = math.floor(math.log(math.abs(x), 2)) + 1
  return x / 2 ^ e, e
end
math.log10 = math.log10 or function(x) return math.log(x, 10) end

-- setfenv/getfenv over 5.4's _ENV. Function environments are upvalue 1
-- named "_ENV" when a function references any global.
function setfenv(fn, env)
  if rawtype(fn) == "number" then
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
  if rawtype(fn) == "number" or fn == nil then return _G end
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
--
-- `package` is NOT opened by the C side (open_cart_libs skips it, along with
-- io and os, because a cart has no filesystem and package.loadlib would drag
-- in dynamic loading). But `package.loaded` is not part of that hazard: it
-- is a plain table, and reading or writing it is one of the most common
-- idioms in real Lua libraries -- the self-registration line
--
--     package.loaded[...] = M
--
-- which a library runs so its own submodules can require it back without
-- re-executing it. g3d's init.lua does exactly this on its line 46, and
-- without the table the cart dies with "attempt to index a nil value
-- (global 'package')" before a single frame renders.
--
-- So: expose `package.loaded`, backed by the SAME table require uses, and
-- nothing else. A library that writes to it is registering a module and that
-- now works; a library that reaches for package.path or package.loadlib
-- still finds nil, which is the honest answer -- there is no search path and
-- no dynamic loading in a cart.
local loaded = {}
package = { loaded = loaded }

function require(name)
  if loaded[name] then return loaded[name] end
  -- In LOVE every module is also requirable by name -- require("love.system")
  -- is how a library pulls in a submodule it uses conditionally, and real
  -- code does it (3DreamEngine requires love.system and love.thread this
  -- way). The tables already exist; resolve to them rather than looking for
  -- a file that a cart could never contain.
  local sub = name:match("^love%.([%w_]+)$")
  if sub then
    local mod = love[sub]
    if mod ~= nil then
      loaded[name] = mod
      return mod
    end
  end
  if name == "love" then
    loaded[name] = love
    return love
  end
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
-- Version identity. Libraries branch on this to pick an API shape, and a
-- missing getVersion means they guess -- usually at the oldest one.
-- Reported as LOVE 11.4, which is the API generation this engine follows
-- (colours are 0-1, love.graphics.newText exists, and so on).
function love.getVersion() return 11, 4, 0, "Mysterious Mysteries" end
function love.isVersionCompatible(major, minor, rev)
  if type(major) == "string" then
    local a, b, c = major:match("^(%d+)%.(%d+)%.?(%d*)$")
    if not a then return false end
    major, minor, rev = tonumber(a), tonumber(b), tonumber(c) or 0
  end
  return major == 11 and (minor or 0) <= 4
end

local deprecation_output = true
function love.setDeprecationOutput(v) deprecation_output = v and true or false end
function love.hasDeprecationOutput() return deprecation_output end

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

  dispatch_net()

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
  tkx, tky = 0, 0
  -- Reset the DEPTH, not the storage: the frames are pooled and reused next
  -- frame. Clearing the array would throw away exactly what push() recycles.
  graphics.__resetStack()
  graphics.clear()
  wc.set_color(cr, cg, cb, ca)

  if love.draw then love.draw() end
end

-- ── type(): LOVE objects report as "userdata" ───────────────────────
--
-- In LOVE, every engine object (Image, Mesh, Canvas, Shader, Source, ...)
-- is a C userdata, and library code type-checks on that:
--
--     if type(mesh) == "userdata" then ... else --[[ raw data ]] ... end
--
-- This engine's objects are Lua TABLES with metatables, so that check takes
-- the wrong branch -- and the wrong branch is usually "treat it as
-- unloaded data", which sends a library down a path that reconstructs an
-- object it already has. 3DreamEngine does this in five places; the first
-- one turns a live Mesh back into a cache record with no vertices.
--
-- So `type` is replaced with one that reports "userdata" for an engine
-- object and defers to the real type() for everything else. An engine
-- object is recognised by having a `type()` method returning a LOVE class
-- name, which is the same duck-typing LOVE's own `Object:typeOf` relies on.
--
-- This is deliberately narrow: it does NOT change what type() says about a
-- cart's own tables, only about objects this engine handed out.
--
-- INSTALLED LAST, at the very end of the prelude, and the prelude's own
-- code above is written against the REAL type(). Installing it earlier
-- would change the meaning of the 25 `type(x) == "table"` checks in this
-- file -- every one of which is asking about an engine object, and every
-- one of which would start taking the else branch.
local raw_type = type
local LOVE_CLASSES = {
  Image = true, Canvas = true, Mesh = true, Shader = true, Font = true,
  Quad = true, SpriteBatch = true, Source = true, ImageData = true,
  ByteData = true, Text = true, Video = true, Thread = true, Channel = true,
  Texture = true, Data = true,
}

function type(v)
  local t = raw_type(v)
  if t == "table" then
    local mt = getmetatable(v)
    if mt then
      -- __index may be a FUNCTION, not a table (the Element proxy in the ffi
      -- shim is one). Indexing it raises, so only look inside when it is
      -- really a table.
      local idx = rawget(mt, "__index")
      local f = rawget(v, "type")
      if f == nil and raw_type(idx) == "table" then f = idx.type end
      if f == nil then f = rawget(mt, "type") end
      if raw_type(f) == "function" then
        local ok, name = pcall(f, v)
        if ok and LOVE_CLASSES[name] then return "userdata" end
      end
    end
  end
  return t
end
