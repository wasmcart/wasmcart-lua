#!/usr/bin/env node
/*
 * gl-samplerupdate-verify.mjs - does a shader sampler SEE setPixel writes?
 *
 * An ImageData allocated with newImageData(w,h) and then painted with
 * setPixel sampled as solid WHITE forever when sent to a shader. Shader
 * sampler textures live in a SECOND pointer-keyed cache (sampler_texs) and
 * wcl_r2d_forget cleared only the draw-path cache, so the first upload --
 * of a freshly allocated, still-blank image -- was cached and never
 * replaced.
 *
 * The cart paints an 8x8 ImageData RED after allocation and draws a
 * full-screen quad sampling it through a shader. PASS = a red screen.
 */
import fs from 'fs';
import path from 'path';

const WEBGL_NODE = process.env.WEBGL_NODE
  || `${process.env.HOME}/code/cliemu/romdev/node_modules/webgl-node/index.mjs`;

const W = 1280, H = 720;
const enginePath = process.argv[2] || 'build/engine.wasm';
const cartDir = process.argv[3] || 'test/prims';
const FRAMES = +(process.argv[4] || 3);

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

// The host's own GL import layer, so this verifies the REAL path a host
// uses rather than a reimplementation of it.
const WASMCART_SRC = process.env.WASMCART_SRC
  || `${process.env.HOME}/code/cliemu/wasmcart/src/webgl_imports.js`;
const { createWebGLImports } = await import(WASMCART_SRC);

const wn = await import(WEBGL_NODE);
const { gl } = wn.createWebGL2Context(W, H);
console.log('renderer:', gl.getParameter(gl.RENDERER));

const assets = loadAssets(cartDir);
let mem;
const dec = new TextDecoder();
const env = {
  wc_log: (p, l) => console.log('  LOG:', dec.decode(new Uint8Array(mem.buffer, p, l))),
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
  // Offline peer host: nothing here is a networking test, and a cart
  // that cannot boot without the network is the bug these would catch.
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
const info = e.wc_get_info();
e.wc_set_seed(1);
e.wc_init();
for (let i = 0; i < FRAMES; i++) e.wc_render();

// What the GPU actually shows. The assertion is on PIXELS because every
// intermediate signal in this bug looked healthy: the ImageData held the
// right bytes, the UVs were right, the sampler uniform was bound and sent.
// Only the screen was wrong.
gl.finish();
const gpu = new Uint8Array(W * H * 4);
gl.readPixels(0, 0, W, H, gl.RGBA, gl.UNSIGNED_BYTE, gpu);

let red = 0, white = 0, other = 0;
for (let i = 0; i < gpu.length; i += 4) {
  const r = gpu[i], g = gpu[i + 1], b = gpu[i + 2];
  if (b > 200 && r < 80 && g < 80) red++;   // phase 2 repaints to BLUE
  else if (r > 200 && g > 200 && b > 200) white++;
  else other++;
}
const total = W * H;
console.log(`red=${red} white=${white} other=${other} of ${total}`);

if (red < total * 0.8) {
  console.error(`FAIL: expected a BLUE screen after the repaint, got ${red}/${total} red and ` +
                `${white} white -- the shader sampler is showing a STALE ` +
                `texture rather than the setPixel writes.`);
  process.exit(1);
}
console.log('\nOK: setPixel writes reach the shader sampler');
