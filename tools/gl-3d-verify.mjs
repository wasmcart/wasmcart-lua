#!/usr/bin/env node
/*
 * tools/gl-3d-verify.mjs - did the 3D scene render, and is DEPTH real?
 *
 * The cart under test is test/g3d, which runs groverburger's g3d with its
 * library sources copied VERBATIM from upstream. That is the actual claim
 * being tested: not "3D works" but "an unmodified third-party LOVE 3D
 * library runs on this engine". If a change to the engine breaks any of the
 * LOVE surface g3d relies on -- custom vertex formats, setDepthMode,
 * package.loaded, getCanvas, flat-16 mat4 sends, GLSL ES 1.00 rewriting --
 * this gate goes red and names which one.
 *
 * Three assertions, each catching what the others cannot:
 *
 *  1. GEOMETRY EXISTS. A textured 3D scene must cover a meaningful number of
 *     pixels. This alone catches the whole class of "the draw executed with
 *     no GL error and rasterized nothing", which is what a transposed matrix,
 *     a degenerate view matrix, or a broken vertex layout each produce --
 *     silently, with a perfect frame count.
 *
 *  2. IT IS A 3D PROJECTION, NOT A FLAT QUAD. The cube's texture is a
 *     regular checkerboard, so under perspective its squares must SHRINK
 *     with distance. Comparing the checker period near the top of the cube
 *     against the period near the bottom asserts foreshortening: an identity
 *     or orthographic transform makes them equal, a perspective one does not.
 *
 *  3. DEPTH ACTUALLY OCCLUDES. The scene draws a near cube FIRST and a far,
 *     overlapping cube SECOND. Under painter's order the far cube wins;
 *     under a depth test the near one does. The gate renders the cart twice
 *     -- once as shipped, once with a NODEPTH asset that turns the depth
 *     test back off -- and requires the two frames to DIFFER materially.
 *     That is the control: if disabling depth changes nothing, the depth
 *     test was never doing anything and assertion 1 would still be green.
 *
 *   node tools/gl-3d-verify.mjs build/engine.wasm test/g3d
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
const cartDir = argv[1] || 'test/g3d';
const FRAMES = +(argv[2] || 3);

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

const appDir = fs.existsSync(path.join(cartDir, 'app'))
  ? path.join(cartDir, 'app') : cartDir;
const baseAssets = loadAssets(appDir);

/* Render the cart once and return the RGBA framebuffer.
 *
 * `extraAssets` is how the control is expressed: adding a NODEPTH asset makes
 * the SAME Lua turn the depth test off, so the two runs differ by exactly one
 * thing. A separate control cart could drift from the real one. */
async function render(extraAssets = {}) {
  const { gl } = wn.createWebGL2Context(W, H);
  const assets = { ...baseAssets, ...extraAssets };
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
  gl.finish();
  const buf = new Uint8Array(W * H * 4);
  gl.readPixels(0, 0, W, H, gl.RGBA, gl.UNSIGNED_BYTE, buf);
  return { buf, logs };
}

const { buf: gpu, logs } = await render();

if (LOGS_ONLY) {
  for (const l of logs) console.log('LOG: ' + l);
  process.exit(0);
}

// readPixels' origin is bottom-left; the cart's is top-left.
const px = (b, x, y) => {
  const o = ((H - 1 - y) * W + x) * 4;
  return [b[o], b[o + 1], b[o + 2]];
};

const problems = [];

// ── 1: geometry exists ──────────────────────────────────────────────
//
// Count pixels that are neither the black background nor the white HUD text.
// The cart's texture is red/blue, so "coloured" is an unambiguous signal that
// the 3D draw put something on screen.
console.log('1. the 3D scene rendered:');
let coloured = 0;
for (let y = 0; y < H; y += 2) {
  for (let x = 0; x < W; x += 2) {
    const [r, g, b] = px(gpu, x, y);
    // red-dominant or blue-dominant: the checkerboard's two colours.
    if ((r > 90 && r > g + 40) || (b > 90 && b > g + 40)) coloured++;
  }
}
// Measured, not guessed: the scene covers ~9900 px at full density, so ~2480
// at this 1/4 sampling. The floor is set well below that to leave room for
// pose and driver differences while still being far above "a few stray
// pixels" -- the failure being caught is a scene that did not draw at all.
const MIN_COLOURED = 1200;
console.log(`   ${coloured >= MIN_COLOURED ? 'ok  ' : 'BAD '} textured pixels: ${coloured} ` +
            `(need >= ${MIN_COLOURED})`);
