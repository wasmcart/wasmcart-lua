#!/usr/bin/env node
/*
 * tools/gl2d-compare.mjs - GL2D vs the software rasterizer, in lockstep.
 *
 * GL2D is NOT bit-exact by design (see docs/architecture.md): fixed-function
 * blending rounds differently from div255, always by 1. This runs a cart on
 * both backends with the same seed and reports the FULL delta histogram, so
 * "within tolerance" is a measurement rather than an assertion.
 *
 *   node tools/gl2d-compare.mjs <cart-dir> [frames] [tolerance]
 */
import fs from 'fs';
import path from 'path';

const WEBGL_NODE = process.env.WEBGL_NODE
  || `${process.env.HOME}/code/cliemu/romdev/node_modules/webgl-node/index.mjs`;
const WASMCART_SRC = process.env.WASMCART_SRC
  || `${process.env.HOME}/code/cliemu/wasmcart/src/webgl_imports.js`;
const ROOT = path.join(path.dirname(new URL(import.meta.url).pathname), '..');
const W = 1280, H = 720;

const cartDir = process.argv[2] || path.join(ROOT, 'test', 'prims');
const FRAMES = +(process.argv[3] || 60);
const TOL = +(process.argv[4] || 1);

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

async function run(engine, useGl) {
  const assets = loadAssets(cartDir);
  const ref = {};
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
    wc_debug_mark: () => {},
    emscripten_notify_memory_growth: () => {},
  };
  const imports = { env, wasi_snapshot_preview1: new Proxy({}, { get: () => () => 0 }) };
  let gl = null;
  if (useGl) {
    const wn = await import(WEBGL_NODE);
    const { createWebGLImports } = await import(WASMCART_SRC);
    gl = wn.createWebGL2Context(W, H).gl;
    imports.gl = createWebGLImports({ getMemory: () => ref.mem, ctx: gl });
  }
  const { instance } = await WebAssembly.instantiate(fs.readFileSync(engine), imports);
  const e = instance.exports;
  mem = e.memory; ref.mem = mem;
  const info = e.wc_get_info();
  e.wc_set_seed(1);
  e.wc_init();
  for (let i = 0; i < FRAMES; i++) e.wc_render();

  if (useGl) {
    gl.finish();
    const raw = new Uint8Array(W * H * 4);
    gl.readPixels(0, 0, W, H, gl.RGBA, gl.UNSIGNED_BYTE, raw);
    const out = new Uint8Array(W * H * 3);
    for (let y = 0; y < H; y++) {
      const s = (H - 1 - y) * W * 4, d = y * W * 3;
      for (let x = 0; x < W; x++) {
        out[d + x * 3] = raw[s + x * 4];
        out[d + x * 3 + 1] = raw[s + x * 4 + 1];
        out[d + x * 3 + 2] = raw[s + x * 4 + 2];
      }
    }
    return { rgb: out, logs };
  }
  const [, iw, ih, fb] = new Uint32Array(mem.buffer, info, 4);
  const px = new Uint32Array(mem.buffer, fb, iw * ih);
  const out = new Uint8Array(W * H * 3);
  for (let i = 0, o = 0; i < iw * ih; i++, o += 3) {
    out[o] = (px[i] >> 16) & 255; out[o + 1] = (px[i] >> 8) & 255; out[o + 2] = px[i] & 255;
  }
  return { rgb: out, logs };
}

const a = await run(path.join(ROOT, 'build', 'engine-gl.wasm'), true);
const b = await run(path.join(ROOT, 'build', 'engine.wasm'), false);

const hist = new Map();
let over = 0, maxd = 0, lit = 0;
for (let i = 0; i < a.rgb.length; i += 3) {
  const d = Math.max(Math.abs(a.rgb[i] - b.rgb[i]),
                     Math.abs(a.rgb[i + 1] - b.rgb[i + 1]),
                     Math.abs(a.rgb[i + 2] - b.rgb[i + 2]));
  hist.set(d, (hist.get(d) || 0) + 1);
  if (d > maxd) maxd = d;
  if (d > TOL) over++;
  if (b.rgb[i] | b.rgb[i + 1] | b.rgb[i + 2]) lit++;
}
const tot = W * H;
console.log(`cart: ${cartDir}  (${FRAMES} frames, tolerance ${TOL})`);
console.log(`software non-black pixels: ${lit} (${(lit / tot * 100).toFixed(1)}%)`);
const rows = [...hist].sort((x, y) => x[0] - y[0]).slice(0, 8);
console.log('delta histogram: ' + rows.map(([d, n]) => `${d}:${(n / tot * 100).toFixed(2)}%`).join('  '));
console.log(`max delta ${maxd}   over tolerance: ${over} (${(over / tot * 100).toFixed(3)}%)`);
if (lit === 0) { console.log('\nFAILED: software drew nothing; comparison is meaningless'); process.exit(1); }
if (over) {
  // Where are the outliers? A rounding difference is scattered; a geometry
  // or ordering bug clusters. Report the bounding box and a few samples so
  // the failure says WHICH it is.
  let x0=1e9,y0=1e9,x1=-1,y1=-1; const samples=[];
  for (let y=0;y<H;y++) for (let x=0;x<W;x++) {
    const i=(y*W+x)*3;
    const d=Math.max(Math.abs(a.rgb[i]-b.rgb[i]),Math.abs(a.rgb[i+1]-b.rgb[i+1]),Math.abs(a.rgb[i+2]-b.rgb[i+2]));
    if (d>TOL) { if(x<x0)x0=x; if(x>x1)x1=x; if(y<y0)y0=y; if(y>y1)y1=y;
      if(samples.length<6) samples.push(`(${x},${y}) d=${d} gl=[${a.rgb[i]},${a.rgb[i+1]},${a.rgb[i+2]}] cpu=[${b.rgb[i]},${b.rgb[i+1]},${b.rgb[i+2]}]`); }
  }
  console.log(`over-tolerance bbox: x ${x0}..${x1}  y ${y0}..${y1}`);
  for (const s of samples) console.log('  ' + s);
  console.log(`\nFAILED: ${over} pixels exceed +/-${TOL}`); process.exit(1);
}
console.log(`\nOK: every pixel within +/-${TOL}`);
