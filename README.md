# wasmcart-lua

**Write games in Lua. Ship them as wasmcart carts.**

A real Lua 5.4 game engine with a LÖVE-style API: `love.load`, `love.update(dt)`,
`love.draw`. It compiles to a single reusable engine wasm; you write only Lua,
the engine is prebuilt and ready. The same `.wasc` runs on every wasmcart host: Node, browser, RetroArch,
native players, handhelds, and terminals. MIT, all layers open.

Drawing goes through **GL2D**, a batched WebGL2 renderer: one atlas, one draw
call for a whole frame of sprites, and coverage for circles computed in the
fragment shader. A software rasterizer is built into the same binary and
renders anything GL2D does not, so the engine never depends on a feature being
implemented twice.

wasmcart games are **gamepad games**: design for d-pad + face buttons + sticks
and they'll feel right on every device (desktop testing maps arrows/z/x onto
the pad for you).

```lua
local x, y = 640, 360

function love.update(dt)
  if love.pad.isDown("left")  then x = x - 6 end
  if love.pad.isDown("right") then x = x + 6 end
end

function love.draw()
  love.graphics.setColor(0.4, 0.8, 1)
  love.graphics.circle("fill", x, y, 36)
  love.graphics.print("hello from lua!", 40, 40)
end
```

## Quick start

**The engine wasm ships in this repo.** You never build or install a
toolchain — add your Lua and assets, and pack.

```bash
cp -r wasmcart-lua/template my-game
cd my-game && ./run.sh          # opens a window; edit app/main.lua, rerun
```

That's the whole loop: edit Lua, run. `template/` already contains
`main.wasm` (the prebuilt engine) and a `manifest.json`, so `run.sh` just
plays the directory as a dev-mode cart via `npx wasmcart`.

Your game is the `app/` directory:

```
my-game/
  main.wasm          <- the prebuilt engine, shipped here, never edited
  manifest.json
  app/
    main.lua         <- your game
    lib/*.lua        <- pure-Lua dependencies
    sprites/*.png    <- assets, read by path
    sounds/*.wav
```

When you want a shippable single file:

```bash
npx wasmcart pack --wasm main.wasm --assets app --name my-game -o my-game.wasc
```

One `.wasc` that runs on every wasmcart host. Only rebuild the engine if
you're changing the engine itself (see "Building the engine" below).

## A real LÖVE game runs on it

