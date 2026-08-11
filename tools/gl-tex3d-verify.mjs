#!/usr/bin/env node
/*
 * tools/gl-tex3d-verify.mjs - does TEXTURE MAPPING work on a 3D MODEL?
 *
 * A textured quad proves almost nothing: facing the camera, linear and
 * perspective-correct interpolation agree, there is no minification, and
 * the uv layout is one flat rect. Every hard part of texturing is absent.
 *
 * test/tex3d draws 3DreamEngine's Suzanne -- a curved surface with a real
 * unwrap and 556 uv coordinates from its own .obj -- wearing a fine grid,
 * viewed from an ANGLE so perspective error cannot hide.
 *
 *  1. THE TEXTURE IS SAMPLED ACROSS ITS AREA. Count distinct texel shades
 *     on the model. The failure this catches is the one that looks most
 *     like success: a mapping that samples ONE texel per triangle renders a
 *     clean, flat-shaded model that a human glances past. Real mapping over
 *     a grid gives dozens of shades.
 *
 *  2. THE PATTERN REPEATS ACROSS THE SURFACE. Count checker transitions
 *     along a scanline over the skull. A model showing its texture stretched
 *     to a single cell has zero transitions while still being "textured".
 *
 *  3. THE MODEL IS STILL A MODEL. Silhouette width varies, so assertions 1
 *     and 2 are being made about actual geometry and not a full-screen
 *     accident.
 *
 *   node tools/gl-tex3d-verify.mjs build/engine.wasm test/tex3d
 */
import fs from 'fs';
import path from 'path';

const WEBGL_NODE = process.env.WEBGL_NODE
  || `${process.env.HOME}/code/cliemu/romdev/node_modules/webgl-node/index.mjs`;
const WASMCART_SRC = process.env.WASMCART_SRC
  || `${process.env.HOME}/code/cliemu/wasmcart/src/webgl_imports.js`;

const W = 1280, H = 720;
const argv = process.argv.slice(2).filter(a => !a.startsWith('--'));
const LOGS_ONLY = process.argv.includes('--logs');
const enginePath = argv[0] || 'build/engine.wasm';
const cartDir = argv[1] || 'test/tex3d';
const FRAMES = +(argv[2] || 15);

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

const { createWebGLImports } = await import(WASMCART_SRC);
const wn = await import(WEBGL_NODE);
const { gl } = wn.createWebGL2Context(W, H);

const appDir = fs.existsSync(path.join(cartDir, 'app'))
  ? path.join(cartDir, 'app') : cartDir;
const assets = loadAssets(appDir);
let mem;
const dec = new TextDecoder();
const logs = [];
const env = {
  wc_log: (p, l) => logs.push(dec.decode(new Uint8Array(mem.buffer, p, l))),
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
  wc_peer_open: () => -1,
  wc_peer_close: () => {},
  wc_peer_send: () => -1,
  wc_peer_broadcast: () => 0,
  wc_peer_state: () => 3,
  wc_peer_count: () => 0,
  wc_peer_id: () => -1,
  wc_peer_name: () => -1,
  wc_peer_transport: () => 0,
  wc_debug_mark: () => {},
  emscripten_notify_memory_growth: () => {},
};

const glImports = createWebGLImports({ getMemory: () => mem, ctx: gl });
const { instance } = await WebAssembly.instantiate(fs.readFileSync(enginePath), {
  env, gl: glImports,
  wasi_snapshot_preview1: new Proxy({}, { get: () => () => 0 }),
});
const e = instance.exports;
mem = e.memory;
e.wc_get_info();
e.wc_set_seed(1);
e.wc_init();
for (let i = 0; i < FRAMES; i++) e.wc_render();

if (LOGS_ONLY) {
  for (const l of logs) console.log('LOG: ' + l);
  process.exit(0);
}

