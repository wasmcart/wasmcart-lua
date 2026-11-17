#!/usr/bin/env node
/*
 * test/clock.mjs -- love.update(dt) and love.timer follow the HOST clock.
 *
 * Packs a tiny cart against the engine and drives it through wasmcart's
 * public CartHost. The cart reports dt and love.timer.getTime() (both in
 * microseconds) through the debug slots.
 *
 * Two runs, each the other's control:
 *   - deterministic clock (seed 7): every dt is the fixed 16.667 ms step and
 *     getTime advances by exactly one step per frame.
 *   - real clock with a 40 ms wait between frames: dt near 40 ms, which a
 *     constant 1/60 (or a frame count wearing a clock's name) cannot produce.
 * Plus a third run at a 120 Hz deterministic step, so a value that only
 * happens to be 1/60 cannot pass both.
 *
 * MUST-FAIL CONTROL: --prelude <file> ships that file as app/prelude.lua,
 * which replaces the engine's embedded prelude. Given the 0.6.2 prelude
 * (fixed dt = 1/60) it fails the 120 Hz and real-clock checks:
 *
 *   git show v0.6.2:runtime/prelude.lua > old-prelude.lua
 *   node test/clock.mjs --prelude old-prelude.lua      # exits 1
 *
 * (The 0.6.2 engine itself cannot be the control: it is an ABI 3 cart, which
 * wasmcart 0.41 refuses to load.)
 *
 *   node test/clock.mjs [engine.wasm] [--prelude file]
 *     (default engine: build/engine-cpu.wasm)
 */
import { existsSync, mkdtempSync, mkdirSync, writeFileSync, rmSync, copyFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { execFileSync } from 'node:child_process';
import { createRequire } from 'node:module';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(HERE, '..');
const require = createRequire(import.meta.url);
const WASMCART = process.env.WASMCART_REPO
  ? resolve(process.env.WASMCART_REPO)
  : dirname(require.resolve('wasmcart'));
const { CartHost } = await import(pathToFileURL(join(WASMCART, 'index.js')).href);

const args = process.argv.slice(2);
const pi = args.indexOf('--prelude');
const preludePath = pi >= 0 ? resolve(args.splice(pi, 2)[1]) : null;
const enginePath = resolve(args[0] || join(ROOT, 'build', 'engine-cpu.wasm'));
if (!existsSync(enginePath)) {
  console.error(`engine not found: ${enginePath} (run runtime/build.sh)`);
  process.exit(2);
}

const MAIN_LUA = `
local sum = 0
function love.update(dt)
  sum = sum + dt
  -- dt must match getDelta, and os.clock must match getTime
  local agree = (dt == love.timer.getDelta()) and (os.clock() == love.timer.getTime())
  love.debugValue(0, math.floor(dt * 1e6 + 0.5))
  love.debugValue(1, agree and math.floor(love.timer.getTime() * 1e6 + 0.5) or -1)
end
function love.draw()
  love.graphics.setColor(0.2, 0.8, 0.3)
  love.graphics.rectangle("fill", 0, 0, 64, 64)
end
`;

const work = mkdtempSync(join(tmpdir(), 'wcl-clock-'));
const appDir = join(work, 'app');
mkdirSync(appDir);
writeFileSync(join(appDir, 'main.lua'), MAIN_LUA);
if (preludePath) {
  copyFileSync(preludePath, join(appDir, 'prelude.lua'));
  console.log(`control: prelude replaced by ${preludePath}`);
}
const cartPath = join(work, 'clock.wasc');
execFileSync('node', [
  join(WASMCART, 'bin', 'wasmcart-pack.js'),
  '--wasm', enginePath, '--assets', appDir,
  '--name', 'clock-verify', '--width', '320', '--height', '240',
  '-o', cartPath,
], { stdio: 'ignore' });

const pads = [{ connected: true, buttons: 0, leftX: 0, leftY: 0, rightX: 0, rightY: 0, leftTrigger: 0, rightTrigger: 0 }];
const wait = (ms) => new Promise((r) => setTimeout(r, ms));

async function run(options, gapMs, frames = 10) {
  const cart = new CartHost();
  await cart.load(cartPath, options);
  const samples = [];
  for (let i = 0; i < frames; i++) {
    if (gapMs) await wait(gapMs);
    cart.runFrame(pads);
    samples.push({
      dt: Number(cart.readDebugValue('score').value) / 1000,     // ms
      time: Number(cart.readDebugValue('aux').value) / 1000,     // ms
    });
  }
  const luaOk = Number(cart.readDebugValue('lua_ok').value);
  cart.dispose?.();
  return { samples, luaOk };
}

const fails = [];
const check = (ok, msg) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); if (!ok) fails.push(msg); };
const fmt = (xs) => xs.map((x) => x.toFixed(3)).join(' ');
const near = (a, b, tol = 0.002) => Math.abs(a - b) < tol;

try {
  const det = await run({ deterministic: { seed: 7 } }, 0);
  console.log('--- deterministic clock (seed 7) ---');
  console.log('dt ms  ', fmt(det.samples.map((s) => s.dt)));
  console.log('time ms', fmt(det.samples.map((s) => s.time)));
  check(det.luaOk === 1, 'cart ran without a Lua error');
  check(det.samples.every((s) => s.time >= 0), 'dt == getDelta and os.clock == getTime every frame');
  check(det.samples.every((s) => near(s.dt, 1000 / 60)), 'every dt is the fixed 16.667 ms step');
  check(det.samples.slice(1).every((s, i) => near(s.time - det.samples[i].time, 1000 / 60)),
    'getTime advances by exactly one step per frame');

  const det2 = await run({ deterministic: { seed: 7 } }, 0);
  check(JSON.stringify(det2.samples) === JSON.stringify(det.samples),
    'a second deterministic run reports identical values');

  const hz120 = await run({ deterministic: { seed: 7, stepMs: 1000 / 120 } }, 0);
  console.log('--- deterministic clock, 120 Hz step ---');
  console.log('dt ms  ', fmt(hz120.samples.map((s) => s.dt)));
  check(hz120.samples.every((s) => near(s.dt, 1000 / 120)), 'every dt is the 8.333 ms step');
  check(near(hz120.samples[9].time, 9 * 1000 / 120), 'getTime is frame * 8.333 ms');

  const real = await run({}, 40);
  console.log('--- real clock, 40 ms between frames ---');
  console.log('dt ms  ', fmt(real.samples.map((s) => s.dt)));
  console.log('time ms', fmt(real.samples.map((s) => s.time)));
  check(real.luaOk === 1, 'cart ran without a Lua error');
  const later = real.samples.slice(1);
  check(later.every((s) => s.dt >= 35 && s.dt <= 250),
    'dt reports the real frame gap (35..250 ms; the host clamps at 250)');
  check(later.every((s, i) => s.time > real.samples[i].time), 'getTime keeps increasing');
  check(Math.abs(later[later.length - 1].dt - det.samples[9].dt) > 10,
    'the real and deterministic clocks disagree, so dt is the host clock and not a constant');
} finally {
  rmSync(work, { recursive: true, force: true });
}

console.log(fails.length ? `\n${fails.length} FAILED` : '\nall clock checks passed');
process.exit(fails.length ? 1 : 0);
