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

## GL display path (opt-in)

`runtime/build.sh` with `WCL_GL=1` also produces `build/engine-gl.wasm`: the
same engine, presenting through `wc_gl_blit` -- the spec's standard display
path, where even a pure-2D cart uploads its finished pixels as a texture and
draws one fullscreen quad.

It is a **separate artifact, not a replacement**, and default OFF. A cart
*is* a GL cart iff its wasm imports from the `gl` module, and a host handed a
GL cart with no GL context must fail the load rather than stub it. Shipping
this as the default would break every 2D-only host these carts run on today,
for a change that moves no pixels. The default `engine.wasm` is byte-identical
to one built before any of this existed, and makes zero `gl` calls.

The software rasterizer remains the reference implementation. `wc_gl_blit`
changes only *how* pixels reach the screen, so the `blit`/`prims` goldens stay
valid, and `tools/gl-verify.mjs` asserts the stronger property: run the cart
against a real WebGL2 context, read the GPU framebuffer back, and diff it
against the cart's own software framebuffer. Verified pixel-identical (0 of
921600 differing) on `blit`, `prims`, `kitchen-sink`, `shmup` and Cavern.
`test/run.js` runs this as `gl-display` when the GL engine and a GL context
are both present, and skips cleanly otherwise.

One format detail worth keeping: the framebuffer is XRGB8888 (`0x00RRGGBB`),
so its bytes run B,G,R,X in little-endian memory, while `wc_gl_blit` uploads
`GL_RGBA` (R first). The engine repacks into a scratch buffer rather than
changing the framebuffer format, which the rasterizer and every golden depend
on. Swapping those two channels is caught by gl-verify as ~23% of pixels
differing, which is how that path is known to be tested.

Why bother, when this moves no pixels and costs a repack? Because it is the
step that makes a GPU rasterizer possible later without a flag day. Measured
on this machine (`tools/gl-probe.js`, AMD 890M via native-gles), 2000
textured quads cost 0.62 ms batched against 7.45 ms for the software sprite
path -- 12x, and more against rotated sprites, which are free on a GPU. That
work is not done here; this commit only moves *presentation*.

### Can a GPU match the software rasterizer? Yes, with two corrections

Step 2 (a GPU *rasterizer*, not just GL presentation) hinges on whether a GPU
can reproduce `draw_image` bit-exactly. If it cannot, the `blit`/`prims`
goldens stop being a shared contract and the GL path needs a weaker,
tolerance-based one. `tools/gl-exactness.mjs` settles it by sweeping every
destination size from 8 to 200 px against a model of the software sampling
rule: **all 193 sizes bit-exact**, but only after two corrections.

1. **Sample position.** A GPU interpolates UV and samples at the pixel
   *centre*; `draw_image` indexes by destination pixel. Deriving the integer
   index from `gl_FragCoord` and using `texelFetch` takes the interpolator
   out of the sampling rule entirely.
2. **`floor()` on an exact boundary.** At scales like 1.5x, `idx*qw/dw` lands
   *exactly* on an integer for a third of the columns, and fp32 division
   returns a hair under it, so `floor()` drops a whole texel. Without a small
   epsilon, **30 of the 193 sizes differ, up to 55% of a sprite's pixels**.
   Deleting the epsilon makes the sweep fail again, which is how it is known
   to be doing something.

This is the same family of bug as the reciprocal-multiply one in the software
blitter: an arithmetically "equivalent" rewrite that selects a different
texel. It is not driver flakiness -- run-to-run output on this driver is
byte-identical.

### Why step 2 is not just "put sprites on the GPU"

The blocker is not exactness, it is **mixing**. The GPU path composites into
the GL framebuffer while the software path writes `wc_framebuffer`, and
software cannot see pixels the GPU drew. Any frame that uses both produces
wrong output, and rotation, canvas targets, scissor and additive blending all
have to fall back to software.

Real carts mix constantly -- Cavern issues 34 `love.graphics.draw` calls
against 36 rectangle/print/circle calls per frame, interleaved. Reconciling
the two costs ~0.19 ms per switch (0.109 readback + 0.084 upload, measured),
which is cheap once and ruinous seventy times.

So step 2 is **the whole 2D pipeline on the GPU**, not a sprite fast path
bolted onto a software rasterizer. That is a much larger change than step 1,
and it is why step 1 shipped separately: presentation moved with zero pixel
risk, and the exactness question above is now answered before committing to
the rest.

### What a full 2D GPU pipeline would cost and buy

