#!/usr/bin/env node
/*
 * test/determinism.js - prove the engine's headline determinism claim.
 *
 * Two properties, and the second one matters as much as the first:
 *
 *   1. REPEATABLE: same seed + same inputs => byte-identical framebuffer.
 *      This is the claim replays, lockstep netplay, and frame-hash goldens
 *      depend on.
 *
 *   2. SEED-SENSITIVE (the control): for a cart whose visuals actually
 *      depend on RNG, a different seed must produce a DIFFERENT frame. If
 *      everything hashes the same no matter what, the harness is measuring
 *      a constant and property 1 is meaningless.
 *
 * Carts that never call love.math.random in a way that reaches the screen
 * (platformer, breakout: fixed layouts, deterministic physics) are expected
 * to be seed-insensitive, so they only get property 1. That expectation is
 * declared here rather than inferred, so a cart that SHOULD vary and
 * silently stops varying still fails.
 */
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const ROOT = path.join(__dirname, '..');
const ENGINE = path.join(ROOT, 'build', 'engine.wasm');

// seedSensitive: does RNG visibly affect this cart's rendered output?
const CARTS = [
  { name: 'shmup',      seedSensitive: true,  frames: 150 }, // enemy drift + bursts
  { name: 'particles',  seedSensitive: true,  frames: 150 }, // random emitter velocities
  { name: 'pong',       seedSensitive: true,  frames: 150 }, // random launch vector
  { name: 'platformer', seedSensitive: false, frames: 150 }, // fixed map + physics
  { name: 'breakout',   seedSensitive: false, frames: 150 }, // fixed layout + physics
];

function loadAssets(dir, prefix = '') {
  const out = {};
  for (const n of fs.readdirSync(dir)) {
    const p = path.join(dir, n);
    const rel = prefix ? prefix + '/' + n : n;
    if (fs.statSync(p).isDirectory()) Object.assign(out, loadAssets(p, rel));
    else out[rel] = fs.readFileSync(p);
  }
  return out;
}

async function frameHash(appDir, seed, frames) {
  const assets = loadAssets(appDir);
  let mem;
  const dec = new TextDecoder();
  const imports = {
    env: {
      wc_log: () => {},
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
    },
    wasi_snapshot_preview1: new Proxy({}, { get: () => () => 0 }),
  };
  const { instance } = await WebAssembly.instantiate(fs.readFileSync(ENGINE), imports);
  const e = instance.exports;
  mem = e.memory;
  const ip = e.wc_get_info();
  const d = () => new DataView(mem.buffer);
  const w = d().getUint32(ip + 4, true);
  const h = d().getUint32(ip + 8, true);
  const fb = d().getUint32(ip + 12, true);
  const inputPtr = d().getUint32(ip + 28, true);

  e.wc_set_seed(seed);
  e.wc_init();
  // A fixed input script: alternate right / left, and TAP A rather than
  // holding it. Holding A matters because carts read it as an edge
  // (love.pad.wasPressed) -- particles cycles its emitter mode on every
  // press, so a held A walks through all modes and lands in one where the
  // seed's effect is averaged out. Tapping exercises both the held and edge
  // paths without turning the run into a mode-cycling stress test.
  const RIGHT = 1 << 11, LEFT = 1 << 10, A = 1 << 0;
  for (let i = 0; i < frames; i++) {
    const btn = (i % 40 < 20 ? RIGHT : LEFT) | (i % 30 === 0 ? A : 0);
    new DataView(mem.buffer).setUint16(inputPtr, btn, true);
    e.wc_render();
  }
  return crypto.createHash('sha256')
    .update(Buffer.from(mem.buffer, fb, w * h * 4)).digest('hex').slice(0, 16);
}

async function main() {
  if (!fs.existsSync(ENGINE)) {
    console.error('engine not built: run runtime/build.sh first');
    process.exit(1);
  }
  let failed = 0;
  for (const { name, seedSensitive, frames } of CARTS) {
    const app = path.join(ROOT, 'examples', name, 'app');
    if (!fs.existsSync(app)) continue;
    const a = await frameHash(app, 777, frames);
    const b = await frameHash(app, 777, frames);
    const c = await frameHash(app, 999, frames);

    const problems = [];
    if (a !== b) problems.push(`same seed produced DIFFERENT frames (${a} vs ${b})`);
    if (seedSensitive && a === c) {
      problems.push('control: a different seed produced an IDENTICAL frame - ' +
                    'the seed is not reaching the RNG');
    }
    if (problems.length) {
      failed++;
      console.log(`FAIL  ${name}`);
      for (const p of problems) console.log(`      ${p}`);
    } else {
      const note = seedSensitive ? 'repeatable + seed-sensitive' : 'repeatable (no RNG in output)';
      console.log(`ok    ${name.padEnd(12)} ${a}  ${note}`);
    }
  }
  console.log(failed ? `\n${failed} FAILED` : '\ndeterminism verified');
  process.exit(failed ? 1 : 0);
}

main().catch(e => { console.error(e); process.exit(1); });
