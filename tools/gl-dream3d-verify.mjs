#!/usr/bin/env node
/*
 * tools/gl-dream3d-verify.mjs - does 3DreamEngine's geometry pipeline work?
 *
 * test/dream3d runs 3DreamEngine with its library sources copied VERBATIM
 * from upstream, loads its own Suzanne .obj, and draws the resulting mesh.
 * That exercises a chain nothing else in this suite touches:
 *
 *   the ffi shim        3Dream packs its vertex buffer through ffi.cdef /
 *                       ffi.cast / struct-element writes. If that shim is
 *                       wrong, the buffer is garbage.
 *   generic formats     its vertex format is {VertexPosition float 4,
 *                       VertexTexCoord float 2, VertexNormal byte 4,
 *                       VertexTangent byte 4} -- a 4-component position and
 *                       two byte attributes, one of them a name this engine
 *                       has no built-in slot for.
 *   ByteData upload     the vertices and the index buffer both arrive as
 *                       packed bytes, not Lua tables.
 *   its .obj loader     which runs on the asset-index directory listing,
 *                       the `bit` library and love.filesystem.
 *
 * The assertions are about the RENDERED SHAPE, because every one of those
 * layers can fail into "a mesh drew, wrongly":
 *
 *  1. GEOMETRY EXISTS. A wrong stride or a mis-packed buffer usually
 *     produces nothing at all.
 *  2. IT IS A MODEL, NOT NOISE. The silhouette's width varies enormously
 *     down its height (a head is not a rectangle). A buffer read at the
 *     wrong stride draws scattered triangles that fill their bounding box
 *     evenly, so a near-constant width is the signature of exactly that.
 *  3. NORMALS ARE REAL. The shader colours by normal, so a correct model is
 *     MULTI-COLOURED. If the byte-normal attribute were dropped or read as
 *     zero, every fragment would take the same colour -- which still looks
 *     like a monkey, just flat, and is the failure a silhouette test cannot
 *     see.
 *
 *   node tools/gl-dream3d-verify.mjs build/engine.wasm test/dream3d
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
const cartDir = argv[1] || 'test/dream3d';
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

// The cart clears to a dark blue-black (0.05, 0.05, 0.12); the model is
// normal-coloured and bright.
const isModel = (p) => p[0] > 90 || p[1] > 90;

console.log('1. 3DreamEngine geometry rendered:');
let count = 0;
const widths = [];
for (let y = 0; y < H; y++) {
  let lo = -1, hi = -1;
  for (let x = 0; x < W; x++) {
    if (isModel(px(x, y))) { count++; if (lo < 0) lo = x; hi = x; }
  }
  if (lo >= 0) widths.push(hi - lo + 1);
}
const MIN_PIXELS = 8000;
console.log(`   ${count >= MIN_PIXELS ? 'ok  ' : 'BAD '} model pixels: ${count} ` +
            `(need >= ${MIN_PIXELS})`);
if (count < MIN_PIXELS) {
  problems.push(
    `only ${count} model pixels. 3DreamEngine's mesh did not render -- its ` +
    'vertex buffer is packed through the ffi shim and uploaded as raw bytes ' +
    'to a declared vertex format, so a wrong stride or a dropped attribute ' +
    'produces exactly this.');
}

console.log('2. the silhouette is a model, not evenly-scattered triangles:');
if (widths.length < 40) {
  console.log(`   BAD  only ${widths.length} rows covered`);
  problems.push(`the model covers ${widths.length} rows; too little to judge.`);
} else {
  const wMin = Math.min(...widths), wMax = Math.max(...widths);
  const ratio = wMax / wMin;
  const ok = ratio >= 3;
  console.log(`   ${ok ? 'ok  ' : 'BAD '} width varies ${wMin}..${wMax} px ` +
              `(ratio ${ratio.toFixed(1)}, need >= 3) over ${widths.length} rows`);
  if (!ok) {
    problems.push(
      `the silhouette is a near-constant ${wMin}..${wMax} px wide. Geometry ` +
      'is on screen but shapeless -- the signature of a buffer read at the ' +
      'wrong stride, which scatters triangles evenly through their bounding ' +
      'box.');
  }
}

console.log('3. normals reached the shader (the model is multi-coloured):');
// Sample the model's pixels and count distinct coarse colours. A dropped
// byte-normal attribute makes every fragment identical.
const buckets = new Set();
for (let y = 0; y < H; y += 3) {
  for (let x = 0; x < W; x += 3) {
    const p = px(x, y);
    if (!isModel(p)) continue;
    buckets.add(`${p[0] >> 5},${p[1] >> 5},${p[2] >> 5}`);
  }
}
const ok3 = buckets.size >= 6;
console.log(`   ${ok3 ? 'ok  ' : 'BAD '} distinct shades: ${buckets.size} ` +
            `(need >= 6)`);
if (!ok3) {
  problems.push(
    `the model renders in ${buckets.size} shade(s). The shader colours by ` +
    'VertexNormal, so a flat result means the byte-normal attribute never ' +
    'reached it -- the vertex format bound the wrong attribute index, or ' +
    'the byte attribute was not normalized.');
}

console.log('');
if (problems.length) {
  console.log('FAIL');
  for (const p of problems) console.log('  - ' + p);
  process.exit(1);
}
console.log(`PASS  3DreamEngine's own mesh pipeline renders (${count} px, ` +
            `${buckets.size} shades)`);
