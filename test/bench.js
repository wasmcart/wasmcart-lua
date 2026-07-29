#!/usr/bin/env node
/*
 * test/bench.js - measure where a cart's frame time actually goes.
 *
 * The point is to decide whether a JIT is worth building. The JIT_PLAN says
 * Tier 1 must be aimed at MEASURED hot spots, and that C builtins are the
 * cheaper lever when the hot loop is not really in Lua. This tells us which
 * world we are in.
 *
 * The cart (test/bench/) runs exactly one workload per frame and names it
 * over the debug log. This host times each wc_render() with a real clock,
 * because the engine's own love.timer is frame-quantized by design and
 * cannot see inside a frame.
 *
 *   node test/bench.js
 *
 * WHAT IT FOUND (2026-07-28), and why Phase 3 has not started:
 *
 *   Lua is NOT the bottleneck in real carts. 2000 entities cost 0.17 ms
 *   (1% of a frame) and 2000 particles 0.15 ms; every example cart runs in
 *   under 4% of its frame budget. The micro benchmarks look slow only
 *   because they are deliberately interpreter-bound.
 *
 *   The one cart that IS heavy is the Cavern port at 9.4 ms (57% of a
 *   frame), and instrumenting it showed 97% of that is inside the C sprite
 *   blitter -- about 1% is Lua. A JIT would optimize the 1%.
 *
 * So the perf work went into the rasterizer, not Tier 1. That is now done:
 * draw_image is 1.65x faster and bit-identical (Cavern 9.1 -> 5.5 ms). See
 * the commit for what was safe and what was not -- in short, the divisions
 * cannot be replaced by reciprocal multiplication without moving pixels.
 *
 * Blitter changes are gated by test/blit/, NOT by determinism.js: those
 * carts draw shapes and text, never sprites, and they passed for an entire
 * session while the blitter was sampling the wrong texels.
 */
const fs = require('fs');
const path = require('path');

const ROOT = path.join(__dirname, '..');
// The CPU comparator: this bench measures the software rasterizer, and the GL
// build needs a real GL context this headless harness cannot provide.
const ENGINE = path.join(ROOT, 'build', 'engine-cpu.wasm');
const APP = path.join(ROOT, 'test', 'bench');
const FRAME_BUDGET_MS = 1000 / 60;

function loadAssets(dir, pre = '') {
  const out = {};
  for (const n of fs.readdirSync(dir)) {
    const p = path.join(dir, n);
    const rel = pre ? pre + '/' + n : n;
    if (fs.statSync(p).isDirectory()) Object.assign(out, loadAssets(p, rel));
    else out[rel] = fs.readFileSync(p);
  }
  return out;
}

async function main() {
  if (!fs.existsSync(ENGINE)) {
    console.error('engine not built: run runtime/build.sh first');
    process.exit(1);
  }
  const assets = loadAssets(APP);
  let mem;
  const dec = new TextDecoder();
  let pending = null, count = 0, done = false;

  const imports = {
    env: {
      wc_log: (p, l) => {
        const s = dec.decode(new Uint8Array(mem.buffer, p, l));
        if (s.startsWith('BENCHRUN ')) pending = s.slice(9);
        else if (s.startsWith('BENCHCOUNT ')) count = +s.slice(11);
        else if (s === 'BENCHDONE') done = true;
      },
      wc_asset_size: (p, l) => {
        const n = dec.decode(new Uint8Array(mem.buffer, p, l));
        return assets[n] ? assets[n].length : -1;
      },
      wc_load_asset: (p, l, d, m) => {
        const n = dec.decode(new Uint8Array(mem.buffer, p, l));
        if (!assets[n]) return -1;
        const b = assets[n];
        const len = Math.min(b.length, m);
        new Uint8Array(mem.buffer, d, len).set(b.subarray(0, len));
        return len;
      },
      // no pads in a headless run, so rumble is a no-op the engine can still call
      wc_pad_has_rumble: () => 0,
      wc_pad_rumble: () => {},
      wc_pad_rumble_stop: () => {},
      wc_debug_mark: () => {},
      emscripten_notify_memory_growth: () => {},
    },
    wasi_snapshot_preview1: new Proxy({}, { get: () => () => 0 }),
  };

  const { instance } = await WebAssembly.instantiate(fs.readFileSync(ENGINE), imports);
  const e = instance.exports;
  mem = e.memory;
  e.wc_get_info();
  e.wc_set_seed(1);
  e.wc_init();

  // Each frame: the cart logs the workload name during wc_render, so the
  // measured time IS that workload's frame. Repeat each a few times and
  // keep the best, to shed scheduler noise.
  const REPEATS = 5;
  const best = new Map();

  for (let rep = 0; rep < REPEATS; rep++) {
    // fresh instance per repeat so table/particle state starts identical
    const { instance: inst } = await WebAssembly.instantiate(fs.readFileSync(ENGINE), imports);
    const ex = inst.exports;
    mem = ex.memory;
    ex.wc_get_info();
    ex.wc_set_seed(1);
    ex.wc_init();
    pending = null; done = false;

    for (let f = 0; f < count + 4 && !done; f++) {
      pending = null;
      const t0 = process.hrtime.bigint();
      ex.wc_render();
      const ms = Number(process.hrtime.bigint() - t0) / 1e6;
      if (pending) {
        const prev = best.get(pending);
        if (prev === undefined || ms < prev) best.set(pending, ms);
      }
    }
  }

  const rows = [...best.entries()];
  const lua = rows.filter(([n]) => !n.startsWith('draw/'));
  const draw = rows.filter(([n]) => n.startsWith('draw/'));

  const show = (title, list) => {
    console.log('\n' + title);
    for (const [name, ms] of list) {
      const pct = (ms / FRAME_BUDGET_MS) * 100;
      const bar = '#'.repeat(Math.min(40, Math.round(pct / 2.5)));
      console.log(`  ${name.padEnd(24)} ${ms.toFixed(2).padStart(7)} ms  ` +
                  `${pct.toFixed(0).padStart(4)}% of a frame  ${bar}`);
    }
  };

  console.log(`frame budget: ${FRAME_BUDGET_MS.toFixed(2)} ms (60fps)`);
  console.log(`best of ${REPEATS} runs per workload`);
  show('LUA EXECUTION (what a JIT would speed up)', lua);
  show('RENDERING (what a JIT would NOT touch)', draw);

  const luaTotal = lua.reduce((s, [, ms]) => s + ms, 0);
  const drawTotal = draw.reduce((s, [, ms]) => s + ms, 0);
  console.log(`\n  lua total  ${luaTotal.toFixed(1)} ms`);
  console.log(`  draw total ${drawTotal.toFixed(1)} ms`);
  console.log(`  ratio      ${(luaTotal / drawTotal).toFixed(2)}x ` +
              `(>1 means Lua dominates and a JIT is worth it)`);
}

main().catch(e => { console.error(e); process.exit(1); });
