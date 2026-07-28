# Architecture

How the engine is put together, and why. Read this before changing
`runtime/`.

## Shape

```
  your game (app/*.lua)          ships inside the .wasc
        |
        | love.* API
        v
  prelude.lua                    the LOVE-style surface, embedded in the wasm
        |
        | wc.* (thin C bridge)
        v
  runtime.c                      VM host, software rasterizer, mixer, assets
        |
        | wasmcart ABI v3 (wc_init / wc_render / wc_get_info)
        v
  any wasmcart host              Node, browser, RetroArch, native, handheld
```

One prebuilt engine wasm (~596 KB, including Box2D v3) + your Lua source +
your assets, packed into a single `.wasc`. Games never compile anything.

## Why the API lives in Lua, not C

`prelude.lua` is embedded into the engine at build time and defines every
`love.*` function. The C layer exposes only a flat, dumb `wc.*` bridge
(`wc.rect`, `wc.circle`, `wc.image_draw`, ...).

This split is deliberate:

- **The transform stack is Lua-side.** C receives world coordinates and knows
  nothing about push/pop/rotate. That keeps the C rasterizer simple and makes
  the transform semantics easy to fix (they were wrong once; the fix was six
  lines of Lua, no rebuild of the renderer).
- **API shape can change without touching C.** Adding `printf` alignment or a
  new object method is a prelude edit.
- **An `app/prelude.lua` asset overrides the embedded one entirely.** That's a
  deliberate hacking hook: replace the whole API surface without rebuilding
  the engine.

The cost is a Lua->C call per primitive. It is not the bottleneck; pixel fill
is. See "Performance" below.

## The frame

`wc_render()` is called once per frame by the host. It:

1. Copies the four pads' buttons + sticks into Lua as 20 numbers.
2. Calls `__wasmcart_frame(...)` in the prelude, which:
   - updates pad state and fires `keypressed`/`gamepadpressed` edges
   - calls `love.update(1/60)`
   - resets the transform stack and clears to the background color
   - calls `love.draw()`
3. Runs one budgeted GC step (`lua_gc(LUA_GCSTEP, 4)`).
4. Mixes audio for the frame's duration.

Everything is wrapped in `lua_pcall`. A Lua error does not kill the cart: it
sets `lua_ok = 0`, logs a traceback, and the frame loop switches to a blue
error screen that keeps running so you can read it.

## Determinism

Four things had to be nailed down, and three of them were sources of WASI
imports too, so the fixes paid twice:

| Source of nondeterminism | Fix |
|---|---|
| wall-clock `dt` | fixed 1/60, `love.timer.getTime()` derived from the frame counter |
| `math.random` seeding from `time()` | `patch-lua.py` fixes lmathlib's seed; prelude replaces `math.random` with the host-seeded xorshift |
| Lua's string-hash seed (ASLR + time) | `luai_makeseed` overridden in `cartconf.h` — this is what makes `pairs()` order stable |
| host clock reaching Lua | `os` is never opened |

## Why the engine imports nothing but `env`

A cart that imports `wasi_snapshot_preview1` functions will not instantiate on
hosts that provide only the wasmcart `env` module. Getting to zero WASI *calls*
took four steps, each documented at its site:

1. **Never open `io`/`os`/`package`.** `open_cart_libs()` in `runtime.c`
   replaces Lua's `linit.c` and opens only base/table/string/math/utf8/
   coroutine/debug. Not "open then delete" — never linked.
2. **Compile out `luaL_loadfilex`.** It's dead code in a cart (we always load
   from asset bytes) but it drags `fopen`/`getc`. `patch-lua.py` guards it
   behind `LUA_CART_NOFILES`.
3. **Redirect Lua's write hooks.** `lua_writestring` /
   `lua_writestringerror` default to `fwrite(stdout)` / `fprintf(stderr)`.
   `cartconf.h` (force-included, so it lands before lauxlib.h's
   `#if !defined` guards) points them at `wc_log`, with line buffering in
   `cartconf.c`.
4. **Fix the two seeds** (above), which removes `clock_time_get`.

A few WASI imports still appear in the module's import *list* as linker
residue from `abort`/`snprintf` paths that are never reached. The test
harness asserts that none of them is ever **called** at runtime, which is the
property that actually matters.

