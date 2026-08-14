# API reference

Every function the engine provides. Anything not listed here does not exist;
if it exists in LÖVE and not here, calling it either errors loudly or is
absent. See the README for the v1 cut list.

Screen is **1280x720** by default, **top-left origin**, y grows down. Colors
are **0..1 floats**. A cart can pick its own resolution (up to **1920x1080**)
by shipping an `app/conf.lua`, LOVE's way:

```lua
-- conf.lua: runs before the prelude and before love.load
function love.conf(t)
  t.window.width  = 1920
  t.window.height = 1080
end
```

The choice is made once at boot; `love.window.setMode` at runtime is still
accepted-and-ignored.

## Callbacks

```lua
function love.load() end             -- once, after the cart boots
function love.update(dt) end         -- dt is ALWAYS 1/60
function love.draw() end             -- after the automatic background clear
function love.keypressed(key) end    -- pad 1 edges, mapped to key names
function love.keyreleased(key) end
function love.gamepadpressed(joy, button) end
function love.gamepadreleased(joy, button) end
```

## love.graphics

### State

| Function | Notes |
|---|---|
| `setColor(r,g,b,[a])` | also accepts a table `{r,g,b,a}` |
| `getColor()` → `r,g,b,a` | 8-bit quantized, so 0.5 returns 0.502 |
| `setBackgroundColor(r,g,b)` / `getBackgroundColor()` | applied by the automatic clear |
| `clear([r,g,b])` | no args = background color |
| `setBlendMode("alpha"\|"add")` | reset to alpha each frame |
| `setScissor(x,y,w,h)` / `setScissor()` | no args disables |
| `intersectScissor(x,y,w,h)` | clips to the INTERSECTION with the current scissor, so a nested clip can never widen an outer one. Disjoint rects give an empty clip, not a negative size |
| `setFont(f)` / `getFont()` / `setNewFont(...)` | `setNewFont` sets and returns |
| `getWidth()` `getHeight()` `getDimensions()` | |
| `getSupported()` `getTextureTypes()` `getImageFormats()` | capability probes; answered honestly, so a library picks its real fallback |
| `isGammaCorrect()` `isWireframe()` | both false here |
| `discard()` `flushBatch()` | genuine no-ops: nothing is buffered across calls |

### Drawing

| Function | Notes |
|---|---|
| `rectangle(mode,x,y,w,h)` | `mode` is `"fill"` or `"line"` |
| `circle(mode,x,y,r)` | |
| `line(x1,y1,x2,y2,...)` | varargs or a single table |
| `points(x1,y1,...)` | |
| `polygon(mode, pts)` | table of `x1,y1,x2,y2,...`; max 256 points |
| `ellipse(mode,x,y,rx,ry)` | |
| `arc(mode,[arctype],x,y,r,a1,a2)` | arctype `"pie"`, `"open"`, `"closed"` |
| `print(text,x,y)` | |
| `printf(text,x,y,limit,align)` | align: `"left"`, `"center"`, `"right"` |
| `draw(image,[quad],x,y,r,sx,sy,ox,oy)` | tinted by the current color |

### Objects

```lua
local img = love.graphics.newImage("sprites/hero.png")
img:getWidth()  img:getHeight()  img:getDimensions()

local q = love.graphics.newQuad(0, 0, 16, 16, img:getWidth(), img:getHeight())
q:getViewport()  q:setViewport(x, y, w, h)

local cv = love.graphics.newCanvas(320, 240)
love.graphics.setCanvas(cv)      -- draw into it
love.graphics.setCanvas()        -- back to the screen
love.graphics.draw(cv, 0, 0)     -- composite it like an image

local f1 = love.graphics.newFont(24)             -- scaled built-in bitfont
local f2 = love.graphics.newFont("f.ttf", 24)    -- TTF
f1:getHeight()  f1:getWidth("some text")
```

The built-in bitfont covers `A-Z a-z 0-9` and
`space - . ! : > + = / ( ) , ? % * < # _ ' " ; [ ]`. Unmapped characters
render as a space.

**Image fonts** — glyphs side by side in one image, separated by whatever
colour the top-left pixel is:

```lua
local f = love.graphics.newImageFont(img, "ABCDEFG...")
```

Glyph widths come from scanning for that separator, so a *proportional*
image font measures correctly rather than assuming a fixed cell — the
difference between a menu that lines up and one that drifts.

**Text objects** hold a pre-wrapped string:

```lua
local t = love.graphics.newText(font, "hello")
t:set("replaced")            -- no wrap
t:setf("wrapped text", 400, "center")
t:getWidth() t:getHeight()   -- getWidth reports the wrap LIMIT when set
love.graphics.draw(t, x, y)
```

The wrap is computed once at `set`/`setf` time rather than every frame,
and shares `printf`'s exact algorithm — two different wrap rules in one
engine means text measured with one and drawn with the other disagree.

**Canvas extras:**

```lua
cv:renderTo(function() ... end)   -- restores the PREVIOUS target, so
                                  -- nesting works; the callback is pcalled
                                  -- so a throw cannot leave it bound
cv:newImageData()                 -- CPU path only: on GPU a canvas is a
                                  -- texture with no CPU-side pixels, and
                                  -- this refuses rather than handing back
                                  -- a blank that looks like a readback
cv:generateMipmaps()              -- no-op; every draw here samples at 1:1
```

### Particles

```lua
local ps = love.graphics.newParticleSystem(image, buffer)
ps:setParticleLifetime(0.5, 1.5)  ps:setEmissionRate(200)
ps:setDirection(-math.pi/2)       ps:setSpread(math.pi/3)
ps:setSpeed(100, 250)             ps:setLinearAcceleration(0, 300, 0, 300)
ps:setSizes(1.4, 0.9, 0.1)        ps:setSizeVariation(0.5)
ps:setColors(1,0.85,0.3,1,  1,0.3,0.1,0.85,  0.3,0.05,0.05,0)
ps:setRadialAcceleration(a, b)    ps:setTangentialAcceleration(a, b)
ps:setLinearDamping(a, b)         ps:setEmissionArea(dist, dx, dy)
ps:setPosition(x, y) ps:start() ps:stop() ps:emit(n) ps:getCount()
love.graphics.draw(ps, x, y)
```

The buffer is a **hard cap**, as in LOVE: emitting past it recycles the
oldest particle rather than growing, so a system tuned against a buffer
size behaves the same here. Size and colour lists are keyframes
interpolated across each particle's life.

### Stencil

Masking to a **non-rectangular** region — the one thing scissor cannot do.

```lua
love.graphics.stencil(function()
  love.graphics.circle("fill", 200, 200, 120)   -- the mask shape
end, "replace", 1)
love.graphics.setStencilTest("equal", 1)         -- keep only inside it
love.graphics.rectangle("fill", 0, 0, 400, 400)
love.graphics.setStencilTest()                   -- off
```

