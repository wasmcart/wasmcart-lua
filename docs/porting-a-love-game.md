# Porting a LÖVE game

This engine speaks a LÖVE-style API on purpose: most game logic moves over
unchanged. What follows is the honest list of what differs, in the order
you'll hit it.

> Not affiliated with LÖVE. This engine shares none of its code and does not
> claim compatibility. "Source-familiar" is the goal, not a drop-in swap.

## 1. `dt` is fixed at 1/60

The single biggest change. LÖVE gives you a variable timestep; this engine
always passes `1/60`.

```lua
-- LÖVE (frame-rate independent)
x = x + speed * dt

-- here: dt is always 1/60, so a per-frame constant is equivalent
x = x + speed * dt        -- still correct, dt just never varies
x = x + 6                 -- also fine, and what most cart code ends up doing
```

Code written the LÖVE way keeps working. What breaks is code that *depends*
on dt varying — frame-time smoothing, `dt`-based interpolation of a laggy
frame, or accumulator loops that subtract a variable dt. Delete the
accumulator; the engine already gives you a fixed step.

This is deliberate: a fixed step is what makes replays and lockstep netplay
possible. See "Determinism" in the README.

## 2. Coordinates and colors match LÖVE 11

- top-left origin, y grows down: same as LÖVE
- colors are 0..1 floats: same as LÖVE 11+ (not the 0..255 of LÖVE 0.10)
- the screen is always 1280x720

If you're porting a pre-11 game, multiply your color literals by 1/255.

## 3. Input is gamepad-first

wasmcart is a cartridge platform, so the pad is the real input device.

```lua
-- LÖVE
if love.keyboard.isDown("left") then ... end

-- here (both work, the first is idiomatic)
if love.pad.isDown("left") then ... end
if love.keyboard.isDown("left") then ... end   -- mapped onto pad 1
```

