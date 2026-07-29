# API reference

Every function the engine provides. Anything not listed here does not exist;
if it exists in LÖVE and not here, calling it either errors loudly or is
absent. See the README for the v1 cut list.

Screen is **1280x720**, **top-left origin**, y grows down. Colors are
**0..1 floats**.

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

`math.random` is replaced with the same deterministic generator, and
`math.randomseed` is a no-op, so library code that uses the stdlib RNG is
deterministic too.

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
love.window.setMode(...)    -- accepted, ignored; resolution is fixed
love.system.getOS()         -- "wasmcart"
love.event.quit()           -- logged and ignored: cartridges don't exit
```
