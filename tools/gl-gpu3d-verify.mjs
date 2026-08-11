#!/usr/bin/env node
/*
 * tools/gl-gpu3d-verify.mjs - are the deferred-rendering capabilities REAL?
 *
 * test/gpu3d prints a PASS/FAIL line per capability, but those lines only
 * prove the calls did not error. A float canvas that silently fell back to
 * RGBA8 still "creates" and still "binds"; MRT that broadcast one colour to
 * every attachment still "draws". So this gate ignores the cart's own
 * verdicts for the things that can lie, and reads the PIXELS.
 *
 * Each assertion has a specific wrong answer it distinguishes:
 *
 *  1. FLOAT CANVAS. The cart writes 2.5 into an rgba16f target and presents
 *     it scaled by 0.25. A real float target gives 0.625 -> 159. An RGBA8
 *     target clamps 2.5 to 1.0 and gives 0.25 -> 64. Those are 95 apart, so
 *     a silent downgrade cannot hide in rounding.
 *
 *  2. MRT. The same pass writes attachment 0 = red 2.5 and attachment 1 =
 *     green 0.5. If MRT were broadcasting, both swatches would be the same
 *     colour. Requiring swatch A to be red-dominant and swatch B to be
 *     green-dominant is what makes "two targets" mean two.
 *
 *  3. COLOUR MASK. White drawn with the green channel masked off must be
 *     magenta. A no-op mask gives white -- one channel apart, and the
 *     channel that is wrong names the bug.
 *
 *  4. INSTANCING. Eight instances placed by gl_InstanceID must appear as
 *     eight separate bars. Ignored instancing stacks them into one.
 *
 *  5. CUBE IMAGE FACE ORDER. Six faces of six known colours, sampled by the
 *     six axis directions. The band colours must come back in face order.
 *     This is the assertion that catches a cubemap wired up backwards --
 *     which on a skybox looks completely fine until the sun is in the wrong
 *     place.
 *
 *   node tools/gl-gpu3d-verify.mjs build/engine.wasm test/gpu3d
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
const cartDir = argv[1] || 'test/gpu3d';
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
// The cart's own verdicts still matter for the things a pixel cannot show
// (a bind that was refused, a format the driver rejected).
for (const l of logs) if (/^FAIL /.test(l)) problems.push('cart reported: ' + l);

// ── 1 + 2: float canvas and MRT ─────────────────────────────────────
console.log('1. float canvas (2.5 stored in rgba16f, presented at 0.25x):');
const fa = px(175, 225);
// 2.5 * 0.25 = 0.625 -> 159. A clamped RGBA8 target would give 1.0*0.25 -> 64.
const floatOK = fa[0] > 120 && fa[0] < 200 && fa[1] < 60;
console.log(`   ${floatOK ? 'ok  ' : 'BAD '} swatch A = ${fa.join(',')} ` +
            `(float -> ~159 red; an RGBA8 downgrade clamps to ~64)`);
if (!floatOK) {
  problems.push(
    `the float swatch is ${fa.join(',')}, not ~159 red. A value of 2.5 did ` +
    'not survive the round trip, so the canvas is not really a float target ' +
    '-- it was silently created as (or downgraded to) 8-bit.');
}

console.log('2. MRT (two attachments, one pass, different values):');
const fb = px(463, 225);
const mrtOK = fa[0] > fa[1] && fb[1] > fb[0];
console.log(`   ${mrtOK ? 'ok  ' : 'BAD '} A = ${fa.join(',')} (red-dominant), ` +
            `B = ${fb.join(',')} (green-dominant)`);
if (!mrtOK) {
  problems.push(
    `attachment 0 = ${fa.join(',')} and attachment 1 = ${fb.join(',')}. The ` +
    'two targets did not receive different colours, so the draw is writing ' +
    'one output to every attachment rather than doing real MRT.');
}

// ── 3: colour mask ──────────────────────────────────────────────────
console.log('3. colour mask (white with green masked off -> magenta):');
const cm = px(751, 225);
const maskOK = cm[0] > 200 && cm[2] > 200 && cm[1] < 60;
console.log(`   ${maskOK ? 'ok  ' : 'BAD '} swatch = ${cm.join(',')} ` +
            `(expect ~255,0,255; an ignored mask gives 255,255,255)`);
if (!maskOK) {
  problems.push(
    `the masked swatch is ${cm.join(',')}, not magenta. setColorMask did not ` +
    'block the green channel.');
}

// ── 4: instancing ───────────────────────────────────────────────────
console.log('4. instancing (8 instances placed by gl_InstanceID):');
let bars = 0, inside = false;
for (let x = 0; x < W; x++) {
  const [r, g, b] = px(x, 485);
  const on = r > 150 && g > 150 && b < 100;
  if (on && !inside) { bars++; inside = true; }
  else if (!on) inside = false;
}
console.log(`   ${bars === 8 ? 'ok  ' : 'BAD '} distinct bars: ${bars} (expect 8)`);
if (bars !== 8) {
  problems.push(
    `${bars} instanced bars instead of 8. gl_InstanceID is not varying per ` +
    'instance, so every instance drew at the same place (or the instanced ' +
    'draw did not happen at all).');
}

// ── 5: cube image face order ────────────────────────────────────────
console.log('5. cube image (6 faces sampled by direction, in face order):');
const WANT = [[255,0,0],[0,255,0],[0,0,255],[255,255,0],[255,0,255],[0,255,255]];
const DIRS = ['+X','-X','+Y','-Y','+Z','-Z'];
let faceOK = 0;
for (let i = 0; i < 6; i++) {
  const x = 64 + Math.round((i + 0.5) * (1152 / 6));
  const got = px(x, 638);
  const want = WANT[i];
  const good = [0,1,2].every(c => Math.abs(got[c] - want[c]) <= 6);
  if (good) faceOK++;
  console.log(`   ${good ? 'ok  ' : 'BAD '} face ${i + 1} (dir ${DIRS[i]}) = ` +
              `${got.join(',')} want ${want.join(',')}`);
}
if (faceOK !== 6) {
  problems.push(
    `${faceOK}/6 cube faces sampled to the right colour. The faces are ` +
    'uploaded in the wrong order or sampled with the wrong direction ' +
    'convention -- a skybox built on this looks fine until you notice the ' +
    'sun is on the wrong side.');
}

console.log('');
if (problems.length) {
  console.log('FAIL');
  for (const p of problems) console.log('  - ' + p);
  process.exit(1);
}
console.log('PASS  float canvases, MRT, colour mask, instancing and cube ' +
            'images all verified by pixel');