Actions: `replace`, `increment`, `decrement`, `invert`, `incrementwrap`,
`decrementwrap`. Compare modes: `equal`, `notequal`, `less`, `lequal`,
`greater`, `gequal`, `always`. Use `notequal` for the inverse mask.

**GL only.** The software rasterizer has no stencil buffer, and giving it
one would put a per-pixel test in the innermost blend loop — a cost every
cart pays to serve the few that mask. Calling `stencil()` on the CPU path
raises a named error rather than drawing an unmasked frame that looks
almost right.

**It costs nothing when unused.** The stencil renderbuffer is allocated on
a canvas's *first* stencil call, `GL_STENCIL_TEST` is only enabled while a
test is live, and no branch was added to the batched draw path.

### Shaders

Custom fragment and vertex shaders run on the GL2D renderer.

```lua
local invert = love.graphics.newShader [[
  vec4 effect(vec4 color, Image tex, vec2 texture_coords, vec2 screen_coords) {
    vec4 px = Texel(tex, texture_coords) * color;
    return vec4(1.0 - px.rgb, px.a);
  }
]]

love.graphics.setShader(invert)
love.graphics.rectangle("fill", 0, 0, 100, 100)   -- drawn inverted
love.graphics.setShader()                          -- back to the default
```

You write only the LÖVE-shaped body. The `#version` line, the ins and outs,
and a `main()` are synthesized by the engine, so the shader applies to
**every** draw path: rectangles, sprites, text and circles alike.

| Function | Notes |
|---|---|
| `newShader(pixelcode, [vertexcode])` | either argument may be an asset path instead of source; order does not matter, they are told apart by content |
| `setShader(s)` / `setShader()` | no args reverts to the engine's default program |
| `getShader()` | the currently bound Shader, or `nil` |
| `Shader:send(name, ...)` | returns `false` if the uniform is not in the linked program |
| `Shader:hasUniform(name)` | |

`send` accepts numbers, a table of 1-4 numbers (a vector), a 4x4
table-of-tables (a `mat4`), booleans, and an `Image` or `Canvas` for a
`sampler2D`:

```lua
s:send("u_time", t)                    -- float
s:send("u_tint", { 0.2, 1.0, 0.4 })    -- vec3
s:send("u_on", true)                   -- bool
s:send("u_lut", myCanvas)              -- sampler2D
```

Predefined for you, matching LÖVE: `Image`, `Texel(tex, uv)`,
`love_ScreenSize`, `VaryingColor`, `number`, and `extern` as an alias for
`uniform`.

A vertex shader defines
`vec4 position(mat4 transform_projection, vec4 vertex_position)`.

**What is different from LÖVE, and why:**

- **The surface is WebGL2 / GLES 3.0.** A shader that writes its own
  `#version`, uses `gl_FragColor`, or reaches for GLES 3.1+ (compute, image
  load/store) is refused **by name** with an explanation, rather than handed
  to the driver to produce errors pointing at line numbers in generated code
  you never wrote. (`gl_FragColor` is refused rather than rewritten because
  `effect()` *returns* its colour; rewriting it would compile and draw
  nothing.)
- **GLSL ES 1.00 spellings are rewritten, not refused.** `attribute`,
  `varying` and `texture2D` become `in`/`out`/`texture`, which is what LÖVE
  itself does — nearly every LÖVE shader in the wild is written that way.
  The rewrite only touches whole identifiers outside comments, strings and
  preprocessor lines, so a `varyingScale` uniform or a comment that mentions
  the word is left alone.
