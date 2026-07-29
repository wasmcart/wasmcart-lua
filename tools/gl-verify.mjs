#!/usr/bin/env node
/*
 * tools/gl-verify.mjs - does the GL display path show the SAME pixels?
 *
 * The GL variant still rasterizes in software; wc_gl_blit only changes how
 * the finished pixels reach the screen. So the GPU's output must match the
 * cart's own framebuffer exactly, or the display path is lying about what
 * the cart drew.
 *
 * This runs the cart against a REAL WebGL2 context (romdev's offscreen
 * native-gles one), then reads the GPU's framebuffer back and diffs it
 * against the cart's software framebuffer, pixel for pixel.
 *
 *   node tools/gl-verify.mjs build/engine.wasm test/prims
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

// the cart's own software framebuffer (XRGB: 0x00RRGGBB)
const [, iw, ih, fb] = new Uint32Array(mem.buffer, info, 4);
const soft = new Uint32Array(mem.buffer.slice(fb, fb + iw * ih * 4));

// what the GPU actually shows
gl.finish();
const gpu = new Uint8Array(W * H * 4);
gl.readPixels(0, 0, W, H, gl.RGBA, gl.UNSIGNED_BYTE, gpu);

let diff = 0, firstBad = null, lit = 0;
for (let y = 0; y < ih; y++) {
  for (let x = 0; x < iw; x++) {
    const s = soft[y * iw + x];
    const sr = (s >> 16) & 255, sg = (s >> 8) & 255, sb = s & 255;
    if (sr | sg | sb) lit++;
    // readPixels' origin is BOTTOM-left while the framebuffer's is top-left.
    // The blit shader flips V when sampling the texture, so what lands on
    // screen is upright; reading it back therefore returns it bottom-up and
    // row y of the image is row (H-1-y) of the readback. Comparing without
    // this flip reports ~55% difference on a correct blit.
    const o = ((H - 1 - y) * W + x) * 4;
    if (gpu[o] !== sr || gpu[o + 1] !== sg || gpu[o + 2] !== sb) {
      if (!diff) firstBad = { x, y, soft: [sr, sg, sb], gpu: [gpu[o], gpu[o+1], gpu[o+2]] };
      diff++;
    }
  }
}
const total = iw * ih;
console.log(`software framebuffer: ${lit} of ${total} pixels non-black`);
console.log(`GPU vs software     : ${diff} differing pixels (${(diff/total*100).toFixed(3)}%)`);
if (firstBad) console.log('  first difference:', JSON.stringify(firstBad));
if (lit === 0) { console.log('\nFAILED: the cart drew nothing; comparison is meaningless'); process.exit(1); }
if (diff !== 0) { console.log('\nFAILED: the GL display path does not show what the cart drew'); process.exit(1); }
console.log('\nOK: GPU output is pixel-identical to the software framebuffer');