## stb_vorbis lives in its own file

`stb_vorbis.c` does `#define L (PLAYBACK_LEFT|PLAYBACK_MONO)`. Lua declares
every API function as `(lua_State *L, ...)`. Including both in one
translation unit turns every Lua prototype into a syntax error. `vorbis.c`
exists solely to keep them apart.

## The renderer

Software rasterizer writing XRGB into the wasmcart framebuffer, or RGBA into
a canvas when one is bound.

- `blend_px` is the single pixel entry point: handles scissor, alpha
  compositing, additive blending, and canvas-vs-framebuffer.
- Filled rects take a fast path (direct row writes) when fully opaque,
  unscissored, unrotated, and targeting the framebuffer.
- Rotated rectangles are emitted from Lua as polygons; the polygon filler is
  a scanline even-odd fill.
- `draw_image` inverse-transforms each destination pixel back into source
  space, so rotation/scale/quad/tint are one code path. It carries a
  hard-won constraint: the per-pixel `/dw` division must NOT be replaced by
  a hoisted reciprocal. When `dw` is not a power of two, `1/dw` is inexact
  and `x*(1/dw)` rounds to a different source texel -- that moved 5.5% of
  the screen in a real game. Incremental u,v stepping fails identically.
  The speed instead comes from hoisting row-constant terms, computing the
  whole v axis once per row for unrotated blits (84% of real draws), and
  deciding destination/scissor/blend once per blit instead of per pixel.
- Text: a 5x7 bitfont (85 glyphs, upper + lower + digits + punctuation) and
  TTF via stb_truetype with a baked atlas cached per (path, pixel size).

The GL path (`wc_gl.h`, GLES3/WebGL2) is intentionally **not** in v1. The
software renderer sustains 720p60 and runs on framebuffer-only hosts
including terminals.

## Performance

Measured headless (V8, no host pacing), 180 frames each:

| example | fps headless | draw calls/frame | gc |
|---|---|---|---|
| pong | ~16800 | 22 | 124 KB |
| shmup | ~7900 | 87 | 137 KB |
| breakout | ~4600 | 73 | 143 KB |
| platformer | ~4800 | 224 | 132 KB |
| particles (900) | ~1900 | 902 | 336 KB |
| kitchen-sink | ~1500 | 136 | 141 KB |

Per-frame cost of real carts, which is the number that matters (measure with
`node test/bench.js` and the cart profiler):

| cart | median frame | share of a 60fps budget |
|---|---|---|
| pong / shmup / platformer | 0.07 - 0.19 ms | ~1% |
| particles, kitchen-sink | 0.4 - 0.6 ms | ~3% |
| the Cavern port | 5.5 ms | 33% |

Cavern is sprite-bound, not Lua-bound: 97% of its frame is inside the
blitter and about 1% is Lua. That is why the perf work went into the
rasterizer rather than into a JIT.

The rasterizer's inner loops were re-decided per pixel: every write
re-read the destination (screen or canvas), the scissor rect and the blend
mode, then branched. Those are constant for a whole run of pixels, so they
are now hoisted into `blend_span()`, which fills one horizontal run with the
decisions already made, and into an equivalent hoist for the per-pixel
Bresenham walk that lines and circle outlines use. Measured per 2000 draws:

| primitive | before | after |
|---|---|---|
| rect fill (opaque) | 0.45 ms | 0.62 ms* |
| rect fill (alpha) | 7.88 ms | 1.24 ms |
| rect outline | 1.18 ms | 0.75 ms |
| circle fill | 4.67 ms | 0.81 ms |
| line | 4.70 ms | 2.73 ms |
| polygon | 8.64 ms | 2.75 ms |
| into a canvas | 10.35 ms | 0.69 ms |
| scissored | 4.70 ms | 0.53 ms |

*opaque rect fill was already the one primitive with a fast path; it now
shares the general one, which costs a little there and pays for itself
everywhere else. Alpha rects previously cost 17x their opaque equivalent
for identical pixel counts, purely from that missing path.