The performance case is not in doubt. `tools/gl-pipeline-probe.mjs` models a
Cavern-shaped frame (34 textured sprite quads + 36 vector draws, each with
its own uniforms) fully on the GPU: **0.11 ms against 5.50 ms software, ~49x**.
Per-draw overhead is not a blocker either -- gl calls issued from wasm cost
about **0.002 ms** each and account for 12% of a GL frame; the other 88% is
the software rasterizer that the rewrite would delete. (An earlier reading of
"64 us per gl call" was an artifact of dividing whole-frame time by call
count, and is wrong.)

The blocker is **bit-exactness of blended primitives**, and it is specific:

- **Sprites**: exact, over all 193 swept sizes (above).
- **Opaque primitives**: exact, once colours are passed as 0..255 integers
  rather than floats. A float uniform of `0.35` is not the same value as
  `89/255`, which alone put every pixel off by one.
- **Blended primitives**: **cannot** match with fixed-function blending. The
  GPU computes `src*a + dst*(1-a)` in normalized floats and rounds at 8 bit;
  `blend_span` uses the exact `div255` multiply-shift. Those disagree on
  **39.7% of (alpha, src, dst) combinations**, always by exactly 1.

A shader doing the integer math itself reproduces `div255` exactly (0
disagreements over the same sweep), but that means no fixed-function blending
and a **destination read per draw** -- `EXT_shader_framebuffer_fetch` where
available, a ping-pong FBO where not. That is the real scope of step 2, and
it is a substantially different engine, not a port of the existing one.

So the honest trade is: ~49x on sprite-bound carts, in exchange for either
giving up bit-exact blending as a contract (the `prims` golden becomes
tolerance-based, ±1 per channel) or carrying a destination-read blending path
on every platform. That is a product decision about what the engine promises,
not a performance one.

### Prior art: wasmcart-mruby's GL2D backend

The sibling `wasmcart-mruby` runtime already shipped this (`runtime/
render2d_gl.c`, ~500 lines), which settles both open questions empirically:

**It took the tolerance route.** It uses ordinary fixed-function blending
(`glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`), and its A/B checker
compares with a tolerance of 8/255. Re-running that comparison at ZERO
tolerance on Tetris: **~19% of pixels differ by exactly 1**, max delta 3,
nothing above 8. So "pixel-equivalent" there means visually identical, not
bit-identical -- exactly the `div255`-vs-float-rounding gap measured above,
confirmed on a shipping implementation rather than in a probe.

**It solves the mixing problem by not mixing.** `wy_r2d_disable()` makes the
CPU fallback **whole-frame and sticky**: any unsupported operation (a render
target, a TTF label, an `@rt:` sprite, a Ruby error) drops that entire frame
to the software rasterizer, which is then presented with `wc_gl_blit`. There
is no per-draw reconciliation, so the ~0.19 ms readback/upload cost never
lands in the inner loop. That is a much better answer than the per-draw
synchronisation this project was sizing, and it is the design to copy.

Measured here on Tetris (1500 frames, this machine): **GL 0.377 ms vs CPU
1.157 ms, 3.1x** -- consistent with the 2-14x range that backend reports, and
well short of the 49x an idealised probe suggests, because a real cart is not
purely fill-bound.

## GL2D (the default)

`runtime/build.sh` produces two artifacts:

| artifact | what it is |
|---|---|
| `build/engine.wasm` | **the engine.** GL2D + the software rasterizer, which still renders every frame GL2D cannot. This is what `template/main.wasm` ships. |
| `build/engine-cpu.wasm` | software only, imports nothing from `gl`. The comparator the GL build is diffed against, and the artifact for a host with no GL. |

The goldens (`test/blit`, `test/prims`, `test/render-hash.js`) run against the
**CPU** build on purpose: they assert bit-equality, which is a property of the
software rasterizer. Running them against GL would either fail or force them
to be loosened into meaninglessness. GL is gated separately and by tolerance,
in the `gl2d*` carts.

`build/engine.wasm` renders through a real WebGL2 context: one shared
2048x2048 atlas, batched solid and textured quads (a whole frame of sprites
is one draw call), cached GL state, indices uploaded once. Built on the model
`wasmcart-mruby` shipped first.

**It is deliberately not bit-exact.** Fixed-function blending rounds
differently from `div255` -- by exactly 1 per blended draw -- and that
*compounds* where draws overlap: four stacked alpha layers measure a delta of
2. The gate is therefore a tolerance (`tools/gl2d-compare.mjs`, ±2), not
equality. The software rasterizer stays the reference implementation and
stays bit-exact; `test/blit` and `test/prims` are unchanged and still assert
equality against it.

