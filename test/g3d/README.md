# test/g3d - the 3D conformance cart

This cart exists to answer one question: **does an unmodified third-party
LOVE 3D library run on this engine?**

`app/g3d/` is [groverburger's g3d](https://github.com/groverburger/g3d)
v1.6.0, copied **verbatim** from upstream. Not adapted, not ported, not
patched. If a single file here differs from upstream, the test has stopped
testing what it claims to.

    upstream: https://github.com/groverburger/g3d
    commit:   639120a
    license:  MIT (see the header in each file, and app/g3d/init.lua)

## Why a whole library instead of a unit test

The engine's 3D support is not one feature, it is a contract made of parts
that only matter together:

| what g3d uses | what it exercises |
|---|---|
| `newMesh(vertexformat, ...)` with a 3-component `VertexPosition` | the custom-vertex-format path and the 3D vertex layout |
| `VertexNormal` in the format and the shader | the 3D attribute that has no 2D equivalent |
| `love.graphics.setDepthMode("lequal", true)` | the depth buffer and depth state |
| a vertex shader in GLSL ES 1.00 spellings (`attribute`, `varying`) | the shader rewriter |
| `shader:send("projectionMatrix", m)` with a flat 16-number table | mat4 uniforms, and the row-major transpose |
| `package.loaded[...] = g3d` | the module self-registration idiom |
| `love.graphics.getCanvas()` | render-target introspection |

A unit test for any one of these passes while the others are broken. A real
library only runs if all of them are right, which is why the gate is a
library and not a checklist.

## Running it

    node tools/gl-3d-verify.mjs build/engine.wasm test/g3d

The verifier asserts three things, and explains each failure in terms of the
bug that causes it:

1. **geometry exists** - catches the whole class of "the draw executed with
   no GL error and rasterized nothing", which is what a transposed matrix or
   a degenerate view matrix produce.
2. **the silhouette is a projected solid**, not a flat quad - catches a
   projection that never reached the vertices.
3. **depth actually occludes** - re-renders the same cart with a `NODEPTH`
   asset that disables the depth test, and requires the two frames to differ.
   Without this, assertion 1 passing would only mean triangles were drawn in
   submission order.

`test/run.js` runs the gate plus a control with the 3D draws removed, which
must fail. A gate that has never been seen red is not a gate.

## The two traps this cart encodes

Both cost real debugging time and both are silent - no error, no warning,
just an empty screen:

- **Matrices are row-major.** LOVE takes them that way and every LOVE
  library writes them that way; GL reads a flat array as column-major. The
  upload transposes. Get this wrong and the draw is flawless and invisible.
- **`camera.lookAt` down the Z axis is degenerate.** g3d's default up vector
  is `{0,0,1}`, so an eye position of `(0,0,4)` looking at the origin makes
  `up` parallel to the view direction; `cross(up, z)` is zero and the view
  matrix comes out with two all-zero rows. Any off-axis eye avoids it. This
  is g3d behaving correctly, not an engine bug - which is exactly why it
  wasted time.