The keyboard mapping covers arrows, **WASD**, `z`/`x`/`c`/`v` (face buttons),
`space`, `return`, `escape`, `backspace`, `tab`, shifts and ctrls. Anything
else won't map — if your game reads arbitrary letter keys, remap those
actions onto pad buttons. (An incomplete keymap fails *silently*: a game
gated on a key you didn't map just never advances.)

`love.mouse` works, backed by the wasmcart pointer ABI. On a pointer-less
host the right analog stick drives a virtual cursor and R acts as click, so
mouse-aimed games stay playable on a pad. Pack with `--pointer` or the host
will not deliver pointer state at all.

## 4. Files become cart assets

There is no filesystem. Everything in your `app/` directory is bundled into
the cart and read by path.

```lua
love.filesystem.read("data/level1.json")   -- works: app/data/level1.json
love.filesystem.newFile(...)               -- errors loudly: no files
```

Saves go to a size-capped save region the host persists:

```lua
love.filesystem.write(nil, "high=" .. score)   -- one blob
local s = love.filesystem.load_save()
```

Serialize to a string yourself. If you were using a save library that writes
many files, collapse it to one blob.

## 5. `require` reads from the cart

Pure-Lua dependencies work: drop them into `app/` or `app/lib/`.

```
app/
  main.lua
  lib/
    middleclass.lua
    bump.lua
  entities/
    player.lua
```

```lua
local Class = require "middleclass"      -- app/lib/middleclass.lua
local Player = require "entities.player" -- app/entities/player.lua
```

Resolution order is `<name>.lua`, `lib/<name>.lua`, `<name>/init.lua`, with
dots becoming directory separators. Modules are cached like standard Lua.

**C modules and `package.loadlib` will never work** — a cart is one wasm
module. If a dependency has a C core, you need a pure-Lua equivalent.

## 6. What you have to remove

These error loudly rather than silently no-op, so you'll find them fast:

| LÖVE feature | Here |
|---|---|
| `love.graphics.newShader` | **available** — GLES 3.0 / WebGL2 only; see below |
| `love.graphics.newMesh` | not implemented; use `polygon()` |
| `love.thread.*` | single wasm instance, no threads |
| `love.physics.*` | **available** — Box2D v3 with wasm SIMD; see below |
| `love.graphics.newVideo` | out of scope |
| `love.window.setMode` | accepted and ignored; resolution is fixed |
| `love.event.quit` | logged and ignored; consoles don't exit |

## 6a. Shaders work, on GLES 3.0

`newShader` / `setShader` / `Shader:send` are implemented on the GL2D
renderer. A LÖVE pixel shader ports unchanged **if it is already GLSL ES
3.00**: you write only `effect(...)` (or `position(...)` for a vertex
shader) and the engine synthesizes everything around it.

What a port usually has to change:

- **Delete the shader's own `#version` line.** The engine emits
  `#version 300 es` and a second directive is a compile error, so it is
  refused up front with that explanation.
- **Modernize GLSL ES 1.00 spellings.** `gl_FragColor` becomes the value you
  `return`; `texture2D` becomes `Texel` or `texture`; `varying` and
  `attribute` become `in` / `out`. Each is caught by name rather than left
  to the driver.
- **Do not build your own projection from `transform_projection`.** It is
  the identity here: vertices arrive already in clip space.
- **A shader is GPU-only.** If the renderer falls back to the software
  rasterizer (a feature GL2D does not implement), the shader stops applying
  and the engine logs that it has. It never silently renders unshaded.

See `examples/shaders/` for a working cart and `docs/api.md` for the full
surface, the `send` types, and the limits.

## 6b. Physics works, via Box2D v3

Real rigid-body physics is embedded. Both the LÖVE-shaped entry points and
the windfield collider API (`wf`) are provided natively.

If your game uses **windfield**, delete the bundled copy and let the engine
provide `wf` — the public API matches. A one-file forwarding shim is the
usual approach; see `ports/cavern/app/source/libraries/windfield/init.lua`
for a worked example.

Two things differ from LÖVE's Box2D v2:

- **No fixtures.** v3 attaches shapes directly to bodies. `love.physics.newFixture`
  has no analogue; use the collider API instead.
- **Contacts are polled, not callbacks.** `world:setCallbacks` does not exist.
  `collider:enter("Class")` after `world:update(dt)` is the supported form
  (this is what windfield already exposes, so most games need no change).

Joints, ray casts, and preSolve/postSolve are not implemented and error
clearly rather than silently doing nothing.

## 7. Performance notes

The v1 renderer is a software rasterizer. It sustains 1280x720 at 60fps with
hundreds of draw calls, but the cost model differs from LÖVE's GPU path:

- **large filled areas are the expensive thing**, not draw-call count. A
  full-screen `rectangle("fill")` per frame costs real time; the automatic
  background clear already covers you.
- `SpriteBatch` exists and behaves the same, but here it is just a retained
  draw list — there is no per-draw GPU state change to amortize, so batching
  is a correctness/compatibility feature rather than a speed one.
- Rotated rectangles go through the polygon path; unrotated ones take a fast
  path. Rotation is not free but is not dramatic either.
- Per-frame table churn is the usual real cost. The GC runs one budgeted step
  at each frame boundary, so garbage shows up as steady overhead rather than
  spikes — but reusing tables still pays.

Check `gc_kb` and `draw_calls` in the debug state if a port feels slow.

## 8. A porting checklist

1. Copy the game's `.lua` files into `app/`, dependencies into `app/lib/`.
2. Convert colors to 0..1 if the game predates LÖVE 11.
3. Delete any fixed-timestep accumulator; keep using `dt`.
4. Map keyboard actions onto pad buttons.
5. Collapse save files into one blob.
6. Run it; the loud errors will point at anything left.
7. Replace the thread usages the errors surface (physics and shaders are fine;
   shaders may need GLSL ES 1.00 spellings modernized — see 6a).
8. If it uses windfield, forward it to the engine's native `wf`.

Assets: PNG for images, WAV/OGG for audio, TTF for fonts.