Rotation, flips and scissor are on the GL path. Rotation costs nothing extra:
`draw_image` already computes the four transformed destination corners, and
those are handed straight to GL, so there is no second implementation of the
transform to disagree with the first. Scissor is `glScissor`, which clips the
same half-open rect the software path does.

Rotation adds a THIRD kind of difference, budgeted separately from blend
rounding. A GPU decides pixel coverage by rasterizing triangles; the software
path inverse-transforms each pixel. Those agree on a quad's interior and
disagree on its boundary, so a few edge pixels are not "off by 2" -- they are
either the sprite or the background. Measured on `test/gl2d`, a 45-degree
sprite differs on **102 pixels (0.011% of the frame), 0% of its interior and
about 27% of its perimeter**. The gate budgets that at 0.05% of the frame; a
control that mirrors the sprite UVs fails it at 20.7%, so the budget is far
too small to hide a real bug.

Render targets are FBOs. A canvas gets its own texture with a framebuffer
attached rather than an atlas slot -- the atlas is one shared texture, so
rendering into a sub-rect of it would let one canvas's draws land on
another's pixels. Drawing a canvas afterwards samples that texture, keyed on
the same RGBA pointer sprites use.

Text is on GL too. `stb_truetype` already bakes every glyph into one 8-bit
coverage bitmap at font-load time, so nothing needs CPU rasterizing: the
bitmap uploads once as a single-channel texture and each glyph is a quad in
the shared batch. The shader has a third mode that multiplies only *alpha* by
the texture's red channel, matching `blend_px(cov * a / 255)`. `TEXTURE_SWIZZLE`
would have been neater but it is GL ES 3.0 only and **not** part of WebGL2,
which is the surface wasmcart specifies -- relying on it made glyphs read
`(cov, 0, 0, 1)` and text came out red. Bitfont text needs nothing special:
its pixels are `fill_rect`s that already batch.

Additive blending is `GL_SRC_ALPHA / GL_ONE`, with destination alpha left
alone so the framebuffer and canvases stay opaque exactly as `blend_px`
leaves them. It gets a **wider tolerance (±8)** than everything else, for a
reason worth stating: alpha blending *converges* -- the destination term
decays by `(1-a)` each step, so old rounding error fades -- while additive
*accumulates*. Eight stacked draws measured a drift of 8.

And the drift is not GL being sloppy. Against the exact real-valued result,
the GPU was **closer** (err 3.2) than the software path (err 4.8), because
`div255` truncates every additive step. The wider budget is tolerating the
software rasterizer's error as much as the GPU's.

**The CPU fallback is whole-frame and sticky.** What GL2D still does not
implement -- polygons, circles, the Lua error screen -- calls
`wcl_r2d_disable()`,
and from then on every frame is rasterized in software and presented with one
`wc_gl_blit`. Reconciling per draw would cost ~0.19 ms each way, which a real
frame pays dozens of times. Cavern uses canvases, so it takes this path and
is bit-identical to the CPU build.

Measured (400-600 frames each):

| cart | speedup |
|---|---|
| `test/gl2dcanvas` (render targets) | **113x** |
| **the Cavern port** | **49.5x** |
| `test/gl2d` (sprites, rotation, scissor) | **18x** |
| `test/gl2dtext` (text-only) | **2.9x** |

Cavern is the one that matters: it was the motivating case, it uses canvases
*and* TTF, and it ran entirely on the software rasterizer until both landed.
The text-only cart is lowest because a frame of nothing but text is cheap in
software too, so the fixed per-frame GL cost is a larger share.

Circles are still a deliberate fallback rather than an approximation: a GL
triangle fan does not reproduce the software span fill, and a circle is
nearly all boundary at small radii.

Sloped lines stay a known gap. GL rasterizes lines by its own
implementation-defined diamond-exit rule, which does not have to match
Bresenham, and a shallow diagonal came out a row off. Axis-aligned lines are
exact; if a cart needs Bresenham-exact diagonals it should not use GL2D.

One consequence worth knowing: `setCanvas` trips the fallback the moment it
is called, *including in `love.load`*. A cart that prepares its art in a
canvas at load time never reaches the GL path, which is why `test/gl2d/`
loads a PNG instead. That cart exists precisely because a cart which trips
the fallback measures the software path on **both** engines and reports a
perfect match while proving nothing -- `test/prims` does exactly that.

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
