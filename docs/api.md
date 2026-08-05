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
| `setFont(f)` / `getFont()` | |
| `getWidth()` `getHeight()` `getDimensions()` | |

### Drawing

| Function | Notes |
|---|---|
| `rectangle(mode,x,y,w,h)` | `mode` is `"fill"` or `"line"` |
| `circle(mode,x,y,r)` | |
| `line(x1,y1,x2,y2,...)` | varargs or a single table |
| `points(x1,y1,...)` | |
| `polygon(mode, pts)` | table of `x1,y1,x2,y2,...`; max 64 points |
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

- **The surface is WebGL2 / GLES 3.0 only.** A shader that writes its own
  `#version`, or uses GLSL ES 1.00 spellings (`gl_FragColor`, `texture2D`,
  `varying`, `attribute`), or anything from GLES 3.1+ (compute, image
  load/store) is refused **by name** with an explanation. That is
  deliberate: handing it to the driver instead produces errors pointing at
  line numbers in generated code you never wrote.
- **`transform_projection` is the identity.** This engine has no
  model/view/projection matrix; vertices reach the vertex shader already in
  clip space. Multiplying by it is correct; deriving your own projection
  from it is not.
- **`Texel` on the draw's own texture honours the draw type.** An
  untextured draw (a rectangle, a circle) sees an all-white texel, so
  `Texel(tex, uv) * color` reduces to the vertex colour. Sample your own
  `Image` uniforms with plain `texture()`.
- **An `Image` sent as a uniform is a sub-rect of a shared atlas**, so its
  uv range is not 0..1. The engine logs the actual range. Use a `Canvas` if
  you need a 0..1 sampler.
- **Limits:** 8 shaders, 4 image uniforms per shader, 16 KB of source.
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

- **The vertex format is fixed** at `{x, y, u, v, r, g, b, a}`, and
  `newMesh(vertexformat, ...)` is **refused**. The renderer has one vertex
  layout, shared by every program including your own shaders through a single
  VAO, so extra attributes have nowhere to go. Accepting the call and
  dropping them would render something that looks nearly right.
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

## love.mouse

```lua
love.mouse.getPosition()   -- pointer, or the virtual cursor on pad-only hosts
love.mouse.getX() love.mouse.getY()
love.mouse.isDown(1, 2)    -- button 1 also mirrors pad R / A
```

Callbacks: `love.mousepressed(x, y, button)` and `love.mousereleased(...)`.
Requires packing with `--pointer`.

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
`world:update`), not delivered by callback. Joints, ray casts, and
preSolve/postSolve are not implemented and error clearly.

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
runs. On hosts older than 2026-08 the normal-load seeding is missing —
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
