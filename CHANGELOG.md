# Changelog

Releases before 0.7.0 are described on the
[GitHub releases page](https://github.com/wasmcart/wasmcart-lua/releases).

## 0.7.0

### dt follows the display

`love.update(dt)` runs once per frame the host shows, and `dt` used to be a
constant 1/60 however fast those frames came. On a 120 Hz display every game
ran twice as fast; on a 144 Hz one, 2.4 times.

`dt` is now the host's frame time (`wc_time.delta_ms`, in seconds), so code
that moves by `dt` runs at the same speed at any refresh rate:

- `love.update(dt)` and `love.timer.getDelta()` return the host's delta. The
  host clamps it (250 ms in the reference hosts), so a stall never becomes
  one giant step.
- `love.timer.getTime()` is the host's `time_ms` in seconds.
- `love.timer.getFPS()` counts frames over the last second instead of
  always answering 60.
- `os.clock()` matches `getTime()`, and `os.time()` is a fixed epoch plus
  whole seconds of it (it used to count frames, 60 "seconds" per second).
- The virtual cursor driven by the right stick moves by `dt` too.

Deterministic replay is unchanged in kind: under `--seed` (or
`load(cart, {deterministic: {seed}})`) the host hands every frame the same
fixed step, 16.667 ms unless the harness picks another, and runs stay
bit-identical. A host that never fills `wc_time` gets the old 1/60 step
rather than a zero `dt`.

A game that moves by per-frame constants (`x = x + 6`) still runs faster on
a faster display. The examples and the template now scale their movement,
timers and spawn rates by `dt`; the porting guide shows how.

### wasmcart 0.41.1, ABI 4

- `runtime/wasmcart.h` is synced to wasmcart 0.41.1 and every manifest
  declares `"abi": 4`. Current wasmcart hosts refuse ABI 3 carts, so 0.6.x
  carts no longer load on them; rebuild against this engine.
- The `wasmcart` dev dependency is `^0.41.1`.
- The lighten/darken blend equations (`GL_MIN`/`GL_MAX`) are defined in
  the renderer, since the spec header does not carry them.

### Tests

- `test/clock.mjs` (also in `npm test`): a deterministic run, a second
  deterministic run that must match it, a 120 Hz step, and a real-clock run
  with 40 ms between frames. `--prelude <file>` swaps in another prelude as
  a must-fail control; the 0.6.2 prelude fails four checks.
