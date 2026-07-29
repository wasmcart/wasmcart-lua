# Examples

Eight carts, each exercising a different part of the engine. All are
verified rendering (not just "runs without crashing").

| example | what it demonstrates |
|---|---|
| `pong` | two-player pads, rectangles, deterministic RNG, beeps |
| `breakout` | save data (persisted high score), game states, brick collision |
| `platformer` | `require` of a lib, tile collision, gravity, scrolling camera |
| `shmup` | coroutine-scripted waves, entity pools, additive blending |
| `particles` | 900 particles, canvases, blend modes, stress test |
| `kitchen-sink` | every v1 API in one screen; the contract example |
| `shaders` | `newShader` / `setShader` / `send`; split-screen so a wrong result is obvious |
| `mesh` | `newMesh`; each panel sits next to a reference it must match, so a wrong uv remap is visible |

`shaders` and `mesh` are the two carts here that **need a GL host**: a shader
is a GPU program and a mesh is GPU geometry, so `newShader` and `newMesh`
refuse rather than pretending on a host with no GL context. `test/run.js`
therefore gates both against a real WebGL2 context instead of running them on
the CPU comparator like the rest.

Run one:

```bash
cd examples/pong
cp ../../build/engine.wasm main.wasm
npx wasmcart .
```

Or pack it:

```bash
npx wasmcart pack --wasm ../../build/engine.wasm --assets app \
  --name pong --width 1280 --height 720 -o pong.wasc
```
