# mesh

`love.graphics.newMesh` on the GL2D renderer: arbitrary textured,
per-vertex-coloured triangles.

Every panel here exists so that a WRONG result is **visible**, not merely
absent. A mesh that fails to draw leaves the frame count perfect and a hole
in the screen, and the blank-frame check in `test/run.js` cannot see it
because the cart draws plenty of text and sprites besides. So each panel is
placed next to something that must match it, or must contrast with it, in a
specific way.

| panel | what a wrong result looks like |
|---|---|
| 1 textured mesh, with **the same image drawn as a sprite** directly beneath | the two disagree. Sprites live in a shared 2048² atlas, so a mesh's 0..1 uv has to be remapped into that sub-rect; a wrong offset shows a garbled crop, or `decoy.png`, which is loaded into the atlas first and is magenta/cyan stripes precisely so a stale offset is unmistakable |
| 2 per-vertex colour, no texture | a flat block instead of a bilinear blend, which is what you get if the colour attribute never reaches the fragment stage |
| 3 `"triangles"` and `"fan"` built from **the same six vertices** | the two panels look identical, which is what a draw mode that silently defaults to `"fan"` produces. In `"triangles"` mode the vertices are two disjoint triangles with a gap; in `"fan"` mode one connected fan |
| 4 uv `0..0.5` through a vertex map | anything other than the texture's red top-left quadrant. Sampling the whole atlas instead puts most of a 2048px texture in the panel and shows almost nothing recognisable |
| 5 rotated, scaled and tinted | the mesh ignores the transform stack, or `setColor` does not modulate it |
| 6 `"strip"` mesh with a per-vertex alpha ramp | a uniform bar, or reversed winding |

`tex.png` is built to be diagnostic rather than pretty: four strongly
distinct quadrant colours, a white border, an asymmetric black L bracket and
a diagonal. No flip, 90° rotation, shear or offset of the uv leaves it
looking correct.

```bash
node ../../../wasmcart/bin/wasmcart.js . --frames 60 --shot out.png
```

Automated: `node tools/gl-mesh-verify.mjs build/engine.wasm examples/mesh`
probes the panels above and asserts them by value; the textured mesh must
agree with its sprite reference to within ±2, the documented blend-rounding
budget. In practice it agrees **exactly** (maxdiff 0 on every probe), because
both paths end up sampling the same atlas texels under `NEAREST`.

`test/run.js` runs that, plus a control copy with the mesh draw removed that
must fail, plus `test/meshfail` for the refusal paths and the 1-based index
semantics, plus `test/meshcost` for the GL call budget.

## What writing this forced into the engine

| Gap | Fix |
|---|---|
| The batcher is hardwired to quads (`glDrawElements(GL_TRIANGLES, (count/4)*6, ...)` against a static quad index buffer) | a mesh cannot ride it, so `wcl_r2d_mesh` flushes and issues its own `glDrawArrays`, the shape the polygon fill already used, minus the 64-triangle cap and plus texturing |
| `newMesh` with a vertex **count** | `calloc` gives transparent black, so a mesh built from positions alone rendered nothing. Vertices are seeded to opaque white, which is LÖVE's default |
| Vertices in a Lua table | a mesh is redrawn every frame and its vertices rarely change, so marshalling them across the boundary each frame would have cost more than the draw. They live in C, and the draw mode and vertex map are expanded there too |
| A `Canvas` as a mesh texture | it is its own texture, not an atlas slot, so it needs no remap, only the V flip an FBO's bottom-left origin requires |
