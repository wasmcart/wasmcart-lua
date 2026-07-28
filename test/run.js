#!/usr/bin/env node
/*
 * test/run.js - headless conformance + smoke harness for the engine.
 *
 * Instantiates the engine wasm with a minimal wasmcart host (the same import
 * surface a real host provides), runs each example for N frames, and asserts
 * the things that actually break: the cart instantiates, Lua boots without
 * error, the framebuffer is not blank/uniform, and no WASI import is ever
 * called at runtime.
 *
 * The "must fail" control is deliberate: a broken-cart case proves the
 * harness can actually detect failure. A suite that only ever reports green
 * is indistinguishable from a suite that cannot see red.
 *
 *   node test/run.js            # all examples
 *   node test/run.js pong       # one example
 */
const fs = require('fs');
const path = require('path');

const ROOT = path.join(__dirname, '..');
// build/engine.wasm is now the GL build. The example smoke runs, the unit
// cart and the blit/prims GOLDENS all target the CPU comparator instead: the
// goldens assert bit-equality, which is a property of the software
// rasterizer, and running them against GL would either fail or force the
// goldens to be loosened into meaninglessness. GL is gated separately, by
// tolerance, in the gl2d* section below.
const ENGINE = path.join(ROOT, 'build', 'engine-cpu.wasm');
const GL_ENGINE = path.join(ROOT, 'build', 'engine.wasm');

function loadAssets(dir) {
  const out = {};
  const walk = (d, prefix) => {
    for (const name of fs.readdirSync(d)) {
      const p = path.join(d, name);
      const rel = prefix ? prefix + '/' + name : name;
      if (fs.statSync(p).isDirectory()) walk(p, rel);
      else out[rel] = fs.readFileSync(p);
    }
  };
  walk(dir, '');
  return out;
}

async function runCart(wasmPath, appDir, frames, opts = {}) {
  const buf = fs.readFileSync(wasmPath);
  const assets = loadAssets(appDir);
  let mem;
  const dec = new TextDecoder();
  const logs = [], marks = [];
  const wasiCalled = new Set();
  const wasi = new Proxy({}, { get: (_t, n) => () => { wasiCalled.add(String(n)); return 0; } });

  const imports = {
    env: {
      wc_log: (p, l) => logs.push(dec.decode(new Uint8Array(mem.buffer, p, l))),
      wc_asset_size: (p, l) => {
        const n = dec.decode(new Uint8Array(mem.buffer, p, l));
        return assets[n] ? assets[n].length : -1;
      },
      wc_load_asset: (p, l, d, max) => {
        const n = dec.decode(new Uint8Array(mem.buffer, p, l));
        if (!assets[n]) return -1;
        const b = assets[n];
        const len = Math.min(b.length, max);
        new Uint8Array(mem.buffer, d, len).set(b.subarray(0, len));
        return len;
      },
      wc_debug_mark: (id) => marks.push(id),
      emscripten_notify_memory_growth: () => {},
    },
    wasi_snapshot_preview1: wasi,
  };

  const { instance } = await WebAssembly.instantiate(buf, imports);
  const e = instance.exports;
  mem = e.memory;

  const infoPtr = e.wc_get_info();
  const dv = () => new DataView(mem.buffer);
  const info = {
    abi: dv().getUint32(infoPtr, true),
    w: dv().getUint32(infoPtr + 4, true),
    h: dv().getUint32(infoPtr + 8, true),
    fbPtr: dv().getUint32(infoPtr + 12, true),
  };

  if (e.wc_set_seed) e.wc_set_seed(opts.seed ?? 0x1234abcd);

  let trap = null;
  const t0 = process.hrtime.bigint();
  try {
    e.wc_init();
    for (let i = 0; i < frames; i++) {
      if (opts.input) opts.input(i, new DataView(mem.buffer), infoPtr);
      e.wc_render();
    }
  } catch (err) {
    trap = err.message;
  }
  const ms = Number(process.hrtime.bigint() - t0) / 1e6;

  // colour histogram over the final frame
  const px = new Uint32Array(mem.buffer, info.fbPtr, info.w * info.h);
  const hist = new Map();
  for (let i = 0; i < px.length; i++) {
    const c = px[i] & 0xffffff;
    hist.set(c, (hist.get(c) || 0) + 1);
  }
  const top = [...hist.entries()].sort((a, b) => b[1] - a[1]).slice(0, 6)
    .map(([c, n]) => ({ color: '#' + c.toString(16).padStart(6, '0'), pct: 100 * n / px.length }));

  // debug fields
  const fields = {};
  if (e.wc_debug_state) {
    let p = e.wc_debug_state();
    const d = dv();
    for (;;) {
      const namePtr = d.getUint32(p, true);
      if (namePtr === 0) break;
      const valPtr = d.getUint32(p + 4, true);
      const type = d.getUint8(p + 8);
      const bytes = new Uint8Array(mem.buffer, namePtr, 64);
      let name = '';
      for (const b of bytes) { if (b === 0) break; name += String.fromCharCode(b); }
      const readers = {
        0: () => d.getUint8(valPtr), 1: () => d.getInt8(valPtr),
        2: () => d.getUint16(valPtr, true), 3: () => d.getInt16(valPtr, true),
        4: () => d.getUint32(valPtr, true), 5: () => d.getInt32(valPtr, true),
        6: () => d.getFloat32(valPtr, true), 7: () => d.getFloat64(valPtr, true),
      };
      fields[name] = readers[type] ? readers[type]() : null;
      p += 16;
    }
  }

  return { info, logs, marks, trap, top, fields, ms, frames,
           fb: new Uint8Array(mem.buffer, info.fbPtr, info.w * info.h * 4).slice(),
           wasi: [...wasiCalled], uniformity: top[0] ? top[0].pct : 100 };
}

