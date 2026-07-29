#!/usr/bin/env node
/*
 * tools/gl-mesh-verify.mjs - did the mesh ACTUALLY render, and correctly?
 *
 * A mesh that never draws leaves the frame count perfect and the screen
 * empty, so "it ran 60 frames" proves nothing. Nor does a blank-frame check:
 * examples/mesh draws plenty of text and sprites that would keep any
 * uniformity gate green with every mesh missing.
 *
 * What does work is an assertion INTERNAL to one frame, in three parts.
 * Each catches a failure the other two cannot see:
 *
 *  1. THE ATLAS UV REMAP. examples/mesh draws a textured mesh, and directly
 *     beneath it the SAME image as an ordinary sprite at the same size. A
 *     sprite goes through wcl_r2d_sprite, which does its own atlas
 *     arithmetic; a mesh goes through wcl_r2d_mesh, which does different
 *     arithmetic on normalized 0..1 uv. If the mesh's remap is wrong the two
 *     panels show different pictures -- most likely the DECOY image, which
 *     is loaded into the atlas first and is bright magenta/cyan stripes
 *     precisely so a stale offset is unmistakable. Sampling several points
 *     across both panels and requiring them to agree is the tightest
 *     available statement of "the uv remap is right", and it is a statement
 *     a difference-from-software comparison cannot make (a mesh has no
 *     software path to compare against).
 *
 *  2. THE COLOUR ATTRIBUTE. The per-vertex-coloured quad must be a GRADIENT.
 *     Probing its four corners and requiring them to differ from each other
 *     catches the case where the colour attribute never reaches the shader
 *     and the quad renders flat -- which still "renders a mesh".
 *
 *  3. DRAW MODE. The "triangles" and "fan" panels are built from the SAME
 *     six vertices, so if the mode were ignored they would be identical.
 *     Probing a point that is inside the fan but in the GAP between the two
 *     separate triangles asserts the modes actually differ.
 *
 * The control: with the mesh draws removed this gate must go RED. test/run.js
 * runs exactly that copy, and a gate that has never been seen red is not a
 * gate.
 *
 *   node tools/gl-mesh-verify.mjs build/engine.wasm examples/mesh
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
const cartDir = argv[1] || 'examples/mesh';
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

// readPixels' origin is bottom-left; the cart's is top-left.
const px = (x, y) => {
  const o = ((H - 1 - y) * W + x) * 4;
  return [gpu[o], gpu[o + 1], gpu[o + 2]];
};
const maxd = (a, b) => Math.max(...[0, 1, 2].map(i => Math.abs(a[i] - b[i])));

const problems = [];

// ── 1: the textured mesh must match the sprite reference ────────────
//
// The mesh is at (40,56) and the sprite at (40,276), both 180x180 covering
// the same 64x64 image. Sample the same offsets inside each and require
// agreement. The offsets deliberately hit all four quadrant colours plus the
// black L bracket, so a uv flip, a 90-degree rotation, a shear and a
// wrong-image sample are each caught by at least one probe.
const MESH_XY = [40, 56], SPRITE_XY = [40, 276];
const OFFS = [
  ['red quadrant (TL)',    45,  30],
  ['green quadrant (TR)', 135,  30],
  ['blue quadrant (BL)',   45, 150],
  ['yellow quadrant (BR)',135, 150],
  ['L bracket (black)',    30,  50],
  ['border (white)',        3,  90],
];
console.log('1. textured mesh vs the same image as a sprite (atlas uv remap):');
let uvBad = 0;
for (const [what, dx, dy] of OFFS) {
  const m = px(MESH_XY[0] + dx, MESH_XY[1] + dy);
  const s = px(SPRITE_XY[0] + dx, SPRITE_XY[1] + dy);
  const d = maxd(m, s);
  // +/-2 is this backend's documented blend rounding budget. Anything above
  // that is a different picture, not a rounding difference.
  const ok = d <= 2;
  if (!ok) uvBad++;
  console.log(`   ${ok ? 'ok  ' : 'BAD '} ${what.padEnd(22)} mesh=${m.join(',').padEnd(12)} ` +
              `sprite=${s.join(',').padEnd(12)} maxdiff=${d}`);
}
if (uvBad) {
  problems.push(`${uvBad}/${OFFS.length} probes disagree between the mesh and the ` +
    'sprite. The mesh is sampling the wrong part of the atlas (or nothing at all).');
}

// The mesh must also not be sampling the DECOY. Its stripes are pure
// magenta/cyan, which nothing in tex.png comes close to.
const decoyish = (c) => (c[0] > 200 && c[1] < 60 && c[2] > 150) ||
                        (c[0] < 60 && c[1] > 190 && c[2] > 220);
let decoyHits = 0;
for (const [, dx, dy] of OFFS) {
  if (decoyish(px(MESH_XY[0] + dx, MESH_XY[1] + dy))) decoyHits++;
}
if (decoyHits) {
  problems.push(`the mesh sampled the DECOY image at ${decoyHits} probe(s): the atlas ` +
    'offset is being ignored, so uv 0..1 addresses the whole atlas.');
}

// ── 2: per-vertex colour must be a gradient, not a flat block ───────
console.log('\n2. per-vertex colour (the quad must be a gradient, not flat):');
const CM = [300, 56];
const corners = [
  ['top-left  (red)',    12,  12],
  ['top-right (green)', 168,  12],
  ['bot-right (blue)',  168, 168],
  ['bot-left  (white)',  12, 168],
];
const got = corners.map(([, dx, dy]) => px(CM[0] + dx, CM[1] + dy));
for (let i = 0; i < corners.length; i++) {
  console.log(`   ${corners[i][0].padEnd(20)} ${got[i].join(',')}`);
}
let spread = 0;
for (let i = 0; i < got.length; i++)
  for (let j = i + 1; j < got.length; j++) spread = Math.max(spread, maxd(got[i], got[j]));
console.log(`   max corner-to-corner spread = ${spread}`);
if (spread < 60) {
  problems.push(`the per-vertex-coloured quad is flat (corner spread ${spread}). The ` +
    'colour attribute is not reaching the fragment stage.');
}
// and it must actually be DRAWN, not background
const bg = px(1240, 700);
if (maxd(got[0], bg) < 20) {
  problems.push('the per-vertex-coloured quad is the background colour: it did not draw.');
}

// ── 3: "triangles" and "fan" must differ on identical vertices ──────
//
// The two panels share one six-vertex list. The probe sits in the horizontal
// band between the two disjoint triangles of the "triangles" panel, which
// the "fan" panel fills in. Same point, same vertices, different mode.
console.log('\n3. "triangles" vs "fan" on the SAME six vertices:');
const TRI = [570, 76], FAN = [750, 76];
const GAP = [60, 100];     // y=100 is between the triangles (they meet at 90..110)
const triPx = px(TRI[0] + GAP[0], TRI[1] + GAP[1]);
const fanPx = px(FAN[0] + GAP[0], FAN[1] + GAP[1]);
console.log(`   gap point   triangles=${triPx.join(',')}  fan=${fanPx.join(',')}`);
const triIsBg = maxd(triPx, bg) < 20;
const fanIsBg = maxd(fanPx, bg) < 20;
console.log(`   triangles: ${triIsBg ? 'background (a gap, as it should be)' : 'FILLED'}`);
console.log(`   fan:       ${fanIsBg ? 'BACKGROUND' : 'filled (as it should be)'}`);
if (!triIsBg) {
  problems.push('the "triangles" mesh has no gap between its two triangles: the mode ' +
    'was ignored and it was drawn as a fan.');
}
if (fanIsBg) {
  problems.push('the "fan" mesh did not fill the region between its vertices: either ' +
    'it did not draw, or "fan" was treated as "triangles".');
}

// ── 4: the uv sub-rect panel must show ONLY the red quadrant ────────
//
// uv 0..0.5 of tex.png is the top-left quadrant: red, with the black L. If
// the remap scaled to the whole atlas instead, this panel would be some
// unrecognisable fraction of a 2048px texture.
console.log('\n4. uv 0..0.5 sub-rect through a vertex map:');
const UVM = [960, 56];
const redish = px(UVM[0] + 150, UVM[1] + 40);
console.log(`   sample inside the sub-rect = ${redish.join(',')}`);
if (!(redish[0] > 150 && redish[1] < 110 && redish[2] < 110)) {
  problems.push(`the uv-sub-rect mesh is ${redish.join(',')}, not the red quadrant it ` +
    'samples. The uv range is not being mapped into the image\'s atlas sub-rect.');
}

const meshErrors = logs.filter(l => /[Mm]esh/.test(l));
if (meshErrors.length) {
  console.log('\nmesh-related log lines:');
  for (const l of meshErrors) console.log('  ' + l);
}

if (problems.length) {
  console.log('\nFAILED:');
  for (const p of problems) console.log('  - ' + p);
  process.exit(1);
}
console.log('\nOK: the textured mesh matches its sprite reference (atlas uv remap is ' +
            'correct), per-vertex colour interpolates, and "triangles"/"fan" differ ' +
            'on identical vertices');
