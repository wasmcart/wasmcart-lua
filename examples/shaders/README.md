# shaders

`love.graphics.newShader` / `setShader` / `Shader:send` on the GL2D renderer.

The screen is split down the middle and the **same** scene is drawn on both
halves, with a colour-inverting shader bound for the right one only. If the
shader ran, the halves are photographic negatives of each other. If it
silently did not — link failure, program never bound, a batch drawn under
the wrong program — the halves come out **identical**, which is the whole
reason for the layout. A frame counter cannot tell those two apart.

Both halves draw a rectangle grid, a circle, a sprite and a line of text, so
the shader is proved on every batch the renderer has: the solid batch, the
fragment-evaluated circle rule, the atlas texture batch, and the glyph
coverage batch. A shader that only reached textured draws would show up as
two halves that agree on the rectangles.

The bar across the bottom is a second shader driven by uniforms sent from
Lua every frame (`u_time`, `u_tint`), because a static inversion never
exercises `send`.

```bash
node ../../../wasmcart/bin/wasmcart.js . --frames 60 --shot out.png
```

Automated: `node tools/gl-shader-verify.mjs build/engine.wasm examples/shaders`
probes four points on both halves and asserts they are the **true inverse**,
not merely different. `test/run.js` runs that, plus a control copy with the
shader deliberately unbound that must fail, plus `test/shaderfail` for the
refusal path.

## What writing this forced into the engine

Every row was found by the screenshot being wrong, not by reasoning:

| Gap | Fix |
|---|---|
| One global program, one VAO | attribute locations pinned with `glBindAttribLocation` before every link, so a cart shader's linker cannot assign `a_uv` the index the VAO records for `a_pos` |
| `u_textured` cached globally | uniform values live in the *program* object, so the cache is invalidated on every program switch and `set_textured` reads the bound program's location |
| A batch could span two programs | `setShader` flushes first, exactly like a texture change; without it the queued vertices render under whichever program happens to be bound at flush time |
| `Texel` returned a real atlas texel for untextured draws | a plain rectangle sampled atlas texel (0,0) — opaque black — so every solid inverted to **white** instead of to its own negative. Visibly wrong while still looking like "the shader ran". `Texel` now honours `u_textured` and hands solids an all-white texel |
| The software fallback silently ate the shader | a bound shader cannot follow the frame onto the CPU rasterizer; `wcl_r2d_disable` now says so once in the cart log instead of rendering plausible-but-unshaded |
| `wc_gl_blit` leaves its own program bound | the cached "which program is current" was stale after any fallback frame, so the next GL frame's first draw used the blit's program |
| Driver errors pointed at generated lines | a `#line 1` after the synthesized preamble, so a syntax error reports the cart's own line number |
| Desktop GLSL reached the driver | `#version`, `gl_FragColor`, `texture2D`, `varying`, `attribute` and GLES 3.1+ constructs are refused **by name**, with what to use instead |