gl.finish();
const gpu = new Uint8Array(W * H * 4);
gl.readPixels(0, 0, W, H, gl.RGBA, gl.UNSIGNED_BYTE, gpu);
const px = (x, y) => {
  const o = ((H - 1 - y) * W + x) * 4;
  return [gpu[o], gpu[o + 1], gpu[o + 2]];
};

const problems = [];
// A Lua error means the pipeline never got as far as drawing anything.
for (const l of logs) {
  if (/lua error|failed to compile|did not compile/i.test(l)) {
    problems.push('cart log: ' + l.split('\n')[0]);
  }
}

// The cart clears to (0.04, 0.05, 0.09).
const isModel = (p) =>
  Math.abs(p[0] - 10) + Math.abs(p[1] - 13) + Math.abs(p[2] - 23) > 60;

console.log('1. the texture is sampled across its area:');
const buckets = new Set();
let count = 0;
const widths = [];
for (let y = 0; y < H; y++) {
  let lo = -1, hi = -1;
  for (let x = 0; x < W; x++) {
    const p = px(x, y);
    if (!isModel(p)) continue;
    count++;
    if (lo < 0) lo = x;
    hi = x;
    if ((x % 2) === 0 && (y % 2) === 0) {
      buckets.add(`${p[0] >> 5},${p[1] >> 5},${p[2] >> 5}`);
    }
  }
  if (lo >= 0) widths.push(hi - lo + 1);
}
const MIN_SHADES = 20;
console.log(`   ${buckets.size >= MIN_SHADES ? 'ok  ' : 'BAD '} distinct texel ` +
            `shades: ${buckets.size} over ${count} model px (need >= ${MIN_SHADES})`);
if (count < 3000) {
  problems.push(`only ${count} model pixels -- the model did not render, so ` +
                'nothing can be said about its texture.');
} else if (buckets.size < MIN_SHADES) {
  problems.push(
    `the model shows ${buckets.size} texel shade(s). The texture is bound but ` +
    'not being sampled across its area -- a uv that collapses to one texel ' +
    'per triangle renders a clean flat-shaded model, which is why this needs ' +
    'a pixel count rather than a look.');
}

console.log('2. the pattern repeats across the surface:');
let best = 0;
for (const y of [280, 300, 320, 340]) {
  let trans = 0, prev = null;
  for (let x = 0; x < W; x++) {
    const p = px(x, y);
    if (!isModel(p)) continue;
    const cur = p[0] > p[1] + 40;      // magenta vs cream
    if (prev !== null && cur !== prev) trans++;
    prev = cur;
  }
  if (trans > best) best = trans;
}
console.log(`   ${best >= 4 ? 'ok  ' : 'BAD '} checker transitions on the best ` +
            `scanline: ${best} (need >= 4)`);
if (best < 4) {
  problems.push(
    `at most ${best} checker transitions across the model. The texture is ` +
    'stretched rather than mapped -- uv is reaching the shader with a ' +
    'near-constant value.');
}

console.log('3. the assertions above are about a model, not a full-screen fill:');
if (widths.length < 40) {
  console.log(`   BAD  only ${widths.length} rows covered`);
  problems.push(`the model covers ${widths.length} rows; too little to judge.`);
} else {
  const wMin = Math.min(...widths), wMax = Math.max(...widths);
  const ratio = wMax / wMin;
  const ok = ratio >= 3;
  console.log(`   ${ok ? 'ok  ' : 'BAD '} silhouette width ${wMin}..${wMax} px ` +
              `(ratio ${ratio.toFixed(1)}, need >= 3)`);
  if (!ok) {
    problems.push(`the silhouette is a near-constant ${wMin}..${wMax} px wide, ` +
                  'so this is not a model.');
  }
}

console.log('');
if (problems.length) {
  console.log('FAIL');
  for (const p of problems) console.log('  - ' + p);
  process.exit(1);
}
console.log(`PASS  texture mapping on a 3D model verified ` +
            `(${buckets.size} texel shades, ${best} checker transitions)`);
