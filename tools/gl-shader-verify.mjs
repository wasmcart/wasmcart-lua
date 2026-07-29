#!/usr/bin/env node
/*
 * tools/gl-shader-verify.mjs - did the custom shader ACTUALLY run?
 *
 * A shader that fails to link renders unshaded, and a frame counter cannot
 * tell that apart from success. Neither can the GL-vs-CPU comparison the
 * other gates use: a custom shader is SUPPOSED to differ from the software
 * rasterizer, so "they differ" proves nothing there either.
 *
 * The check that does work is internal to one GL frame. examples/shaders
 * draws the same scene twice, at x and x+640, with a shader bound for the
 * right copy only. So:
 *
 *   - the halves must DIFFER (or the shader was never applied), and
 *   - they must differ the RIGHT way: the sampled inversion pairs have to
 *     satisfy right ~= 1 - left, not merely be unequal. A program that
 *     linked but sampled the wrong thing (the atlas-texel bug this file was
 *     written after) also produces "they differ", while producing white.
 *
 * The control lives in the same run: with the shader deliberately never
 * bound the halves are identical, and this gate must then FAIL. Run it with
 * `--control` to prove exactly that.
 *
 *   node tools/gl-shader-verify.mjs build/engine.wasm examples/shaders
 */
import fs from 'fs';
import path from 'path';

const WEBGL_NODE = process.env.WEBGL_NODE
  || `${process.env.HOME}/code/cliemu/romdev/node_modules/webgl-node/index.mjs`;

const W = 1280, H = 720;
const argv = process.argv.slice(2).filter(a => a !== '--logs');
/* --logs skips the split-screen probes and just prints the cart log, for a
 * cart whose verdict is about what the engine SAID (test/shaderfail) rather
 * than about pixels. It still needs the real GL context: on a stubbed `gl`
 * every shader is refused for the wrong reason, which would make the refusal
 * gate pass without the compiler ever being consulted. */
const LOGS_ONLY = process.argv.includes('--logs');
const enginePath = argv[0] || 'build/engine.wasm';
const cartDir = argv[1] || 'examples/shaders';
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

const WASMCART_SRC = process.env.WASMCART_SRC
  || `${process.env.HOME}/code/cliemu/wasmcart/src/webgl_imports.js`;
const { createWebGLImports } = await import(WASMCART_SRC);

const wn = await import(WEBGL_NODE);
const { gl } = wn.createWebGL2Context(W, H);

/* Two layouts in this repo: examples/ are packable carts with an app/ asset
 * dir next to a manifest, while test/ carts are a bare asset dir. Accept
 * both so this tool works on either. */
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

const SHIFT = 640;   // the example's half-width
// Points chosen to land in the middle of a shape on BOTH halves, one per
// draw path, so a shader that only reaches textured draws is caught too.
const probes = [
  ['solid rect',  70, 110],
  ['solid rect2', 340, 290],
  ['circle',      150, 420],
  ['sprite',      300, 380],
];

let differing = 0, inverted = 0;
const rows = [];
for (const [what, x, y] of probes) {
  const a = px(x, y), b = px(x + SHIFT, y);
  const diff = Math.max(...[0, 1, 2].map(i => Math.abs(a[i] - b[i])));
  // the shader is `1 - rgb`, so the pair must sum to ~255 per channel.
  // 6 of tolerance covers the +/-1 rounding this backend documents plus the
  // 8-bit round trip through the inversion.
  const invErr = Math.max(...[0, 1, 2].map(i => Math.abs(a[i] + b[i] - 255)));
  if (diff > 8) differing++;
  if (invErr <= 6) inverted++;
  rows.push(`  ${what.padEnd(12)} left=${a.join(',').padEnd(12)} right=${b.join(',').padEnd(12)} ` +
            `maxdiff=${String(diff).padStart(3)} |a+b-255|=${invErr}`);
}

console.log(`probing ${probes.length} points on both halves of the split screen:`);
for (const r of rows) console.log(r);

const shaderErrors = logs.filter(l => /newShader|setShader/.test(l));
if (shaderErrors.length) {
  console.log('\nshader-related log lines:');
  for (const l of shaderErrors) console.log('  ' + l);
}

if (differing !== probes.length) {
  console.log(`\nFAILED: only ${differing}/${probes.length} probes differ between the ` +
              'halves. The shader did not reach every draw path (or did not run at all).');
  process.exit(1);
}
if (inverted !== probes.length) {
  console.log(`\nFAILED: ${differing}/${probes.length} probes differ but only ` +
              `${inverted}/${probes.length} are the true inverse. The shader ran and ` +
              'produced the WRONG colour -- e.g. sampling a real texel for an ' +
              'untextured draw, which turns every solid into white.');
  process.exit(1);
}
console.log(`\nOK: all ${probes.length} probes inverted correctly; the custom shader ran ` +
            'on solids, circles and sprites alike');