A primitive-heavy frame (`test/prims`) is **2.46x** faster end to end. The
example carts do not move: they draw few enough shapes to sit at 0.06 ms
either way, and Cavern does not move either because it is sprite-bound.
Every change is bit-identical -- see the note on divisions below.

(GC figures are higher than they were pre-Box2D: the physics layer keeps
Lua-side collider tables alive. Headroom is unaffected.)

Headless fps is not host fps — it excludes vsync, audio pacing, and the
host's blit. The useful reading is the ratio: all examples have large
headroom over the 60fps budget, and the cost tracks **pixels filled**, not
draw-call count.

## Testing

`test/run.js` instantiates the engine with a minimal host and asserts:

- the module instantiates and `wc_init` doesn't trap
- `lua_ok` stays 1
- no WASI function is called at runtime
- the final frame isn't ≥99.5% a single color (blank-render detector)
- no error-shaped log lines

`test/blit/` and `test/prims/` hash a frame against a golden: the first
exercises every sprite-blit argument shape, the second every vector
primitive across the opaque, alpha, additive, scissored and canvas paths.
Both are separate from the determinism suite on purpose, and neither is
redundant:

- the determinism carts draw shapes and text but never sprites, and they
  passed for an entire debugging session while the blitter sampled the
  wrong texels;
- none of the 11 carts in the suite drew an **alpha** line, so a control
  that deliberately corrupted every alpha line pixel changed nothing
  anywhere. `test/prims/` was written for exactly that hole and fails on
  that control now.

If such a change is intended, delete the matching `golden.txt`.

Because a golden only proves a frame is *stable*, not *correct*,
`test/render-hash.js --shot <cart> <out.png>` renders any cart to a PNG so
the output can actually be looked at. Three real bugs in this engine were
found that way while the suite was fully green.

Then it runs `test/unit/` — a cart of 278 in-engine assertions covering real
Lua semantics (closures, coroutines, metatables, varargs, pcall, goto),
`require`, determinism, the API surface, font coverage, and transforms.

Finally it runs a **deliberately broken cart that must fail**. If that one
passes, the harness cannot detect failure and every other result is
unverified. A suite that only ever goes green is indistinguishable from a
suite that is broken.

## Packing a cart

The manifest declares where assets live:

```json
{ "entry": "main.wasm", "assets": "app/" }
```

The host reads `manifest.assets` in BOTH modes -- dev mode joins it as a
directory, a packed `.wasc` strips it as a path prefix -- so one field covers
both and the trailing slash is required (without it the prefix strip leaves a
leading `/` and every asset lookup misses).

Every manifest here previously omitted the field, which silently defaults to
`assets/` while the files actually live in `app/`. Packed carts therefore
booted to "missing asset: main.lua", the Cavern port included. Declaring
`"assets": "app/"` fixes both modes.

## Input: gamepad first, always

**wasmcart is a gamepad platform.** The host synthesizes a pad from the
keyboard when no physical controller is attached, so `love.pad` is the one
input a cart can always rely on. A mouse or touch pointer may not exist.

Carts should read `love.pad` (and `love.keyboard`, which the prelude maps
onto pad 1). Anything driven only by `love.mouse` -- especially the
`love.mousepressed` EVENT -- is a gap: the prelude synthesizes clicks from
the pad at a virtual cursor, but that cursor starts at screen centre, so a
hover-and-click menu is unreachable until the player nudges it onto a target.

That is not hypothetical. Cavern is a mouse game upstream, and its main menu
had no notion of a selected item, so on a pad host the game could not be
started at all. Its menu now keeps an explicit selection driven by
`love.pad`, with the mouse as an optional convenience that moves the same
cursor. Confirm accepts both face buttons and start, because which button
reads as "confirm" varies by host mapping and guessing wrong strands the
player on the title screen.

All six examples were already gamepad-only and needed no change.

## Adding to the API

1. Add the primitive to `wc_lib[]` in `runtime.c` if C work is needed.
2. Expose it in `prelude.lua` with the LÖVE-shaped signature.
3. Add an assertion to `test/unit/main.lua`.
4. `cd runtime && ./build.sh && node ../test/run.js`.
5. Look at a screenshot. Green tests did not catch the rotation bug; a
   screenshot did.