- **`transform_projection` is the identity.** This engine has no
  model/view/projection matrix; 2D vertices reach the vertex shader already
  in clip space. Multiplying by it is correct; deriving your own projection
  from it is not. (In a [3D](#3d) shader you supply your own matrices as
  uniforms, which is the same thing every LÖVE 3D library does.)
- **`Texel` on the draw's own texture honours the draw type.** An
  untextured draw (a rectangle, a circle) sees an all-white texel, so
  `Texel(tex, uv) * color` reduces to the vertex colour. Sample your own
  `Image` uniforms with plain `texture()`.
- **An `Image` sent as a uniform gets its own 0..1 texture.** Sprites live
  in a shared atlas, but a sampler uniform needs the whole image, so the
  engine uploads it separately (with `GL_REPEAT` and mipmaps) the first time
  it is sent.
- **Limits:** 64 shaders, 15 sampler uniforms per shader, 16 KB of source.
- **Shaders need GL.** `newShader` fails on a host with no GL context, and
  `setShader` fails if the renderer has fallen back to the software
  rasterizer, rather than drawing unshaded and looking almost right. If a
  later draw trips that fallback while a shader is bound, the engine says so
  in the cart log.

A compile or link failure raises a Lua error, and the driver's own info log
is written to the cart log so you see the real message.

### Meshes

Arbitrary textured, per-vertex-coloured triangles, on the GL2D renderer.

```lua
local m = love.graphics.newMesh({
  --  x    y    u  v   r  g  b  a
  {   0,   0,  0, 0,  1, 0, 0, 1 },
  { 180,   0,  1, 0,  0, 1, 0, 1 },
  { 180, 180,  1, 1,  0, 0, 1, 1 },
  {   0, 180,  0, 1,  1, 1, 1, 1 },
}, "fan")
m:setTexture(myImage)                 -- optional

love.graphics.draw(m, 40, 56)         -- x, y, r, sx, sy, ox, oy
```

The vertex is LÖVE's default format, `{x, y, u, v, r, g, b, a}`, with colour
in 0..1 and defaulting to opaque white. `u`/`v` are 0..1 over **your own
image**, and the engine remaps that into the shared atlas for you.

| Function | Notes |
|---|---|
| `newMesh(vertices, [mode], [usage])` | |
| `newMesh(vertexcount, [mode], [usage])` | vertices default to white at (0,0) |
| `Mesh:setVertices(verts, [start])` | `start` is 1-based |
| `Mesh:setVertex(i, x, y, u, v, r, g, b, a)` | also accepts a single table |
| `Mesh:getVertex(i)` | returns all 8 components |
| `Mesh:getVertexCount()` | |
| `Mesh:setTexture(img)` / `getTexture()` | an `Image` or a `Canvas`; `nil` clears |
| `Mesh:setVertexMap(map)` / `getVertexMap()` | 1-based indices, like LÖVE |
| `Mesh:setDrawRange(start, count)` / `getDrawRange()` | 1-based; no args clears |
| `Mesh:getDrawMode()` | |

Draw modes: `"fan"` (the default), `"strip"`, `"triangles"`.

**What is different from LÖVE, and why:**

- **The 2D vertex format is fixed** at `{x, y, u, v, r, g, b, a}`. A declared
  `newMesh(vertexformat, ...)` is matched against the engine's two built-in
  layouts by attribute name and size: a 2-component `VertexPosition` gets
  this 2D path, a 3-component one gets [the 3D path](#3d). A format that
  matches neither is refused by name rather than accepted with its extra
  attributes silently dropped.
- **`"points"` is refused.** There is no point primitive on this path.
  Use `love.graphics.points`, or a `"triangles"` mesh of small quads.
- **`usage` is accepted and ignored.** `"static"`/`"dynamic"`/`"stream"` is a
  GPU buffer hint; every mesh here uploads on the draw that uses it, so there
  is nothing for the hint to select. A *misspelled* usage is still an error.
- **A mesh is its own draw call.** The batcher is hardwired to quads, and a
  mesh is arbitrary triangles, so each `draw(mesh, ...)` is one
  `glDrawArrays`. Batching a hundred small meshes is not free the way a
  hundred sprites are; put them in one mesh instead.
- **`love.graphics.setColor` tints a mesh**, multiplying its per-vertex
  colours, exactly as it does a sprite.
- **Meshes need GL.** `newMesh` fails on a host with no GL context, and
  `draw` fails if the renderer has fallen back to the software rasterizer.
  There is no software path that rasterizes a textured, per-vertex-coloured
  triangle, and inventing an approximate one that disagreed with GL would be
  worse than saying so.
- **Limits:** 32 meshes, 4096 vertices each.

### 3D

The engine has a real 3D pipeline: a depth buffer, face culling, and meshes
with position, texture coordinates, normals and colour, transformed on the
GPU by your own shader.

There is no camera, no matrix stack, no model loader and no scene graph, and
that is deliberate — LÖVE has none of those either. 3D in LÖVE is four
primitives that a *library* builds a renderer on top of, and matching that
surface is what lets existing LÖVE 3D libraries run here unmodified.
[groverburger's g3d](https://github.com/groverburger/g3d) runs with its
sources copied verbatim; `test/g3d/` is that, as a conformance gate.

```lua
local mesh = love.graphics.newMesh({
  {"VertexPosition", "float", 3},
  {"VertexTexCoord", "float", 2},
  {"VertexNormal",   "float", 3},
  {"VertexColor",    "byte",  4},
}, {
  --  x  y  z    u  v    nx ny nz   r g b a
  {  -1, -1, 0,  0, 0,   0, 0, 1,   1,1,1,1 },
  {   1, -1, 0,  1, 0,   0, 0, 1,   1,1,1,1 },
  {   0,  1, 0,  0.5,1,  0, 0, 1,   1,1,1,1 },
}, "triangles")
mesh:setTexture(myImage)

local shader = love.graphics.newShader([[
  vec4 effect(vec4 color, Image tex, vec2 uv, vec2 sc) {
    return Texel(tex, uv) * color;
  }
]], [[
  uniform mat4 projectionMatrix;
  uniform mat4 viewMatrix;
  uniform mat4 modelMatrix;
  varying vec3 normal;                 // ES 1.00 spellings are rewritten
  vec4 position(mat4 transform_projection, vec4 vertex_position) {
    normal = VertexNormal;
    return projectionMatrix * viewMatrix * modelMatrix * vertex_position;
  }
]])

love.graphics.setDepthMode("lequal", true)
love.graphics.setMeshCullMode("back")

function love.draw()
  love.graphics.setShader(shader)
  shader:send("projectionMatrix", myProjection)   -- flat 16 or 4x4 nested
  shader:send("viewMatrix", myView)
  shader:send("modelMatrix", myModel)
  love.graphics.draw(mesh)                        -- NO x/y/rotation args
  love.graphics.setShader()
end
```

| Function | Notes |
|---|---|
| `newMesh(vertexformat, vertices, "triangles")` | 3-component `VertexPosition` selects 3D |
| `Mesh:setVertices(verts)` | replaces the buffer; cannot grow past the original count |
| `Mesh:setTexture(img)` / `getTexture()` | its own GL texture with `GL_REPEAT` + mipmaps |
| `Mesh:setVertexMap(indices)` | 1-based, like LÖVE |
| `setDepthMode(compare, write)` / `getDepthMode()` | `never/less/equal/lequal/greater/notequal/gequal/always` |
| `setMeshCullMode(mode)` / `getMeshCullMode()` | `none` (default), `back`, `front` |
| `setFrontFaceWinding(w)` / `getFrontFaceWinding()` | `ccw` (default), `cw` |

The vertex layout is `{x, y, z, u, v, nx, ny, nz, r, g, b, a}`; everything
past `z` is optional and defaults to uv 0, normal 0, opaque white.

**What to know before you build on this:**

- **A 3D draw takes no placement arguments.** `draw(mesh)` only — the
  transform is your shader's matrices, and the 2D transform stack has no
  meaning in a perspective projection. Passing x/y would silently shift the
  model by whatever the stack held.
- **A 3D draw REQUIRES a bound shader** with a vertex stage. There is no
  default 3D program, because there is no default projection; without one,
  `draw` fails loudly rather than drawing model-space coordinates as clip
  space.
- **Matrices are row-major**, as in LÖVE. `shader:send` accepts a flat
  16-number table or a nested 4x4 and transposes on upload. This is the
  single most expensive thing to get wrong: a transposed matrix draws
  perfectly and rasterizes nothing, with no GL error to point at it.
- **Textures bypass the 2D atlas.** A 3D mesh's texture gets its own GL
  texture so `uv` outside 0..1 can wrap, and it is mipmapped and trilinear —
  a receding surface aliases badly under the 2D path's `NEAREST`.
- **Depth is cleared for you** at the start of each frame, and depth/cull
  state is turned off around every 2D draw, so 2D and 3D compose in one
  frame without either disturbing the other.
- **`"triangles"` only.** Fans and strips are 2D conveniences; 3D geometry
  arrives triangulated.
- **3D needs GL**, on the same terms as shaders and 2D meshes: `newMesh`
  refuses on a host with no GL context rather than pretending.
- **Limits:** 64 meshes, 200000 vertices each.

### Deferred rendering

The GPU surface a real 3D renderer needs: render targets in float formats,
several of them written in one pass, cube/array/volume textures, and
instancing.

```lua
-- A g-buffer: colour + normals + depth, all written by ONE geometry pass.
local albedo = love.graphics.newCanvas(w, h, { format = "rgba16f" })
local normal = love.graphics.newCanvas(w, h, { format = "rgba16f" })
local depth  = love.graphics.newCanvas(w, h, { format = "depth24" })

local geometry = love.graphics.newShader([[
  #pragma wasmcart mrt 2
  void effect2(out vec4 c0, out vec4 c1) {
    c0 = vec4(albedoColor, 1.0);
    c1 = vec4(normalize(VertexNormal) * 0.5 + 0.5, 1.0);
  }
]], myVertexShader)

love.graphics.setCanvas({ albedo, normal, depthstencil = depth })
love.graphics.clear()
love.graphics.setShader(geometry)
love.graphics.draw(mesh)
love.graphics.setCanvas()

-- The lighting pass reads them back as samplers.
lighting:send("albedoMap", albedo)
lighting:send("normalMap", normal)
```

| Function | Notes |
|---|---|
| `newCanvas(w, h, {format=, type=, layers=, mipmaps=, msaa=})` | a GPU target |
| `setCanvas({t1, t2, ..., depthstencil=d})` | multiple render targets |
| `setCanvas({{cube, face=n}})` / `{{arr, layer=n}}` | draw into one face/layer |
| `getCanvasFormats()` / `getSystemLimits()` | **probed at runtime**, not hardcoded |
| `newCubeImage{6 faces}` / `newArrayImage{...}` / `newVolumeImage{...}` | from paths or Images |
| `drawInstanced(mesh, count)` | `gl_InstanceID` in the vertex shader |
| `setColorMask(r,g,b,a)` / `getColorMask()` | |
| `Canvas:generateMipmaps()` / `setFilter` / `setWrap` | |

**Formats:** `normal`/`rgba8`, `r8`, `rg8`, `r16f`, `rg16f`, `rgba16f`,
`r32f`, `rgba32f`, `depth16`, `depth24`, `depth32f`, `depth24stencil8`.
**Types:** `2d`, `cube`, `array`, `volume`.

**Shader sampler types:** `Image` (2D), `CubeImage`, `ArrayImage`,
`VolumeImage` — declare with the type that matches the texture, or the
shader will not compile.

### Custom vertex formats

`newMesh` accepts a fully declared format with arbitrary named attributes,
which is what a real renderer needs — a tangent for normal mapping, material
terms for PBR, per-instance data:

```lua
local mesh = love.graphics.newMesh({
  { "VertexPosition", "float", 4 },   -- 2, 3 or 4 components
  { "VertexTexCoord", "float", 2 },
  { "VertexNormal",   "byte",  4 },   -- byte attributes are always 4,
  { "VertexTangent",  "byte",  4 },   -- normalized to 0..1 in the shader
}, verts, "triangles")
```

Vertices may be flat component arrays, tables of named fields
(`VertexPositionX = ...`), or a **ByteData** of already-interleaved bytes —
the last is the zero-marshalling path for a renderer that packs its own
buffer. `Mesh:setVertexMap` likewise takes a table or a packed ByteData.

A shader declares the extra attributes itself; the engine binds each name to
a stable index across every program, so a shader linked against one mesh
draws correctly with another. **Limit: 8 attribute slots across the cart.**

### Running LÖVE 3D libraries

Both major LÖVE 3D libraries run with their sources copied verbatim, and
each is a regression gate in this repo:

| Library | Test | What it proves |
|---|---|---|
| [g3d](https://github.com/groverburger/g3d) | `test/g3d` | perspective + depth occlusion |
| [3DreamEngine](https://github.com/3dreamengine/3DreamEngine) | `test/dream3d` | ffi-packed buffers, custom vertex formats, its own `.obj` loader |

Two compatibility layers make that possible and are worth knowing about:

- **`require("ffi")` works.** Not a real FFI — there is no C to call — but
  the typed-memory subset these libraries actually use: `cdef` of flat
  structs, `new`/`cast`/`copy`/`sizeof`, 0-based indexing, and struct
  elements that write through to the backing bytes. Implemented in pure Lua.
- **`love.thread` gives you real Channels and inert Threads.** A cart is one
  wasm instance, so a Thread never runs — but Channels are genuine queues,
  so producer/consumer code works unchanged. Work that must actually happen
  has to run on the main thread.

**What to know:**

- **Two kinds of canvas.** `newCanvas(w, h)` with no settings is the
  original CPU-backed canvas the software rasterizer can draw into.
  Anything with a `format` or a non-2D `type` is a **GPU-only** target: no
  readback, no software path, and creating one on a host with no GL is an
  error rather than a silent 8-bit downgrade. That downgrade is exactly the
  bug this refuses to hide — a renderer that thinks it has 16 bits and gets
  8 produces banding no one can explain later.
- **`getCanvasFormats()` is probed, not declared.** On WebGL2 float targets
  need `EXT_color_buffer_float`, so the honest answer is a runtime one. Pick
  your g-buffer format from this table.
- **MRT needs `#pragma wasmcart mrt N`** and `void effect2(out vec4 c0,
  ...)` with one `out` per target. `effect()` returns a single colour and
  cannot express a g-buffer. Outputs are pinned to explicit locations, so
  attachment order is exactly the order in `setCanvas`.
- **`depthstencil = true` is refused.** There is no implicit depth buffer to
  hand out; create one with `{format="depth24"}` and pass it.
- **Texture units are shared.** Image samplers and render-target samplers
  draw from one pool of 15 per shader, so they can never collide.
- **Instancing carries no per-instance attributes.** Pass per-instance data
  as a uniform array indexed by `gl_InstanceID`, which is what LÖVE's own
  `drawInstanced` gives you.
- **Limits:** 32 GPU targets, 64 shaders, 8 render targets per pass (query
  `getSystemLimits().multicanvas`).

### Directory listing

`love.filesystem.getDirectoryItems` works if the cart ships an
`assets.index` file — one asset path per line, written by
`tools/gen-asset-index.sh app/` before packing. The wasmcart ABI can look an
asset up by path but cannot enumerate one, so without that index there is
nothing to list and the call errors rather than returning `{}` (which would
tell a library its resources are missing). Many Lua libraries discover their
own modules by listing a directory, so this is worth generating.

### Transforms

```lua
love.graphics.push()
love.graphics.translate(x, y)   -- composes through the current rotation/scale
love.graphics.rotate(radians)
love.graphics.scale(sx, [sy])
love.graphics.pop()
love.graphics.origin()          -- reset to identity
```

The stack is reset at the start of every frame.

## love.pad (idiomatic input)

```lua
love.pad.isDown("a")             -- pad 1
love.pad.isDown(2, "a")          -- pad 2 (1-4)
love.pad.wasPressed("start")     -- edge this frame
love.pad.wasReleased("b")
love.pad.axis("leftx")           -- -1..1; leftx lefty rightx righty
love.pad.axis(3, "lefty")
```

Buttons: `a b x y l r start select up down left right l3 r3`.

### Rumble

```lua
love.pad.hasVibration()             -- pad 1: does this device have motors?
love.pad.hasVibration(2)
love.pad.setVibration(0.5, 0.25, 0.4)     -- pad 1: strong, weak, seconds
love.pad.setVibration(2, 1, 0, 0.4)       -- pad 2 (the 4-argument form)
love.pad.setVibration()                   -- stop pad 1
love.pad.stopVibration(2)                 -- stop pad 2
love.pad.getVibration()                   -- last strengths asked for
```

`left` is the low-frequency ("strong") motor and `right` the high-frequency
("weak") one, both `0..1` and clamped. Duration is in SECONDS and capped at 5;
passing `0` uses the cap.

Both forms are all numbers, so the pad number is recognised by argument count:
only the four-argument call names a pad, which means the explicit form has to
pass a duration. Pads are numbered 1-4 as everywhere else in `love.pad`.

Rumble is capability-gated per DEVICE, not per platform: a keyboard-only setup
reports none, and calls to a pad without motors are silent no-ops, so the query
is worth making but not required. The host runs its own shutoff timer, so for
sustained rumble re-arm every frame; a cart that stops calling leaves the
motors quiet.

`Joystick:setVibration(left, right, duration)`,
`Joystick:isVibrationSupported()` and `Joystick:getVibration()` are the same
thing on the joystick objects.

## love.keyboard / love.joystick

Provided for porting; both are mapped onto the pads.

```lua
love.keyboard.isDown("left", "a")   -- any-of
```

Mapped keys: `left right up down` and `w a s d` → dpad, `z`→a, `x`→b, `c`→x,
`v`→y, `space`→a, `return`/`enter`→start, `escape`/`backspace`/`tab`→select,
`lshift`/`rshift`→l/r, `lctrl`/`rctrl`→l/r.

## love.mouse (and touch)

```lua
love.mouse.getPosition()   -- pointer, or the virtual cursor on pad-only hosts
love.mouse.getX() love.mouse.getY()
love.mouse.isDown(1, 2)    -- button 1 also mirrors pad R / A
```

Callbacks: `love.mousepressed(x, y, button)` and `love.mousereleased(...)`.
No pack flag is involved: the engine declares `WC_FLAG_POINTER` itself, and
that flag is the only gate (the old `--pointer` pack flag is a warning no-op).

**Touch.** `love.mouse` reads pointer slot 0, which is the MOUSE. Touch
fingers arrive in slots 1-9 of the wasmcart pointer ABI, one slot per finger
for as long as it stays down. Read them with the engine-level binding:

```lua
for slot = 0, 9 do
  local x, y, buttons, active = wc.pointer(slot)
  if active and buttons ~= 0 then
    -- slot 0 = mouse drag, slots 1+ = touch contacts
  end
end
```

A game that reads only `love.mouse` works perfectly on desktop and ignores
every touch on a phone -- that is the #1 pointer portability trap. Poll all
ten slots (the loop is free) unless the game genuinely wants only a cursor.
`examples/breakout` draws a per-slot stroke trace showing exactly this.

For hosts that draw ON-SCREEN touch pads, pack with an advisory `controls`
hint (`wasmcart-pack --controls dpad,a,b,start`) so the overlay shows only
what the game reads. Presentation-only: the full pad is always delivered.

```lua
for _, joy in ipairs(love.joystick.getJoysticks()) do
  joy:isGamepadDown("a")
  joy:getGamepadAxis("leftx")
end
```

## love.net (networking)

LÖVE has no networking, so this is not a LÖVE API being mirrored. It is the
wasmcart peer ABI given a LÖVE-shaped surface: polling functions on `love.net`,
callbacks assigned as `love.net.<event>` the same way `love.update` is.

```lua
local peer = love.net.open("wss://example.com/lobby")  -- id, or nil
love.net.send(peer, data)          -- data is a string of BYTES
love.net.broadcast(data)           -- every open peer; returns how many
love.net.close(peer)
love.net.state(peer)               -- "connecting" | "open" | "closing" | "closed"
love.net.isOpen(peer)
love.net.peers()                   -- array of peer ids
love.net.count()
love.net.name(peer)                -- DISPLAY ONLY, see below
love.net.transport(peer)           -- { reliable, ordered, lowLatency }

function love.net.connected(peer, name) end
function love.net.message(peer, data) end
function love.net.disconnected(peer) end
function love.net.error(peer) end
function love.net.overflow(dropped) end
```

### One primitive

There is exactly one thing here: a connection to a peer. What it runs over is
the host's business and deliberately invisible to the cart. A WebSocket, a
WebRTC data channel, a LAN socket and a serial cable all arrive as the same
peer, which is what lets one cart binary work on a host with matchmaking and on
a host where you type in an IP.

There is no client/server split either. Which end dialed is a host-side fact. A
cart that wants to be a server just behaves like one, in the messages it sends.

The address grammar belongs to the HOST, not to this engine. `wss://…`,
`room:ABCD`, `192.168.1.7:9000` are all plausible; a host that does not
understand one fails the open. `love.net.open` returning `nil` is normal and
recoverable, not an error worth crashing on: an offline device is a supported
configuration and a cart must still boot and play on one.

### Two gates, and only one is yours

Reaching the network needs BOTH halves:

1. The cart sets `WC_FLAG_NET_PEER`. The engine does this for you, always.
2. The manifest grants the transport. Pack with `--ws <domain>`, which writes
   `net.domains`, and only the domains listed there can be dialed.

Neither half can be asserted from Lua, and that is the point. A cart cannot
grant itself network reach: that decision belongs to whoever packaged it. With
no grant, every `love.net.open` returns `nil` and no callback ever fires, with
the same cart code that works when the grant is there.

```sh
wasmcart-pack --wasm engine.wasm --assets app/ --ws example.com -o game.wasc
```

### Messages are bytes

Payloads are binary. Lua strings carry arbitrary bytes including NUL, so they
are what `send`/`broadcast` take and what `love.net.message` hands back, with
exact lengths preserved end to end. Text framing, JSON, `string.pack` structs,
whatever the game wants on top is the cart's job. The ABI moves bytes and
nothing else, because a text frame is meaningful for a WebSocket and meaningless
for a serial cable.

```lua
-- pack a position update; string.pack is the natural fit
love.net.broadcast(string.pack("<Bff", MSG_MOVE, x, y))

function love.net.message(peer, data)
  local kind = string.unpack("<B", data)
  if kind == MSG_MOVE then
    local _, x, y = string.unpack("<Bff", data)
    players[peer].x, players[peer].y = x, y   -- keyed on the PEER ID
  end
end
```

### The id is the handle, the name is decoration

`peer` is a host-assigned integer, stable for the session. It is what you key a
player table on and what you pass to `send`.

`love.net.name(peer)` is DISPLAY ONLY, and this is the source of a real bug
class. The name comes from a remote machine, so it is attacker-controlled text.
It is not unique, not stable across sessions, not necessarily valid UTF-8, and
not a handle. Draw it, and nothing else. Never use it as a table key, never
compare it to decide who somebody is, and bound how much of it you render: the
engine caps how many bytes the host may hand over, but it cannot stop a cart
from trusting the contents.

### Callback timing

Peer events are queued by the engine and dispatched at the top of the frame,
before `love.update`, so a handler runs against the same world state the rest of
the frame sees rather than halfway through it. Ordering within a frame is the
host's delivery order.

The queue is bounded. A peer that floods faster than the cart drains costs the
cart frames, not memory: past the cap the oldest events are dropped and
`love.net.overflow(n)` reports how many, so a cart that needs a complete stream
can resynchronize instead of silently playing on a partial one. Messages larger
than 8 KiB are truncated to it.

### Transport properties

`love.net.transport(peer)` reports properties, never a transport name, so a cart
cannot branch on the implementation this design exists to hide. All three false
is the normal answer from a host that does not characterize its transport: read
that as "unknown, assume nothing", not as "unreliable".

## love.audio

```lua
local s = love.audio.newSource("sounds/hit.wav")   -- .wav or .ogg
s:play() s:stop() s:pause() s:resume()
s:setVolume(v) s:getVolume()
s:setPitch(p)  s:getPitch()
s:setLooping(b) s:isLooping()
s:seek(seconds) s:tell()
s:isPlaying()

love.audio.play(s)  love.audio.stop(s)
love.audio.beep(freq, [volume])    -- generated square wave, no asset needed
```

16 voices, 48kHz output.

## love.physics / wf (Box2D v3)

```lua
love.physics.setMeter(32)     -- pixels per meter (default 32)
love.physics.getMeter()
love.physics.stats()          -- { bodies, shapes, simd, simdWidth }

local world = wf.newWorld(gx, gy)          -- gravity in px/s^2
world:setGravity(gx, gy)
world:addCollisionClass("Name", { ignores = { "Other" } })
world:update(dt)
world:queryCircleArea(x, y, r, { "Enemy" })
world:queryRectangleArea(x, y, w, h, classes)
world:queryPolygonArea(verts, classes)     -- AABB-approximate
world:draw(alpha)                          -- debug outlines
world:destroy()

local c = world:newRectangleCollider(x, y, w, h)     -- x,y = top-left
local c = world:newBSGRectangleCollider(x, y, w, h, cut)  -- bevelled
local c = world:newCircleCollider(x, y, r)
local c = world:newPolygonCollider(verts)
local c = world:newLineCollider(x1, y1, x2, y2)

c:setCollisionClass("Player")
c:getPosition() c:getX() c:getY()
c:setPosition(x, y) c:setX(x) c:setY(y)
c:getLinearVelocity() c:setLinearVelocity(x, y)
c:applyForce(x, y) c:applyLinearImpulse(x, y)
c:setType("static"|"kinematic"|"dynamic")
c:setFixedRotation(b) c:setLinearDamping(v) c:setGravityScale(v) c:setBullet(b)
c:getAngle() c:setAngle(a) c:getMass()
c:setObject(o) c:getObject()
c:enter("Class") c:exit("Class") c:getEnterCollisionData("Class")
c:destroy()
```

Up to 4 independent worlds. Contacts are polled per step (`:enter` after
`world:update`), not delivered by callback. Ray casts and
preSolve/postSolve are not implemented and error clearly.

### Joints

Available two ways: the LOVE-shaped `love.physics.new*Joint`, and the raw
`b2` table underneath. Anchors are world-space pixels in both.

```lua
local j = love.physics.newRevoluteJoint(bodyA, bodyB, x, y [, collide])
local j = love.physics.newDistanceJoint(bodyA, bodyB, x1, y1, x2, y2 [, collide])
local j = love.physics.newPrismaticJoint(bodyA, bodyB, x, y, ax, ay [, collide])
local j = love.physics.newWeldJoint(bodyA, bodyB, x, y [, collide])
local j = love.physics.newMotorJoint(bodyA, bodyB [, correction, collide])
local j = love.physics.newWheelJoint(bodyA, bodyB, x, y, ax, ay [, collide])
local j = love.physics.newRopeJoint(bodyA, bodyB, x1, y1, x2, y2, maxLength)
local j = love.physics.newFrictionJoint(bodyA, bodyB, x, y [, collide])
local j = love.physics.newMouseJoint(body, x, y)

j:setMotorSpeed(v)  j:setMaxMotorForce(f)  j:enableMotor(b)
j:setLimits(lo, hi) j:enableLimit(b)       j:getLimits()
j:setSpringFrequency(hz)  j:setSpringDampingRatio(d)
j:getLength() j:setLength(v)               -- distance/rope
j:getJointAngle()                          -- revolute
j:getJointTranslation()                    -- prismatic
j:setTarget(x, y)                          -- mouse
j:getReactionForce() j:getReactionTorque()
j:destroy()
```

**Box2D 3.x has seven joint types where LOVE's API assumes 2.x's eleven**,
so three of these are built on others. That is stated here because the
behaviour is subtly different from desktop LOVE and you should know which:

| LOVE joint | here |
|---|---|
| revolute, prismatic, distance, weld, motor, wheel | a real b2 joint of that type |
| **rope** | a distance joint with its limit enabled and its spring slack. 2.x's rope joint was *folded into* the distance joint in v3; this is upstream's own replacement |
| **friction** | a motor joint with zero target velocity and a capped force, so all it can do is brake |
| **mouse** | a motor joint with a linear spring on a kinematic anchor. Box2D 3.2's own samples implement mouse dragging exactly this way, spring constants included |
| **gear**, **pulley** | **not available.** v3 removed both and offers no primitive to build them on. They raise a named error rather than silently doing nothing |

Gear and pulley could be faked in Lua by reading one joint each step and
driving the other, but a constraint solved outside the solver drifts under
load and fights the bodies it constrains — wrong precisely when a game
leans on it. A joint that is subtly wrong is worse than one that is
honestly missing.

**Do not place both bodies at the same origin.** Their local frames then
coincide, the solver has a zero-length separation to work from, and the
joint jitters instead of acting — which looks exactly like an ignored
axis argument. Measured: co-located bodies under a pure +x force moved
`(+0.16, -3.63)`; the same rig half a pixel apart moved `(+7220, +0.00)`.

The raw table, if you need it:

```lua
local j = b2.joint_revolute(world, bodyA, bodyB, ax, ay [, collide])
local j = b2.joint_distance(world, bodyA, bodyB, ax, ay, length [, collide])
local j = b2.joint_prismatic(world, bodyA, bodyB, ax, ay, axisX, axisY [, collide])
local j = b2.joint_weld(world, bodyA, bodyB, ax, ay [, collide])
local j = b2.joint_motor(world, bodyA, bodyB [, collide])
local j = b2.joint_wheel(world, bodyA, bodyB, ax, ay, axisX, axisY [, collide])
local j = b2.joint_rope(world, bodyA, bodyB, ax, ay, maxLength [, collide])
local j = b2.joint_friction(world, bodyA, bodyB, ax, ay, maxF, maxT [, collide])
local j = b2.joint_mouse(world, anchorBody, target, x, y, maxForce [, collide])
b2.joint_set_motor(j, on, speed, maxForce)
b2.joint_set_limits(j, on, lower, upper)
b2.joint_set_spring(j, on, hertz, damping)
b2.joint_type(j)     -- "revolute" | "prismatic" | ...
b2.joint_destroy(j)
b2.joint_force(j)    -- -> fx, fy   (how hard the constraint is working)
b2.joint_torque(j)

-- material after creation, not just in the shape def
b2.shape_set_friction(shape, f)      b2.shape_get_friction(shape)
b2.shape_set_restitution(shape, r)   b2.shape_get_restitution(shape)
b2.shape_set_density(shape, d)

b2.body_angular_velocity(body)       b2.body_set_angular_velocity(body, w)
b2.body_apply_torque(body, t)        b2.body_set_angular_damping(body, d)
b2.body_is_awake(body)               b2.body_set_awake(body, bool)
b2.body_enable_sleep(body, bool)
```

A distance joint measures between the two bodies' local frames, so its
anchor sets frame A while frame B rides body B's origin: passing one world
point for both would leave zero separation and the rest length would have
nothing to act on.

## b3 (Box3D)

3D rigid bodies, from Erin Catto's Box3D. There is no LOVE equivalent to
imitate -- LOVE has no 3D physics -- so this is Box3D's own vocabulary
rather than a LOVE-shaped wrapper. Shaped like the `b2` table above:
integer handles, the same pixels/meter convention, and the same argument
order (position first, type last).

```lua
b3.info()          -- { simd = "neon", threads = true, workers = 12, hw_threads = 12 }
b3.set_meter(64)   -- pixels per meter (default 64)
b3.get_meter()

local w = b3.world_new(gx, gy, gz [, workers])   -- gravity in px/s^2
b3.world_step(w, dt [, subSteps])                -- subSteps default 4
b3.world_destroy(w)

-- type: 0 static, 1 kinematic, 2 dynamic (default)
local body = b3.body_new(w, x, y, z [, type])
b3.body_destroy(body)
b3.body_position(body)                  -- -> x, y, z
b3.body_rotation(body)                  -- -> quaternion x, y, z, w
b3.body_set_transform(body, x,y,z, ax,ay,az, radians)   -- axis + angle
b3.body_velocity(body)                  -- -> vx, vy, vz
b3.body_set_velocity(body, vx, vy, vz)
b3.body_apply_force(body, fx, fy, fz)
b3.body_apply_impulse(body, ix, iy, iz)
b3.body_mass(body)

b3.shape_box(body, hx, hy, hz [, density])       -- HALF-extents
b3.shape_sphere(body, radius [, density])
b3.shape_capsule(body, halfHeight, radius [, density])   -- along local Y
b3.shape_destroy(shape)

-- surface material. Box3D's defaults are friction 0.6, restitution 0,
-- rolling resistance 0: a sensible crate, but NOT a ball. Without these a
-- struck ball neither bounces off a wall nor ever coasts to a stop.
b3.shape_set_material(shape, friction, restitution [, rollingResistance])
b3.shape_set_friction(shape, f)            b3.shape_get_friction(shape)
b3.shape_set_restitution(shape, r)         b3.shape_get_restitution(shape)
b3.shape_set_rolling_resistance(shape, r)  b3.shape_get_rolling_resistance(shape)
b3.shape_set_density(shape, d)             b3.shape_get_density(shape)

-- damping bleeds speed the way cloth or air does
b3.body_set_linear_damping(body, d)   b3.body_get_linear_damping(body)
b3.body_set_angular_damping(body, d)  b3.body_get_angular_damping(body)

b3.body_angular_velocity(body)               -- -> wx, wy, wz (rad/s)
b3.body_set_angular_velocity(body, wx,wy,wz)
b3.body_apply_torque(body, tx, ty, tz)
b3.body_apply_angular_impulse(body, ix, iy, iz)
b3.body_apply_impulse_at(body, ix,iy,iz, px,py,pz)   -- off-centre: english

-- sleep answers "has everything settled?" -- how a turn-based physics game
-- knows the shot is over
b3.body_is_awake(body)             b3.body_set_awake(body, bool)
b3.body_enable_sleep(body, bool)
b3.body_set_sleep_threshold(body, pxPerSec)
b3.body_get_sleep_threshold(body)

b3.body_set_type(body, t)          b3.body_get_type(body)
b3.body_set_bullet(body, bool)     b3.body_is_bullet(body)
b3.body_set_gravity_scale(body, s) b3.body_get_gravity_scale(body)

-- contact events. Hit events are OFF by default: opt each shape in, then
-- read them once per step. `speed` is the approach speed in px/s, which is
-- what a collision sound's volume should scale with.
b3.shape_enable_hit_events(shape, bool)

-- ── the DEFAULT RENDERER (love.physics3d.debug) ─────────────────────
--
-- Box2D ships a debug renderer that draws every body; this is the Box3D
-- equivalent, built on whatever 3D library the cart is using. It is the
-- baseline you check a game against before trusting a pixel of the real
-- graphics: when a mesh and the body it represents disagree, you see it
-- immediately instead of inferring it from a screenshot.
--
-- THE SHAPE IS THE SOURCE OF TRUTH. Create shapes through these wrappers
-- and the mesh is built from the shape's own dimensions, then only its
-- TRANSFORM is synced. Nothing describes the geometry twice, so the debug
-- view cannot drift from the simulation.
local dbg = love.physics3d.debug
dbg.init(dream, 120)          -- the 3D lib, and pixels per world unit

-- Every builder makes the SHAPE and its MESH from the same numbers. The
-- last argument of each is an optional SKIN name (see below).
dbg.box(body, hx, hy, hz [, density] [, skin])
                                           -- an UNSKINNED thin box draws as
                                           -- a PLANE, since a floor is
                                           -- usually a flattened box and a
                                           -- box outline describes it worst.
                                           -- A skinned one stays a box.
dbg.sphere(body, r [, density] [, skin])
dbg.plane(body, hx, hz [, thickness] [, density] [, skin])   -- explicit floor
dbg.capsule(body, halfHeight, r [, density] [, skin])
dbg.cylinder(body, height, r, yOffset, sides [, density] [, skin] [, vRange])
dbg.cone(body, height, r1, r2, yOffset, slices [, density] [, skin] [, vRange])

dbg.toggle()                  dbg.setEnabled(bool)   dbg.isEnabled()
dbg.reset()                   -- on world teardown, or it draws dead bodies
dbg.setBodyVisible(body, bool)  -- stop drawing a body without destroying it
dbg.draw()                    -- inside the 3D pass
dbg.count()

-- Static bodies draw green, dynamic magenta, two-sided so the view never
-- depends on the culling path being right -- that is one of the things it
-- exists to check. Shading is BAKED PER VERTEX from each builder's own
-- normals, because this engine's 3D path has no runtime lighting; without
-- it a sphere, a cylinder and a capsule are indistinguishable silhouettes.
-- Meshes are shared by size signature, so forty identical bumpers cost one
-- sphere.

-- ── SKINS: the same geometry, wearing a real surface ────────────────
--
-- The two debug colours answer "is this body where I think it is". They
-- cannot answer "does this look like a bowling alley", and a game built ON
-- the default renderer would otherwise have to abandon it to get textures
-- -- re-authoring every mesh by hand and taking back exactly the class of
-- bug this renderer exists to prevent.
--
-- A skinned shape is still built from the shape's own dimensions by the
-- same builders. Only the material and the vertex colour change.
dbg.defineSkin("lane", {
  texture   = img,            -- rides on EMISSION: with no runtime lights
                              -- an albedo-only surface renders black
  color     = { 1, 1, 1 },    -- tints the texture; replaces the debug palette
  uvScale   = 1 / 256,        -- texture repeats per PIXEL, or {u, v} per-axis
  segments  = 24,             -- sphere/capsule only; 12 is the debug default
  roughness = 0.9, metallic = 0, cullMode = "none",
})
dbg.getSkin(name)

-- uvScale is repeats per pixel so a texture stays the same physical size on
-- a long surface and a short one. Per-axis {u, v} exists for the case where
-- one axis must map EXACTLY ONCE: a bowling lane's texture is 39 boards
-- across, and a fractional repeat saws a board in half at the gutter.
--
-- vRange (cylinder/cone only) is {v0, v1}: which slice of the skin's
-- texture this section wears. A profile built from stacked hulls -- a
-- bowling pin -- is several meshes that must look like one object wearing
-- one skin, and each section's v would otherwise run 0..1 over its own
-- little height. NOTE v RUNS FROM THE TOP DOWN: v=0 is the top of the
-- profile, v=1 the bottom.
--
-- A skin named but never defined warns once and draws untextured, rather
-- than silently rendering in debug green as though that were a choice.

-- ── ART-DIRECTING THE LIGHT ─────────────────────────────────────────
--
-- The defaults are chosen to make a COLLIDER readable: high ambient, so
-- nothing is ever lost in shadow. Those are the right defaults for a debug
-- view and the wrong ones for a finished scene -- high ambient is exactly
-- what makes a picture look flat, because it shrinks the difference
-- between a face in the light and a face out of it.
dbg.setLightRig({
  ambient       = 0.24,             -- default 0.42
  key           = { -0.45, 0.80, 0.40 },   keyColor  = { 1, 0.97, 0.90 },
  fill          = {  0.65, 0.25, -0.55 },  fillColor = { 0.55, 0.65, 0.85 },
  keyIntensity  = 0.75, fillIntensity = 0.55,
})
dbg.setLighting(bool)   dbg.isLighting()
dbg.rebake()            -- re-bake every mesh under the current rig

-- Shading is baked at MESH BUILD TIME, so changing the rig has to rebuild
-- what already exists. setLightRig and setLighting do that for you; rebake
-- is there if you change something else they cannot see.
b3.shape_enable_contact_events(shape, bool)
b3.world_set_hit_threshold(world, pxPerSec)  -- below this, no hit event
b3.world_set_gravity(world, gx, gy, gz)

local ev = b3.contact_events(world)
-- ev.hits   = { {a=shape, b=shape, x=,y=,z=, nx=,ny=,nz=, speed=}, ... }
-- ev.begins = { {a=shape, b=shape}, ... }
-- ev.ends   = { {a=shape, b=shape}, ... }

-- nil when nothing is hit
local hx,hy,hz, nx,ny,nz, frac = b3.raycast(w, ox,oy,oz, dx,dy,dz)
```

Rotation is a quaternion, not an angle -- in 3D there is no single angle
to return. `body_set_transform` takes an axis and an angle so a cart never
has to build one by hand.

**SIMD and threads are properties of the BUILD, and `b3.info()` reports
what it actually got rather than what it asked for.** Natively that is
NEON or AVX2 with a real worker pool across every hardware thread; under
plain wasm it is `wasm-simd128` and `workers = 0`, because worker threads
need SharedArrayBuffer and a host that can provide it. The solver runs
serially in that case -- correct, just not parallel. Results are identical
either way: `examples/physics` asserts the same numbers on both targets.

Up to 4 worlds, 2048 bodies, 4096 shapes. Joints, sensors, and contact
events are not exposed yet.

## love.math

```lua
love.math.random()          -- [0,1)
love.math.random(n)         -- 1..n
love.math.random(a, b)      -- a..b
love.math.noise(x, [y])     -- value noise, 0..1
love.math.setRandomSeed()   -- no-op: the HOST owns the seed
```

`math.random` is replaced with the same generator, and `math.randomseed`
is a no-op, so library code that uses the stdlib RNG follows the host seed
too.

**Where the seed comes from.** The engine exports `wc_set_seed(u32)` and
draws ALL randomness from that one host-provided seed. A current wasmcart
host seeds it with fresh entropy on every normal load (different shuffle
every power-on) and pins it only for `--seed` / `deterministic` replay
runs. On hosts older than wasmcart 0.17.0 the normal-load seeding is missing —
every boot then replays the same "random" sequence. Carts that must behave
on old hosts can stir human input timing into their own PRNG (nobody
presses a button on the same frame twice); see cardtable/cards.lua in the
casino carts for the pattern.

## love.timer

```lua
love.timer.getTime()    -- seconds, derived from the frame counter
love.timer.getDelta()   -- always 1/60
love.timer.getFPS()     -- always 60
```

## love.filesystem

Reads come from the cart's bundled assets. There is no real filesystem.

```lua
local text = love.filesystem.read("data/level1.txt")
love.filesystem.getInfo("data/level1.txt")     -- {type="file"} or nil
love.filesystem.exists("data/level1.txt")
for line in love.filesystem.lines("data/x.txt") do ... end

love.filesystem.write(nil, "high=1200")   -- one blob to the save region
local saved = love.filesystem.load_save()  -- nil if never written
```

Save region is 4 KB. Serialize structured data yourself.

## require

```lua
local m = require "mymodule"      -- app/mymodule.lua
local n = require "lib.helper"    -- app/lib/helper.lua
```

Search order per name: `<path>.lua`, `lib/<path>.lua`, `<path>/init.lua`,
with `.` becoming `/`. Modules are cached. C modules are impossible.

## Debug + logging

```lua
love.log("anything", 42, someTable)   -- to the host log; `print` is an alias
love.debugValue(0, score)             -- slot 0 -> the "score" debug field
love.debugValue(1, level)             -- slot 1 -> the "aux" debug field
love.mark(7)                          -- frame-stamped event annotation
```

The cart exposes `tick_count`, `score`, `aux`, `lua_ok`, `gc_kb`, and
`draw_calls` by name through the wasmcart debug ABI. A harness reads them
with `wasm({op:'debugState'})` — no vision required.

## Window / system / event

```lua
love.window.getWidth() / getHeight() / getDimensions()
love.window.setTitle(s)     -- accepted, ignored
love.window.setMode(...)    -- accepted, ignored; resolution is chosen once
                            -- at boot by conf.lua (see the top of this file)
love.system.getOS()         -- "wasmcart"
love.event.quit()           -- logged and ignored: cartridges don't exit
```