// ── assertions ──────────────────────────────────────────────────────
function check(name, results) {
  const problems = [];
  if (results.trap) problems.push(`TRAP: ${results.trap}`);
  if (results.fields.lua_ok === 0) problems.push('lua_ok=0 (Lua error, see logs)');
  if (results.wasi.length) problems.push(`WASI called at runtime: ${results.wasi.join(',')}`);
  if (results.uniformity >= 99.5) {
    problems.push(`frame is ${results.uniformity.toFixed(1)}% one color (blank render?)`);
  }
  const errLogs = results.logs.filter(l => /error|not found|failed|PANIC/i.test(l));
  if (errLogs.length) problems.push(`error logs: ${errLogs.join(' | ')}`);
  return problems;
}

async function main() {
  const only = process.argv[2];
  if (!fs.existsSync(ENGINE)) {
    console.error('engine not built: run runtime/build.sh first');
    process.exit(1);
  }

  const exDir = path.join(ROOT, 'examples');
  let names = fs.readdirSync(exDir).filter(n =>
    fs.existsSync(path.join(exDir, n, 'app', 'main.lua')));
  if (only) names = names.filter(n => n === only);

  let failed = 0;
  console.log('engine:', (fs.statSync(ENGINE).size / 1024).toFixed(1) + ' KB\n');

  for (const name of names) {
    const app = path.join(exDir, name, 'app');
    const r = await runCart(ENGINE, app, 180);
    const problems = check(name, r);
    const fps = (r.frames / (r.ms / 1000)).toFixed(0);
    if (problems.length) {
      failed++;
      console.log(`FAIL  ${name}`);
      for (const p of problems) console.log(`      ${p}`);
      if (r.logs.length) console.log(`      logs: ${r.logs.slice(0, 6).join(' | ')}`);
    } else {
      console.log(`ok    ${name.padEnd(14)} ${String(fps).padStart(6)} fps headless  ` +
        `gc=${r.fields.gc_kb}kb draws=${r.fields.draw_calls}  ` +
        `top=${r.top.slice(0, 3).map(t => t.color + ' ' + t.pct.toFixed(0) + '%').join(' ')}`);
    }
  }

  // ── in-engine unit tests (semantics a pixel histogram can't see) ──
  const unitDir = path.join(ROOT, 'test', 'unit');
  if (fs.existsSync(path.join(unitDir, 'main.lua'))) {
    const ru = await runCart(ENGINE, unitDir, 2);
    const unitFails = ru.fields.score;      // debugValue(0) = fail count
    const unitTotal = ru.fields.aux;        // debugValue(1) = total
    if (ru.trap || ru.fields.lua_ok === 0) {
      console.log(`\nFAIL  unit tests did not run: ${ru.trap || 'lua error'}`);
      for (const l of ru.logs.slice(0, 10)) console.log(`      ${l}`);
      failed++;
    } else if (unitFails > 0) {
      console.log(`\nFAIL  unit  ${unitFails}/${unitTotal} assertions failed`);
      for (const l of ru.logs.filter(l => l.startsWith('FAIL'))) console.log(`      ${l}`);
      failed++;
    } else {
      console.log(`\nok    unit         ${unitTotal} assertions passed in-engine`);
    }
  }

  // ── the rasterizer must not change, ever ──────────────────────────
  // determinism.js does NOT cover this, and neither cart is redundant:
  //
  //   blit/   the sprite blitter. determinism.js passed for an entire
  //           session while the blitter sampled wrong texels, because its
  //           carts draw shapes and text and never a sprite.
  //   prims/  the vector primitives. During the primitive optimization, a
  //           control that corrupted every ALPHA line pixel changed nothing
  //           in any of the 11 carts then in the suite -- alpha lines were
  //           drawn by none of them. Both controls now fail here.
  //
  // Each hashes a frame that drives its primitives through every branch the
  // optimizer hoists a decision out of: opaque, alpha, additive, scissored,
  // and canvas destination.
  for (const [name, why] of [['blit', 'sprite output'], ['prims', 'primitive output']]) {
    const dir = path.join(ROOT, 'test', name);
    const golden = path.join(dir, 'golden.txt');
    if (!fs.existsSync(path.join(dir, 'main.lua'))) continue;
    const rb2 = await runCart(ENGINE, dir, 3);
    if (rb2.trap || rb2.fields.lua_ok === 0) {
      console.log(`\nFAIL  ${name}  cart did not run: ${rb2.trap || 'lua error'}`);
      for (const l of rb2.logs.slice(0, 6)) console.log(`      ${l}`);
      failed++;
      continue;
    }
    const crypto = require('crypto');
    const hash = crypto.createHash('sha256')
      .update(Buffer.from(rb2.fb)).digest('hex').slice(0, 16);
    if (!fs.existsSync(golden)) {
      fs.writeFileSync(golden, hash + '\n');
      console.log(`\nok    ${name.padEnd(12)} golden recorded (${hash})`);
    } else {
      const want = fs.readFileSync(golden, 'utf8').trim();
      if (hash === want) {
        console.log(`\nok    ${name.padEnd(12)} ${why} unchanged (${hash})`);
      } else {
        console.log(`\nFAIL  ${name}  ${why} CHANGED`);
        console.log(`      golden ${want}`);
        console.log(`      got    ${hash}`);
        console.log(`      if this change is intended, delete test/${name}/golden.txt`);
        failed++;
      }
    }
  }

  // ── the GL display path must show what the cart drew ──────────────
  // Only when a GL context is available on this machine. wc_gl_blit changes only HOW pixels reach the
  // screen, so the GPU's output must equal the software framebuffer exactly;
  // anything else means the display path is misreporting the cart.
  const glEngine = GL_ENGINE;
  if (fs.existsSync(glEngine)) {
    const { execFileSync } = require('child_process');
    try {
      const out = execFileSync(process.execPath,
        [path.join(ROOT, 'tools', 'gl-verify.mjs'), glEngine, path.join(ROOT, 'test', 'prims'), '3'],
        { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
      const m = out.match(/(\d+) differing pixels/);
      console.log(`\nok    gl-display   GPU output identical to software (${m ? m[1] : '0'} differing)`);
    } catch (err) {
      const txt = (err.stdout || '') + (err.stderr || '');
      if (/Cannot find|ERR_MODULE_NOT_FOUND|createWebGL2Context/.test(txt)) {
        console.log('\nskip  gl-display   no GL context available on this machine');
      } else {
        console.log('\nFAIL  gl-display  GPU output differs from the software framebuffer');
        for (const l of txt.trim().split('\n').slice(-4)) console.log(`      ${l}`);
        failed++;
      }
    }
  }

  // ── the GL2D backend must stay within tolerance ───────────────────
  // GL2D is deliberately NOT bit-exact: fixed-function blending rounds
  // differently from div255, by exactly 1 per blended draw, and that
  // compounds where draws overlap (4 stacked layers measured at 2). The
  // gate is therefore a tolerance, not equality -- and test/gl2d/ uses only
  // primitives the GL path implements, because a cart that trips the sticky
  // CPU fallback measures the software path on both engines and would
  // report a perfect match while proving nothing.
  for (const cart of ['gl2d', 'gl2dcanvas', 'gl2dtext']) {
    if (!fs.existsSync(glEngine)) break;
    if (!fs.existsSync(path.join(ROOT, 'test', cart, 'main.lua'))) continue;
    const { execFileSync } = require('child_process');
    try {
      const out = execFileSync(process.execPath,
        [path.join(ROOT, 'tools', 'gl2d-compare.mjs'), path.join(ROOT, 'test', cart), '3', '2'],
        { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
      // Report the two budgets separately. "max delta" alone is misleading:
      // it is dominated by a handful of rotated-sprite EDGE pixels, which
      // are either the sprite or the background and so differ hugely in
      // value while being visually correct.
      const edge = out.match(/(\d+) pixels \(([\d.]+)%\) exceed/);
      console.log(edge
        ? `\nok    ${cart.padEnd(12)} within tolerance (${edge[2]}% edge pixels, budget 0.05%)`
        : `\nok    ${cart.padEnd(12)} within tolerance (every pixel +/-2)`);
    } catch (err) {
      const txt = (err.stdout || '') + (err.stderr || '');
      if (/Cannot find|ERR_MODULE_NOT_FOUND|createWebGL2Context/.test(txt)) {
        console.log(`\nskip  ${cart.padEnd(12)} no GL context available on this machine`);
      } else {
        console.log(`\nFAIL  ${cart}  GL2D output drifted beyond tolerance`);
        for (const l of txt.trim().split('\n').slice(-6)) console.log(`      ${l}`);
        failed++;
      }
    }
  }

  // ── documented examples must actually run ─────────────────────────
  // Docs rot silently: an API gets renamed and the README keeps promising
  // the old one. test/doccheck/ is every code block from README.md and
  // docs/api.md, executed for real.
  const docDir = path.join(ROOT, 'test', 'doccheck');
  if (fs.existsSync(path.join(docDir, 'main.lua'))) {
    const rd = await runCart(ENGINE, docDir, 2);
    const blocks = rd.fields.score;
    const failLines = rd.logs.filter(l => l.startsWith('DOC FAIL'));
    if (rd.trap || rd.fields.lua_ok === 0 || failLines.length) {
      console.log(`\nFAIL  doccheck  ${failLines.length} documented example(s) broken`);
      for (const l of failLines) console.log(`      ${l}`);
      if (rd.trap) console.log(`      trap: ${rd.trap}`);
      failed++;
    } else {
      console.log(`\nok    doccheck     ${blocks} documented code blocks run clean`);
    }
  }

  // ── the control that MUST fail ────────────────────────────────────
  // If this passes, the harness cannot see errors and every green above
  // is meaningless.
  const bad = path.join(ROOT, 'test', 'broken');
  fs.mkdirSync(bad, { recursive: true });
  fs.writeFileSync(path.join(bad, 'main.lua'),
    'function love.draw() this_function_does_not_exist() end\n');
  const rb = await runCart(ENGINE, bad, 10);
  const badProblems = check('broken', rb);
  if (badProblems.length === 0) {
    console.log('\nFAIL  control: the deliberately-broken cart PASSED.');
    console.log('      the harness cannot detect failure; treat all results above as unverified.');
    failed++;
  } else {
    console.log(`\nok    control      broken cart correctly detected (${badProblems[0].slice(0, 60)})`);
  }
  fs.rmSync(bad, { recursive: true, force: true });

  console.log(failed ? `\n${failed} FAILED` : '\nall green');
  process.exit(failed ? 1 : 0);
}

main().catch(e => { console.error(e); process.exit(1); });