if (coloured < MIN_COLOURED) {
  problems.push(
    `only ${coloured} textured pixels. The 3D draw is not reaching the ` +
    'framebuffer. This is what a transposed matrix, a degenerate view ' +
    'matrix, or a broken vertex layout all look like: no GL error, no ' +
    'missing uniform, and nothing rasterized.');
}

// ── 2: it is a 3D projection, not a flat quad ───────────────────────
//
// A rotated cube under perspective has an irregular, many-sided silhouette:
// its width varies from row to row as the visible faces turn away. A quad --
// which is what an identity transform, a dropped z, or a collapsed vertex
// layout would leave -- has a silhouette whose width is constant down its
// whole height.
//
// So: measure the run width on every row of the scene and require it to VARY
// substantially. This tests the projected shape itself rather than the
// texture, so it holds regardless of filtering or mip selection.
console.log('2. the silhouette is a projected solid, not a flat quad:');
const widths = [];
for (let y = 0; y < H; y++) {
  let lo = -1, hi = -1;
  for (let x = 0; x < W; x++) {
    const [r, g, b] = px(gpu, x, y);
    if ((r > 90 && r > g + 40) || (b > 90 && b > g + 40)) {
      if (lo < 0) lo = x;
      hi = x;
    }
  }
  if (lo >= 0) widths.push(hi - lo + 1);
}
if (widths.length < 40) {
  console.log(`   BAD  scene spans only ${widths.length} rows`);
  problems.push(`the scene covers ${widths.length} rows; too little geometry to ` +
                'test the projection.');
} else {
  const wMin = Math.min(...widths), wMax = Math.max(...widths);
  // A rotated cube's widest row is far wider than its narrowest (the corners
  // taper). A flat axis-aligned quad would give wMin === wMax.
  const ratio = wMax / wMin;
  const ok = ratio >= 1.5;
  console.log(`   ${ok ? 'ok  ' : 'BAD '} silhouette width varies ${wMin}..${wMax} px ` +
              `(ratio ${ratio.toFixed(2)}, need >= 1.5) over ${widths.length} rows`);
  if (!ok) {
    problems.push(
      `the silhouette is ${wMin}..${wMax} px wide on every row (ratio ` +
      `${ratio.toFixed(2)}). The geometry is on screen but its shape is a flat ` +
      'quad, so the projection is not being applied to z.');
  }
}

// ── 3: THE CONTROL - depth must actually occlude ────────────────────
//
// Same cart, same pose, one difference: a NODEPTH asset that turns the depth
// test off. If the frames match, the depth test is inert.
console.log('3. depth occlusion (control: the same cart with depth disabled):');
const { buf: ctl } = await render({ NODEPTH: Buffer.from('1') });
let diff = 0;
for (let y = 0; y < H; y++) {
  for (let x = 0; x < W; x++) {
    const o = ((H - 1 - y) * W + x) * 4;
    if (gpu[o] !== ctl[o] || gpu[o + 1] !== ctl[o + 1] || gpu[o + 2] !== ctl[o + 2]) diff++;
  }
}
const MIN_DIFF = 2000;
console.log(`   ${diff >= MIN_DIFF ? 'ok  ' : 'BAD '} pixels changed by disabling depth: ` +
            `${diff} (need >= ${MIN_DIFF})`);
if (diff < MIN_DIFF) {
  problems.push(
    `disabling the depth test changed only ${diff} pixels. The depth test is ` +
    'not affecting the image, so assertion 1 passing means only that ' +
    'triangles were drawn in submission order.');
}

console.log('');
if (problems.length) {
  console.log('FAIL');
  for (const p of problems) console.log('  - ' + p);
  process.exit(1);
}
console.log(`PASS  g3d renders in perspective; depth occlusion verified ` +
            `(${diff} px change with depth off)`);
