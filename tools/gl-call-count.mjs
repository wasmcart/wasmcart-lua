#!/usr/bin/env node
/*
 * tools/gl-call-count.mjs - how many GL calls does a frame actually make?
 *
 * Written to hold one specific line: adding custom shaders must cost NOTHING
 * on the default path. "It should be free" is an assertion; this is a
 * measurement. A cart that never calls setShader must issue ZERO
 * glUseProgram per frame, and no extra uniform uploads either -- if a future
 * change starts re-binding the program per draw or per batch, the number
 * moves and the gate in test/run.js catches it.
 *
 * It proxies the host's real GL import table, so it counts the calls a host
 * would actually receive rather than a model of them.
 *
 *   node tools/gl-call-count.mjs build/engine.wasm test/gl2d [frames]
 *   node tools/gl-call-count.mjs build/engine.wasm test/gl2d 10 --max-useprogram 0
 */
import fs from 'fs';
import path from 'path';

const W = 1280, H = 720;
const args = process.argv.slice(2);
const flagIdx = args.indexOf('--max-useprogram');
const MAX_USE = flagIdx >= 0 ? +args[flagIdx + 1] : null;
const positional = args.filter((a, i) => i !== flagIdx && i !== flagIdx + 1);

const enginePath = positional[0] || 'build/engine.wasm';
const cartDir = positional[1] || 'test/gl2d';
const FRAMES = +(positional[2] || 10);

const WEBGL_NODE = process.env.WEBGL_NODE
  || `${process.env.HOME}/code/cliemu/romdev/node_modules/webgl-node/index.mjs`;
const WASMCART_SRC = process.env.WASMCART_SRC
  || `${process.env.HOME}/code/cliemu/wasmcart/src/webgl_imports.js`;

const wn = await import(WEBGL_NODE);
const { gl } = wn.createWebGL2Context(W, H);
const { createWebGLImports } = await import(WASMCART_SRC);

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

const appDir = fs.existsSync(path.join(cartDir, 'app'))
  ? path.join(cartDir, 'app') : cartDir;
const assets = loadAssets(appDir);

let mem;
const dec = new TextDecoder();
const env = {
  wc_log: () => {},
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
  wc_pad_has_rumble: () => 0,
  wc_pad_rumble: () => {},
  wc_pad_rumble_stop: () => {},
  wc_debug_mark: () => {},
  emscripten_notify_memory_growth: () => {},
};

const raw = createWebGLImports({ getMemory: () => mem, ctx: gl });
const counts = {};
const glImports = new Proxy(raw, {
  get: (t, k) => {
    const f = t[k];
    if (typeof f !== 'function') return f;
    return (...a) => { counts[k] = (counts[k] || 0) + 1; return f(...a); };
  },
});

const { instance } = await WebAssembly.instantiate(fs.readFileSync(enginePath),
  { env, gl: glImports, wasi_snapshot_preview1: new Proxy({}, { get: () => () => 0 }) });
const e = instance.exports;
mem = e.memory;
e.wc_get_info();
e.wc_set_seed(1);
e.wc_init();

// Warm-up frames are discarded: the first frames upload the atlas and
// compile shaders, which is startup cost, not per-frame cost.
for (let i = 0; i < 3; i++) e.wc_render();
for (const k of Object.keys(counts)) delete counts[k];
for (let i = 0; i < FRAMES; i++) e.wc_render();

const total = Object.values(counts).reduce((a, b) => a + b, 0);
console.log(`${cartDir}: GL calls per frame, averaged over ${FRAMES} frames`);
for (const [k, v] of Object.entries(counts).sort((a, b) => b[1] - a[1]))
  console.log(`  ${k.padEnd(28)} ${(v / FRAMES).toFixed(1)}`);
console.log(`  ${'TOTAL'.padEnd(28)} ${(total / FRAMES).toFixed(1)}`);

const usePerFrame = (counts.glUseProgram || 0) / FRAMES;
console.log(`glUseProgram per frame: ${usePerFrame.toFixed(2)}`);
if (MAX_USE !== null && usePerFrame > MAX_USE) {
  console.log(`\nFAILED: ${usePerFrame.toFixed(2)} glUseProgram per frame exceeds the ` +
              `budget of ${MAX_USE}. The default (no custom shader) path must not ` +
              're-bind the program.');
  process.exit(1);
}
console.log('\nOK');