[Cavern](https://github.com/challacade/cavern) — a complete open-source LÖVE
adventure game — is ported in `ports/cavern/`, with **all 97 of its Lua files
byte-identical to upstream**. Only its physics wrapper is swapped for the
engine's native one. It boots, plays, and is verified through the harness.

The port ships **100% freely-licensed** — Cavern's own art, audio and levels
are CC BY-NC-ND, so all 143 of those assets were replaced with generated
MIT equivalents (see `ports/cavern/tools/`).

That port is what drove Box2D, the Lua 5.1 compatibility layer, mouse
support, SpriteBatch, and a dozen other gaps into the engine. See
`ports/cavern/README.md` for the full list of what a real game demanded.

## This is real Lua

Not a subset, not a dialect. The engine embeds **Lua 5.4** — closures,
coroutines, metatables, varargs, the garbage collector, `string.format`,
`pcall`, `goto`, integer division. Pure-Lua libraries work: drop them in
`app/` and `require` them.

```lua
local Class = require "middleclass"   -- pure-Lua libs just work
local bump  = require "bump"

local co = coroutine.create(function()
  for i = 1, 10 do coroutine.yield(i * i) end
end)
```

Anything that transpiles to Lua rides along for free: **Teal**, **Fennel**,
**YueScript**, **MoonScript**. Transpile to `.lua`, put it in `app/`, done.

## The API

> If you want the mature, batteries-included 2D framework, go use
> [LÖVE](https://love2d.org/) — it's excellent, and its API idioms are the
> reason this project speaks the same dialect. This is an unaffiliated engine
> that contains none of LÖVE's code and doesn't claim compatibility; it exists
> because Lua-games-as-open-cartridges is a good idea.

Everything happens in `love.update(dt)` and `love.draw()`, 60 times a second,
on a 1280x720 **top-left origin** canvas. Colors are 0..1 floats.

### love.graphics

| Call | Notes |
|---|---|
| `setColor(r,g,b,a)` / `getColor()` | 0..1 floats |
| `setBackgroundColor(r,g,b)` / `clear()` | cleared automatically each frame |
| `rectangle(mode,x,y,w,h)` | `"fill"` or `"line"`; rotates correctly under transforms |
| `circle(mode,x,y,r)` · `line(...)` · `points(...)` · `polygon(mode,pts)` | |
| `newImage(path)` → `Image` | PNG; `:getWidth()` `:getHeight()` |
| `newQuad(x,y,w,h,sw,sh)` → `Quad` | spritesheet tiles |
| `draw(img,[quad],x,y,r,sx,sy,ox,oy)` | rotation, scale, origin, tint from setColor |
| `newCanvas(w,h)` / `setCanvas(c)` | render targets; draw a canvas like an image |
| `newFont(size)` / `newFont(path,size)` / `setFont` | built-in bitfont + TTF |
| `print(text,x,y)` / `printf(text,x,y,limit,align)` | `"center"`, `"right"` |
| `push` `pop` `translate` `rotate` `scale` `origin` | full transform stack |
| `setScissor(x,y,w,h)` · `setBlendMode("alpha"\|"add")` | |
| `getWidth()` `getHeight()` | 1280 x 720 |

### Input (gamepad-first)

```lua
love.pad.isDown("left")            -- pad 1 held
love.pad.isDown(2, "a")            -- pad 2 held
love.pad.wasPressed("a")           -- edge, this frame
love.pad.wasReleased("start")
love.pad.axis("leftx")             -- -1..1 analog
```

Buttons: `a b x y l r start select up down left right l3 r3`.
`love.keyboard.isDown` and `love.joystick` are provided too, mapped onto the
pad; keyboard arrows/z/x reach pad 1 so you can test at a desk.
`love.keypressed` / `love.gamepadpressed` callbacks fire on edges.

### love.audio

```lua
local s = love.audio.newSource("sounds/jump.wav")  -- WAV and OGG
s:setVolume(0.7); s:setPitch(1.2); s:setLooping(true)
s:play()  s:pause()  s:seek(1.5)  s:tell()  s:isPlaying()
love.audio.beep(440)                               -- generated square, no asset
```

16 mixer voices at 48kHz.

### love.physics (Box2D v3)

Real rigid-body physics: **Box2D 3.2.0** compiled with wasm SIMD
(`-msimd128 -msse2`, so the solver is genuinely vectorized, not scalar
fallback). Two APIs over the same engine:

```lua
-- windfield-style (what most LOVE games use), provided natively as `wf`
local world = wf.newWorld(0, 900)
world:addCollisionClass("Player", { ignores = { "Enemy" } })
local p = world:newBSGRectangleCollider(x, y, w, h, 8)
p:setCollisionClass("Player")
p:setFixedRotation(true)
p:applyLinearImpulse(0, -400)
if p:enter("Wall") then ... end
world:update(dt)

love.physics.setMeter(32)          -- LOVE-shaped entry points too
love.physics.stats()               -- { simd = "wasm-simd128", bodies = n }
```

Multiple independent worlds are supported (a zero-gravity gameplay world
alongside a gravity world for debris is a common real pattern).

### love.math / love.timer / love.filesystem

```lua
love.math.random()          -- deterministic, host-seeded
love.math.random(1, 6)
love.math.noise(x, y)
love.timer.getTime()        -- frame-derived, not wall clock
love.filesystem.read("data/level1.txt")   -- cart assets
love.filesystem.write(nil, "high=1200")   -- persisted save region
love.filesystem.load_save()
```

`math.random` is replaced by the deterministic generator too, so a seeded
replay is bit-identical.

## Determinism

Same seed + same inputs → the same frames, on every host. That is a design
guarantee, not a side effect:

- `dt` is **fixed at 1/60**. There is no variable timestep (LÖVE devs: this is
  the one habit to unlearn).
- RNG comes only from the host seed (`wc_set_seed`), never a clock.
- `love.timer.getTime()` is derived from the frame counter.
- The Lua string-hash seed is fixed, so `pairs()` iteration order is stable.
- One pinned VM in wasm behaves identically everywhere.

This is what makes replays, lockstep netplay, and frame-hash regression
testing possible.

## What's not here (v1)

Each of these fails **loudly** with a message telling you what to use instead,
rather than silently doing nothing:

| Missing | Why / what to do |
|---|---|
| custom shaders, meshes | GL2D owns the pipeline; use canvases and tinting |
| threads | the engine is a single wasm instance |
| video playback | out of scope |
| real filesystem | carts have no files; use cart assets + the save region |
| variable dt | deliberate, see Determinism |

## Layout

```
runtime/     the engine: runtime.c, render2d_gl.c, physics.c, prelude.lua
ports/       real third-party games ported to the engine (Cavern)
template/    copy this to start a game
examples/    six complete games, all verified rendering
test/        headless harness, 278 in-engine assertions, GL conformance carts
docs/        porting guide, API reference, architecture
build/       engine.wasm (GL2D) + engine-cpu.wasm (comparator) + .wasc carts
```

## Building the engine

Only needed if you're changing the engine itself; games use the prebuilt wasm.

```bash
cd runtime && ./build.sh      # needs emcc + a wasmcart checkout
```

The build fetches and pins Lua 5.4.7, applies two small source guards
(documented in `patch-lua.py`), builds Box2D 3.2.0 with wasm SIMD, and links
two artifacts:

| artifact | what it is |
|---|---|
| `build/engine.wasm` | **the engine** (~629 KB). GL2D plus the software rasterizer. This is what `template/main.wasm` ships. |
| `build/engine-cpu.wasm` | software only, imports nothing from `gl`. The comparator the GL build is diffed against, and the fallback for a host with no GL. |

## Performance

GL2D against the software rasterizer, same cart, same frames, on an AMD 890M:

| cart | GL2D | software | |
|---|---|---|---|
| the Cavern port | 0.118 ms | 5.666 ms | **47.9x** |
| pong | 0.024 | 0.049 | 2.07x |
| breakout | 0.054 | 0.098 | 1.82x |
| shmup | 0.060 | 0.107 | 1.78x |
| platformer | 0.132 | 0.187 | 1.41x |
| kitchen-sink | 0.229 | 0.308 | 1.34x |
| particles | 0.446 | 0.523 | 1.17x |

Cavern is the honest headline: it is a real ported game, it is sprite-bound,
and it is the case the renderer was built for. The small examples gain less
because they were never slow -- at 0.02-0.5 ms a frame, most of what is left
is fixed per-frame cost rather than drawing.

## Testing

```bash
npm test                      # examples + 278 in-engine assertions + determinism
```

`test/run.js` asserts real failure modes: instantiation, Lua errors, blank
frames, and any WASI import being called at runtime. It ends with a
deliberately-broken cart that **must** fail — if that one passes, the harness
can't see errors and every other green is meaningless.

The `gl2d*` carts check GL against the software rasterizer by **tolerance**,
not equality: GPU blending rounds differently from the software `div255`, by
about 1 per blended draw. Sprites, canvases, text and circles hold ±2;
additive gets ±8 because it accumulates rather than converging. `test/blit`
and `test/prims` still assert **bit-equality**, against the CPU build.

`test/determinism.js` proves the determinism claim two ways: the same seed
must give a byte-identical framebuffer, and for carts whose visuals depend on
RNG a *different* seed must give a different one. Without that second check
the first is just measuring a constant.

**What has actually been exercised:** headless Node/V8 and the romdev
harness (spec conformance, screenshots, driven input, debug state), against a
real GPU via an offscreen WebGL2 context. The "runs on every wasmcart host"
line above is a property of the format — the cart is verified spec-conformant
and imports only the wasmcart `env` and `gl` modules — but it has not been run
on a handheld or in a browser here.

One consequence of GL2D being the default: `build/engine.wasm` imports the
`gl` module, which makes it a **GL cart** to every host. That is about whether
a host supplies a GL context, *not* about how it displays the result --
rendering on the GPU and printing to a terminal are orthogonal. Give the host
an offscreen context and a GL cart reads back a normal framebuffer that chafa
renders as ANSI exactly like a software one; that is precisely what the romdev
harness does for every screenshot in this repo.

Today `npx wasmcart --term` still refuses GL carts, because it loads without
a `glBackend` at all rather than because a terminal cannot show one. Until
that host grows an offscreen context, `engine-cpu.wasm` is the artifact for
it. The engine also detects a stubbed `gl` module at startup and falls back
to the software rasterizer rather than rendering a black screen.

## The wasmcart org

This is one language runtime among several. Every repo below produces or runs
the *same* `.wasc` carts, so a host that plays one plays them all. Full list:
**[github.com/orgs/wasmcart/repositories](https://github.com/orgs/wasmcart/repositories)**

| Repo | What it is |
|------|------------|
| [**wasmcart**](https://github.com/wasmcart/wasmcart) | the spec, the JS reference hosts (`CartHost`, `CartHostWeb`), and the `wasmcart` CLI + packer |
| [**wasmcart-lua**](https://github.com/wasmcart/wasmcart-lua) (this repo) | write games in Lua (Lua 5.4, LÖVE-style API, batched GL2D renderer) |
| [**wasmcart-mruby**](https://github.com/wasmcart/wasmcart-mruby) | write games in Ruby (mruby runtime, DragonRuby-style API) |
| [**wasmcart-pygame**](https://github.com/wasmcart/wasmcart-pygame) | write games in Python (CPython 3.13 + pygame-ce) |
| [**wasmcart-jsgame**](https://github.com/wasmcart/wasmcart-jsgame) | write games in JavaScript (QuickJS, Canvas 2D + WebGL2 + Web Audio) |
| [**wasmcart-sdl2**](https://github.com/wasmcart/wasmcart-sdl2) | SDL2 backend + porting guide, for bringing existing C/SDL games over |
| [**wasmcart-libretro**](https://github.com/wasmcart/wasmcart-libretro) | libretro core - run carts in RetroArch / RetroDECK |
| [**wasmcart-native**](https://github.com/wasmcart/wasmcart-native) | native player built on libnode, no Node install needed |

## License

MIT.
