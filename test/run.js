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
 *   node test/run.js ping       # one example
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
  const logs = [], marks = [], rumbles = [];
  // pads the fake host reports as rumble-capable (0-based ids)
  const rumbleCapable = new Set(opts.rumblePads ?? []);
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
      // Rumble is write-only at the ABI, so recording the calls is the only
      // way a test can see what the cart asked the host to do.
      wc_pad_has_rumble: (id) => (rumbleCapable.has(id) ? 1 : 0),
      wc_pad_rumble: (id, low, high, ms) => rumbles.push({ id, low, high, ms }),
      wc_pad_rumble_stop: (id) => rumbles.push({ id, stop: true }),
      // Peer networking, in the OFFLINE configuration: every open is refused
      // and there are no peers. That is a supported host, not a degenerate
      // one, and running the whole example suite against it is what proves
      // no cart depends on the network to boot or draw. The real networking
      // assertions need a real socket and live in test/net.mjs.
      wc_peer_open: () => -1,
      wc_peer_close: () => {},
      wc_peer_send: () => -1,
      wc_peer_broadcast: () => 0,
      wc_peer_state: () => 3,   // WC_PEER_CLOSED
      wc_peer_count: () => 0,
      wc_peer_id: () => -1,
      wc_peer_name: () => -1,
      wc_peer_transport: () => 0,
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
    // Re-read the info struct, exactly as the real host does: a cart may pick
    // its resolution during wc_init (conf.lua), and reading the pre-init
    // values here would score the histogram over the wrong extent.
    info.w = dv().getUint32(infoPtr + 4, true);
    info.h = dv().getUint32(infoPtr + 8, true);
    info.fbPtr = dv().getUint32(infoPtr + 12, true);
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

  return { info, logs, marks, rumbles, trap, top, fields, ms, frames,
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

  // runtime/wasmcart.h is a copy of the spec's header, and a quoted #include
  // makes the copy win over build.sh's -I "$WASMCART_REPO/include" regardless
  // of flag order. A stale copy therefore builds the engine against the old
  // ABI with no warning, so check it before anything else runs.
  {
    const { spawnSync } = require('child_process');
    const r = spawnSync('node', [path.join(ROOT, 'tools', 'abi-drift.mjs')],
      { encoding: 'utf8' });
    process.stdout.write(r.stdout || '');
    process.stderr.write(r.stderr || '');
    if (r.status !== 0) failed++;
  }

  // Examples that CANNOT run on the CPU comparator, because the feature they
  // demonstrate is a GPU program. `shaders` calls newShader, which refuses on
  // a host with no GL rather than pretending -- so running it against
  // engine-cpu.wasm would report a Lua error for behaving correctly. It is
  // gated separately below, against a real GL context.
  // `mesh` joins it for the same reason: a mesh is GPU geometry, newMesh
  // refuses on a host with no GL rather than pretending, and the software
  // rasterizer has no textured per-vertex-coloured triangle to fall back to.
  const GL_ONLY_EXAMPLES = new Set(['shaders', 'mesh']);

  for (const name of names) {
    if (GL_ONLY_EXAMPLES.has(name)) {
      console.log(`(gl)  ${name.padEnd(14)} GL-only example; gated by the shader section below`);
      continue;
    }
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

  // ── b3 surface material / damping / sleep / contact events ────────
  // Behavioural, not just presence: each case is one a pre-extension
  // binding passes vacuously (restitution 0.9 and 0.0 bounced identically
  // because neither was reachable from Lua).
  const b3Dir = path.join(ROOT, 'test', 'b3mat');
  if (fs.existsSync(path.join(b3Dir, 'main.lua'))) {
    const rb = await runCart(ENGINE, b3Dir, 2);
    const bFails = rb.fields.score;
    const bTotal = rb.fields.aux;
    if (rb.trap || rb.fields.lua_ok === 0) {
      console.log(`\nFAIL  b3mat did not run: ${rb.trap || 'lua error'}`);
      for (const l of rb.logs.slice(0, 12)) console.log(`      ${l}`);
      failed++;
    } else if (bFails > 0) {
      console.log(`\nFAIL  b3mat  ${bFails}/${bTotal} assertions failed`);
      for (const l of rb.logs.filter(l => l.startsWith('FAIL'))) console.log(`      ${l}`);
      failed++;
    } else {
      console.log(`ok    b3mat        ${bTotal} physics assertions passed in-engine`);
    }
  }

  // ── conf.lua resolution selection ─────────────────────────────────
  // A cart that ships conf.lua picks its own resolution (up to 1920x1080);
  // everything else stays at the 1280x720 default (the unit cart above and
  // every example assert that side). This cart asks for 1600x900 and the
  // checks are ordered to localize a break: reported dims first, then the
  // cart's own Lua-visible dims, then a corner-pixel probe that only passes
  // if the framebuffer STRIDE matches the reported width.
  {
    const rc = await runCart(ENGINE, path.join(ROOT, 'test', 'confres'), 2);
    const w = rc.info.w, h = rc.info.h;
    const px = new Uint32Array(rc.fb.buffer, rc.fb.byteOffset, w * h);
    const corner = px[(h - 1) * w + (w - 1)] & 0xffffff;   // inside the marker
    const inside = px[850 * w + 1550] & 0xffffff;          // marker center
    const bg     = px[0] & 0xffffff;                       // top-left: background
    if (rc.trap || rc.fields.lua_ok === 0) {
      console.log(`\nFAIL  confres      did not run: ${rc.trap || 'lua error'}`);
      for (const l of rc.logs.slice(0, 10)) console.log(`      ${l}`);
      failed++;
    } else if (w !== 1600 || h !== 900) {
      console.log(`\nFAIL  confres      conf.lua ignored: info reports ${w}x${h}, want 1600x900`);
      failed++;
    } else if (rc.fields.score > 0) {
      console.log(`\nFAIL  confres      ${rc.fields.score}/${rc.fields.aux} in-cart assertions failed`);
      for (const l of rc.logs.filter(l => l.includes('FAIL'))) console.log(`      ${l}`);
      failed++;
    } else if (corner !== inside || corner === bg) {
      console.log(`\nFAIL  confres      stride mismatch: corner=#${corner.toString(16)} ` +
        `marker=#${inside.toString(16)} bg=#${bg.toString(16)}`);
      failed++;
    } else {
      console.log(`ok    confres      cart chose 1600x900; stride and dims agree`);
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

  // ── the CPU-fallback blit must show what the cart drew ────────────
  // When a frame falls back, the software rasterizer draws it and wc_gl_blit
  // presents it as a fullscreen quad, so the GPU's output must equal the
  // cart's framebuffer EXACTLY -- the blit changes only how pixels reach the
  // screen, not what they are.
  //
  // This needs a cart that actually falls back. It used to use test/prims,
  // which fell back because it drew circles; now that circles render on the
  // GPU, prims stays on GL, its software framebuffer is empty, and the gate
  // reported "the cart drew nothing". Point it at a cart that still falls
  // back for a reason GL2D does not implement -- a concave polygon fill.
  const glEngine = GL_ENGINE;
  if (fs.existsSync(glEngine)) {
    const { execFileSync } = require('child_process');
    try {
      const out = execFileSync(process.execPath,
        [path.join(ROOT, 'tools', 'gl-verify.mjs'), glEngine, path.join(ROOT, 'test', 'fallback'), '3'],
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
  // Per-cart tolerance. Additive gets more: it ACCUMULATES rounding rather
  // than converging the way alpha blending does, and the GPU is actually the
  // more accurate of the two there (div255 truncates every additive step).
  for (const [cart, tol] of [['gl2d', '2'], ['gl2dcanvas', '2'],
                             ['gl2dtext', '2'], ['gl2dblend', '8'],
                             ['gl2dpoly', '2'], ['gl2dcircle', '2']]) {
    if (!fs.existsSync(glEngine)) break;
    if (!fs.existsSync(path.join(ROOT, 'test', cart, 'main.lua'))) continue;
    const { execFileSync } = require('child_process');
    try {
      const out = execFileSync(process.execPath,
        [path.join(ROOT, 'tools', 'gl2d-compare.mjs'), path.join(ROOT, 'test', cart), '3', tol],
        { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
      // Report the two budgets separately. "max delta" alone is misleading:
      // it is dominated by a handful of rotated-sprite EDGE pixels, which
      // are either the sprite or the background and so differ hugely in
      // value while being visually correct.
      const edge = out.match(/(\d+) pixels \(([\d.]+)%\) exceed/);
      console.log(edge
        ? `\nok    ${cart.padEnd(12)} within tolerance (${edge[2]}% edge pixels, budget 0.05%)`
        : `\nok    ${cart.padEnd(12)} within tolerance (every pixel +/-${tol})`);
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

  // ── custom shaders ────────────────────────────────────────────────
  //
  // Three gates, because each catches a failure the others cannot see:
  //
  //  1. gl-shader-verify on examples/shaders -- did the shader run, and did
  //     it produce the RIGHT colour? A shader that links but samples the
  //     wrong thing still "differs from unshaded", so difference alone is
  //     not evidence; the probes assert the true inverse.
  //  2. the same tool on a copy with setShader commented out, which MUST
  //     fail. A gate that has never been seen red is not a gate.
  //  3. test/shaderfail in-engine, which asserts newShader REFUSES four
  //     differently-broken shaders while still accepting a good one.
  if (fs.existsSync(glEngine)) {
    const { execFileSync } = require('child_process');
    const os = require('os');
    const tool = path.join(ROOT, 'tools', 'gl-shader-verify.mjs');
    const shaderEx = path.join(ROOT, 'examples', 'shaders');
    const runTool = (dir) => execFileSync(process.execPath, [tool, glEngine, dir, '3'],
      { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });

    if (fs.existsSync(shaderEx)) {
      let glMissing = false;
      try {
        const out = runTool(shaderEx);
        const m = out.match(/all (\d+) probes inverted/);
        console.log(`\nok    shaders      custom shader verified on ${m ? m[1] : '?'} draw paths ` +
                    '(solids, circle, sprite)');
      } catch (err) {
        const txt = (err.stdout || '') + (err.stderr || '');
        if (/Cannot find|ERR_MODULE_NOT_FOUND|createWebGL2Context/.test(txt)) {
          console.log('\nskip  shaders      no GL context available on this machine');
          glMissing = true;
        } else {
          console.log('\nFAIL  shaders      the custom shader did not run, or ran wrong');
          for (const l of txt.trim().split('\n').slice(-6)) console.log(`      ${l}`);
          failed++;
        }
      }

      // the control: the SAME cart with the shader never bound must be
      // caught. If this passes, the gate above is blind and means nothing.
      if (!glMissing) {
        const ctl = fs.mkdtempSync(path.join(os.tmpdir(), 'wcl-shader-ctl-'));
        fs.cpSync(path.join(shaderEx, 'app'), path.join(ctl, 'app'), { recursive: true });
        const mainPath = path.join(ctl, 'app', 'main.lua');
        fs.writeFileSync(mainPath, fs.readFileSync(mainPath, 'utf8')
          .replace('love.graphics.setShader(invert)', '-- control: not bound'));
        let caught = false;
        try { runTool(ctl); } catch { caught = true; }
        fs.rmSync(ctl, { recursive: true, force: true });
        if (caught) {
          console.log('ok    shader-ctl   unshaded control correctly detected');
        } else {
          console.log('FAIL  shader-ctl   the control PASSED: the shader gate cannot see failure');
          failed++;
        }
      }
    }
  }

  // ── meshes ────────────────────────────────────────────────────────
  //
  // Same three-gate shape as shaders, for the same reason: a mesh that never
  // draws leaves the frame count perfect and a hole in the screen, and the
  // uniformity check above cannot see that because examples/mesh draws
  // plenty of text and sprites besides.
  //
  //  1. gl-mesh-verify on examples/mesh. Its strongest assertion is the
  //     ATLAS UV REMAP: the cart draws a textured mesh and the SAME image as
  //     an ordinary sprite directly beneath it, and the two must agree pixel
  //     for pixel. A mesh's uv is 0..1 over its own image while sprites live
  //     in a shared 2048^2 atlas, so the remap is the one piece of arithmetic
  //     that is easy to get wrong and hard to see -- a wrong offset renders a
  //     crop of a NEIGHBOURING image, which still looks like "a mesh drew".
  //     It also asserts per-vertex colour interpolates, and that "triangles"
  //     and "fan" differ on an identical vertex list.
  //  2. a control copy with the mesh draw removed, which MUST fail.
  //  3. test/meshfail in-engine: newMesh must REFUSE what this engine cannot
  //     express, rather than silently approximating it.
  if (fs.existsSync(glEngine)) {
    const { execFileSync } = require('child_process');
    const os = require('os');
    const tool = path.join(ROOT, 'tools', 'gl-mesh-verify.mjs');
    const meshEx = path.join(ROOT, 'examples', 'mesh');
    const runTool = (dir) => execFileSync(process.execPath, [tool, glEngine, dir, '3'],
      { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });

    if (fs.existsSync(meshEx)) {
      let glMissing = false;
      try {
        runTool(meshEx);
        console.log('\nok    mesh         textured mesh matches its sprite reference ' +
                    '(atlas uv remap exact)');
      } catch (err) {
        const txt = (err.stdout || '') + (err.stderr || '');
        if (/Cannot find|ERR_MODULE_NOT_FOUND|createWebGL2Context/.test(txt)) {
          console.log('\nskip  mesh         no GL context available on this machine');
          glMissing = true;
        } else {
          console.log('\nFAIL  mesh         the mesh did not render, or rendered wrong');
          for (const l of txt.trim().split('\n').slice(-8)) console.log(`      ${l}`);
          failed++;
        }
      }

      // the control: the SAME cart with the textured mesh never drawn. If
      // this passes, the gate above is blind.
      if (!glMissing) {
        const ctl = fs.mkdtempSync(path.join(os.tmpdir(), 'wcl-mesh-ctl-'));
        fs.cpSync(path.join(meshEx, 'app'), path.join(ctl, 'app'), { recursive: true });
        const mainPath = path.join(ctl, 'app', 'main.lua');
        fs.writeFileSync(mainPath, fs.readFileSync(mainPath, 'utf8')
          .replace('love.graphics.draw(texMesh, 40, 56)', '-- control: not drawn'));
        let caught = false;
        try { runTool(ctl); } catch { caught = true; }
        fs.rmSync(ctl, { recursive: true, force: true });
        if (caught) {
          console.log('ok    mesh-ctl     undrawn control correctly detected');
        } else {
          console.log('FAIL  mesh-ctl     the control PASSED: the mesh gate cannot see failure');
          failed++;
        }
      }
    }
  }

  // ── 3D ────────────────────────────────────────────────────────────
  //
  // test/g3d runs groverburger's g3d with its sources copied VERBATIM from
  // upstream, so this gate is not "does 3D work" but "does an unmodified
  // third-party LOVE 3D library still run". It covers a wide slice of the
  // LOVE surface at once -- custom vertex formats, setDepthMode,
  // package.loaded, getCanvas, flat-16 mat4 sends, GLSL ES 1.00 rewriting --
  // and any one of them regressing turns it red.
  //
  // The gate's own control is INTERNAL (it re-renders with depth disabled
  // and requires the frames to differ), so the control below is a second,
  // coarser one: with the 3D draws removed the gate must fail outright.
  if (fs.existsSync(glEngine) &&
      fs.existsSync(path.join(ROOT, 'test', 'g3d', 'app', 'main.lua'))) {
    const { execFileSync } = require('child_process');
    const os = require('os');
    const tool = path.join(ROOT, 'tools', 'gl-3d-verify.mjs');
    const g3dDir = path.join(ROOT, 'test', 'g3d');
    const run3d = (dir) => execFileSync(process.execPath, [tool, glEngine, dir, '3'],
      { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });

    let glMissing = false;
    try {
      const out = run3d(g3dDir);
      const m = out.match(/depth off\)/) ? out.match(/\((\d+) px change/) : null;
      console.log('\nok    g3d          unmodified g3d renders in perspective; ' +
                  `depth occludes (${m ? m[1] : '?'} px)`);
    } catch (err) {
      const txt = (err.stdout || '') + (err.stderr || '');
      if (/Cannot find|ERR_MODULE_NOT_FOUND|createWebGL2Context/.test(txt)) {
        console.log('\nskip  g3d          no GL context available on this machine');
        glMissing = true;
      } else {
        console.log('\nFAIL  g3d          3D did not render, or depth is inert');
        for (const l of txt.trim().split('\n').slice(-10)) console.log(`      ${l}`);
        failed++;
      }
    }

    if (!glMissing) {
      const ctl = fs.mkdtempSync(path.join(os.tmpdir(), 'wcl-g3d-ctl-'));
      fs.cpSync(path.join(g3dDir, 'app'), path.join(ctl, 'app'), { recursive: true });
      const mainPath = path.join(ctl, 'app', 'main.lua');
      fs.writeFileSync(mainPath, fs.readFileSync(mainPath, 'utf8')
        .replace('    nearCube:draw()\n    farCube:draw()\n',
                 '    -- control: the 3D draws removed\n'));
      let caught = false;
      try { run3d(ctl); } catch { caught = true; }
      fs.rmSync(ctl, { recursive: true, force: true });
      if (caught) {
        console.log('ok    g3d-ctl      undrawn control correctly detected');
      } else {
        console.log('FAIL  g3d-ctl      the control PASSED: the 3D gate cannot see failure');
        failed++;
      }
    }
  }

  // ── the deferred-rendering capabilities ───────────────────────────
  //
  // MRT, float canvases, cube/array/volume textures, instancing and the
  // colour mask. Every one of these can FAIL SILENTLY -- a float target that
  // was quietly created as 8-bit still binds and still draws, MRT that
  // broadcasts one colour to every attachment still "works" -- so the gate
  // reads pixels rather than trusting that the calls returned.
  //
  // The controls below break each capability in the way it actually breaks
  // in the wild, and each must turn the gate red.
  if (fs.existsSync(glEngine) &&
      fs.existsSync(path.join(ROOT, 'test', 'gpu3d', 'app', 'main.lua'))) {
    const { execFileSync } = require('child_process');
    const os = require('os');
    const tool = path.join(ROOT, 'tools', 'gl-gpu3d-verify.mjs');
    const dir = path.join(ROOT, 'test', 'gpu3d');
    const runGpu = (d) => execFileSync(process.execPath, [tool, glEngine, d, '3'],
      { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });

    let glMissing = false;
    try {
      runGpu(dir);
      console.log('\nok    gpu3d        MRT, float canvas, colour mask, ' +
                  'instancing, cube faces verified by pixel');
    } catch (err) {
      const txt = (err.stdout || '') + (err.stderr || '');
      if (/Cannot find|ERR_MODULE_NOT_FOUND|createWebGL2Context/.test(txt)) {
        console.log('\nskip  gpu3d        no GL context available on this machine');
        glMissing = true;
      } else {
        console.log('\nFAIL  gpu3d        a GPU capability is not doing what it claims');
        for (const l of txt.trim().split('\n').slice(-12)) console.log(`      ${l}`);
        failed++;
      }
    }

    // Four controls, one per failure mode. Each edit is the real-world bug:
    // a format silently downgraded, a mask that does nothing, instancing
    // ignored, cube faces wired up in the wrong order.
    if (!glMissing) {
      const CONTROLS = [
        ['gpu3d-ctl-float', 'format = "rgba16f" })\n    local b', 'format = "rgba8" })\n    local b'],
        ['gpu3d-ctl-mask', 'setColorMask(true, false, true, true)',
                           'setColorMask(true, true, true, true)'],
        ['gpu3d-ctl-inst', 'love.graphics.drawInstanced, quad, 8',
                           'love.graphics.drawInstanced, quad, 1'],
        ['gpu3d-ctl-cube', 'faces[i] = "face" .. i .. ".png"',
                           'faces[i] = "face" .. ((i == 1) and 2 or (i == 2) and 1 or i) .. ".png"'],
      ];
      for (const [name, from, to] of CONTROLS) {
        const ctl = fs.mkdtempSync(path.join(os.tmpdir(), 'wcl-' + name + '-'));
        fs.cpSync(path.join(dir, 'app'), path.join(ctl, 'app'), { recursive: true });
        const mainPath = path.join(ctl, 'app', 'main.lua');
        const src = fs.readFileSync(mainPath, 'utf8');
        if (!src.includes(from)) {
          console.log(`FAIL  ${name.padEnd(12)} the control's anchor text is gone; ` +
                      'the control is not testing anything');
          failed++;
          fs.rmSync(ctl, { recursive: true, force: true });
          continue;
        }
        fs.writeFileSync(mainPath, src.replace(from, to));
        let caught = false;
        try { runGpu(ctl); } catch { caught = true; }
        fs.rmSync(ctl, { recursive: true, force: true });
        if (caught) {
          console.log(`ok    ${name.padEnd(12)} broken capability correctly detected`);
        } else {
          console.log(`FAIL  ${name.padEnd(12)} the control PASSED: this gate is blind`);
          failed++;
        }
      }
    }
  }

  // ── 3DreamEngine ──────────────────────────────────────────────────
  //
  // The heaviest LOVE 3D library there is, running with its sources copied
  // verbatim. It exercises a chain nothing else covers: the ffi shim packs
  // its vertex buffer, a DECLARED vertex format carries VertexTangent and a
  // 4-component position, the vertices and index buffer arrive as packed
  // ByteData, and its .obj loader runs on the asset-index directory
  // listing. Any one of those regressing turns this red.
  if (fs.existsSync(glEngine) &&
      fs.existsSync(path.join(ROOT, 'test', 'dream3d', 'app', 'main.lua'))) {
    const { execFileSync } = require('child_process');
    const os = require('os');
    const tool = path.join(ROOT, 'tools', 'gl-dream3d-verify.mjs');
    const dir = path.join(ROOT, 'test', 'dream3d');
    const run3 = (d) => execFileSync(process.execPath, [tool, glEngine, d, '15'],
      { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });

    let glMissing = false;
    try {
      const out = run3(dir);
      const m = out.match(/\((\d+) px, (\d+) shades\)/);
      console.log('\nok    dream3d      3DreamEngine renders its own model ' +
                  (m ? `(${m[1]} px, ${m[2]} shades)` : ''));
    } catch (err) {
      const txt = (err.stdout || '') + (err.stderr || '');
      if (/Cannot find|ERR_MODULE_NOT_FOUND|createWebGL2Context/.test(txt)) {
        console.log('\nskip  dream3d      no GL context available on this machine');
        glMissing = true;
      } else {
        console.log('\nFAIL  dream3d      3DreamEngine did not render correctly');
        for (const l of txt.trim().split('\n').slice(-10)) console.log(`      ${l}`);
        failed++;
      }
    }

    // Two controls: geometry removed (nothing renders), and the normal
    // attribute ignored (a model renders, FLAT). The second is the one that
    // matters -- it is the failure a silhouette check cannot see.
    if (!glMissing) {
      const CONTROLS = [
        ['dream3d-ctl', '  love.graphics.draw(mesh)', '  -- control: not drawn'],
        ['dream3d-ctl-n',
         '      vec3 n = normalize(VertexNormal);\n      return vec4(n * 0.5 + 0.5, 1.0);',
         '      return vec4(0.8, 0.7, 0.6, 1.0);'],
      ];
      for (const [name, from, to] of CONTROLS) {
        const ctl = fs.mkdtempSync(path.join(os.tmpdir(), 'wcl-' + name + '-'));
        fs.cpSync(path.join(dir, 'app'), path.join(ctl, 'app'), { recursive: true });
        const mainPath = path.join(ctl, 'app', 'main.lua');
        const src = fs.readFileSync(mainPath, 'utf8');
        if (!src.includes(from)) {
          console.log(`FAIL  ${name.padEnd(12)} the control's anchor text is gone`);
          failed++;
          fs.rmSync(ctl, { recursive: true, force: true });
          continue;
        }
        fs.writeFileSync(mainPath, src.replace(from, to));
        let caught = false;
        try { run3(ctl); } catch { caught = true; }
        fs.rmSync(ctl, { recursive: true, force: true });
        if (caught) {
          console.log(`ok    ${name.padEnd(12)} broken pipeline correctly detected`);
        } else {
          console.log(`FAIL  ${name.padEnd(12)} the control PASSED: this gate is blind`);
          failed++;
        }
      }
    }
  }

  // ── texture mapping ON A MODEL ────────────────────────────────────
  //
  // Separate from the g3d and dream3d gates because neither asserts it: a
  // textured quad has no perspective interpolation, no minification and a
  // trivial uv layout, and a model that samples ONE texel per triangle
  // renders clean and flat -- which reads as success at a glance.
  if (fs.existsSync(glEngine) &&
      fs.existsSync(path.join(ROOT, 'test', 'tex3d', 'app', 'main.lua'))) {
    const { execFileSync } = require('child_process');
    const os = require('os');
    const tool = path.join(ROOT, 'tools', 'gl-tex3d-verify.mjs');
    const dir = path.join(ROOT, 'test', 'tex3d');
    const runT = (d) => execFileSync(process.execPath, [tool, glEngine, d, '25'],
      { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });

    let glMissing = false;
    try {
      const out = runT(dir);
      const m = out.match(/\((\d+) texel shades, (\d+) checker/);
      console.log('\nok    tex3d        texture mapping on a 3D model ' +
                  (m ? `(${m[1]} shades, ${m[2]} transitions)` : ''));
    } catch (err) {
      const txt = (err.stdout || '') + (err.stderr || '');
      if (/Cannot find|ERR_MODULE_NOT_FOUND|createWebGL2Context/.test(txt)) {
        console.log('\nskip  tex3d        no GL context available on this machine');
        glMissing = true;
      } else {
        console.log('\nFAIL  tex3d        texture mapping is wrong on a model');
        for (const l of txt.trim().split('\n').slice(-10)) console.log(`      ${l}`);
        failed++;
      }
    }

    // The control is the failure that looks like success: the model still
    // renders, lit and clean, but every fragment samples the same texel.
    if (!glMissing) {
      const ctl = fs.mkdtempSync(path.join(os.tmpdir(), 'wcl-tex3d-ctl-'));
      fs.cpSync(path.join(dir, 'app'), path.join(ctl, 'app'), { recursive: true });
      const mainPath = path.join(ctl, 'app', 'main.lua');
      const src = fs.readFileSync(mainPath, 'utf8');
      const from = 'return vec4(Texel(t, uv).rgb * l, 1.0);';
      if (!src.includes(from)) {
        console.log("FAIL  tex3d-ctl    the control's anchor text is gone");
        failed++;
      } else {
        fs.writeFileSync(mainPath, src.replace(from,
          'return vec4(Texel(t, vec2(0.3, 0.3)).rgb * l, 1.0);'));
        let caught = false;
        try { runT(ctl); } catch { caught = true; }
        if (caught) {
          console.log('ok    tex3d-ctl    single-texel control correctly detected');
        } else {
          console.log('FAIL  tex3d-ctl    the control PASSED: this gate is blind');
          failed++;
        }
      }
      fs.rmSync(ctl, { recursive: true, force: true });
    }
  }

  // A mesh cannot ride the quad batcher, so each one is its own
  // glDrawArrays. That is the accepted trade; this keeps it from getting
  // worse. 12 meshes must be 12 draws, not 24, and must never re-bind the
  // program.
  if (fs.existsSync(glEngine) &&
      fs.existsSync(path.join(ROOT, 'test', 'meshcost', 'main.lua'))) {
    const { execFileSync } = require('child_process');
    try {
      const out = execFileSync(process.execPath,
        [path.join(ROOT, 'tools', 'gl-call-count.mjs'), glEngine,
         path.join(ROOT, 'test', 'meshcost'), '10',
         '--max-useprogram', '0', '--max-drawarrays', '12'],
        { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
      const m = out.match(/TOTAL\s+([\d.]+)/);
      console.log(`ok    mesh-cost    12 meshes = 12 draws, 0 glUseProgram ` +
                  `(${m ? m[1] : '?'} GL calls/frame total)`);
    } catch (err) {
      const txt = (err.stdout || '') + (err.stderr || '');
      if (/Cannot find|ERR_MODULE_NOT_FOUND|createWebGL2Context/.test(txt)) {
        console.log('skip  mesh-cost    no GL context available on this machine');
      } else {
        console.log('FAIL  mesh-cost    a mesh draw costs more GL calls than it should');
        for (const l of txt.trim().split('\n').slice(-4)) console.log(`      ${l}`);
        failed++;
      }
    }
  }

  // newMesh must REFUSE what this engine cannot express -- a custom vertex
  // format, the "points" draw mode, an unknown mode or usage -- rather than
  // approximate it. Run against a REAL GL context: without one every newMesh
  // is refused for having no GL at all, which would make the gate green
  // without the actual refusal paths ever being reached.
  const mfDir = path.join(ROOT, 'test', 'meshfail');
  if (fs.existsSync(path.join(mfDir, 'main.lua')) && fs.existsSync(glEngine)) {
    const { execFileSync } = require('child_process');
    try {
      const out = execFileSync(process.execPath,
        [path.join(ROOT, 'tools', 'gl-mesh-verify.mjs'), '--logs', glEngine, mfDir, '3'],
        { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
      const lines = out.split('\n');
      const refusals = lines.filter(l => /^LOG: REFUSED/.test(l)).length;
      const leaks = lines.filter(l => /^LOG: ACCEPTED/.test(l));
      // the 1-based/0-based index round-trips. An off-by-one there is
      // invisible on screen (a mesh drawn from vertex 2 still looks like a
      // mesh), so it has to be asserted by value.
      const badValues = lines.filter(l => /^LOG: BADVALUE/.test(l));
      const semantics = lines.some(l => /^LOG: ok mesh index semantics/.test(l));
      const noGl = lines.some(l => /software rasterizer/.test(l));
      if (noGl) {
        console.log('\nFAIL  meshfail     ran without GL, so the refusals prove nothing');
        failed++;
      } else if (badValues.length) {
        console.log('\nFAIL  meshfail     mesh index/getter semantics are wrong:');
        for (const l of badValues) console.log(`      ${l}`);
        failed++;
      } else if (!semantics) {
        console.log('\nFAIL  meshfail     the index-semantics block never ran ' +
                    '(the cart errored partway through)');
        for (const l of lines.slice(-6)) console.log(`      ${l}`);
        failed++;
      } else if (leaks.length) {
        console.log('\nFAIL  meshfail     newMesh ACCEPTED something it cannot render:');
        for (const l of leaks) console.log(`      ${l}`);
        failed++;
      } else if (refusals < 9) {
        console.log(`\nFAIL  meshfail     only ${refusals} refusals, expected >= 9`);
        for (const l of lines.slice(0, 12)) console.log(`      ${l}`);
        failed++;
      } else {
        console.log(`\nok    meshfail     ${refusals} unsupported mesh forms refused loudly`);
      }
    } catch (err) {
      const txt = (err.stdout || '') + (err.stderr || '');
      if (/Cannot find|ERR_MODULE_NOT_FOUND|createWebGL2Context/.test(txt)) {
        console.log('\nskip  meshfail     no GL context available on this machine');
      } else {
        console.log('\nFAIL  meshfail     the cart did not run');
        for (const l of txt.trim().split('\n').slice(-6)) console.log(`      ${l}`);
        failed++;
      }
    }
  }

  // Shaders must cost NOTHING when a cart does not use them. A cart that
  // never calls setShader must issue zero glUseProgram per frame; anything
  // above that means the program is being re-bound per draw or per batch.
  if (fs.existsSync(glEngine)) {
    const { execFileSync } = require('child_process');
    try {
      const out = execFileSync(process.execPath,
        [path.join(ROOT, 'tools', 'gl-call-count.mjs'), glEngine,
         path.join(ROOT, 'test', 'gl2d'), '10', '--max-useprogram', '0'],
        { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
      const m = out.match(/TOTAL\s+([\d.]+)/);
      console.log(`ok    shader-cost  0 glUseProgram/frame with no shader bound ` +
                  `(${m ? m[1] : '?'} GL calls/frame total)`);
    } catch (err) {
      const txt = (err.stdout || '') + (err.stderr || '');
      if (/Cannot find|ERR_MODULE_NOT_FOUND|createWebGL2Context/.test(txt)) {
        console.log('skip  shader-cost  no GL context available on this machine');
      } else {
        console.log('FAIL  shader-cost  the default path re-binds the GL program');
        for (const l of txt.trim().split('\n').slice(-4)) console.log(`      ${l}`);
        failed++;
      }
    }
  }

  // newShader must REFUSE broken shaders, with the driver's own message
  // reaching the cart log.
  //
  // This has to run against a REAL GL context. On a stubbed `gl` every
  // shader is refused because there is no GL at all, which would make the
  // gate green without the compiler ever being consulted -- the refusal
  // would be right for the wrong reason.
  const sfDir = path.join(ROOT, 'test', 'shaderfail');
  if (fs.existsSync(path.join(sfDir, 'main.lua')) && fs.existsSync(glEngine)) {
    const { execFileSync } = require('child_process');
    try {
      const out = execFileSync(process.execPath,
        [path.join(ROOT, 'tools', 'gl-shader-verify.mjs'), '--logs', glEngine, sfDir, '3'],
        { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
      const lines = out.split('\n');
      const refusals = lines.filter(l => /newShader/.test(l)).length;
      // A real GLSL compiler message, not our own wrapper text. Drivers do
      // not agree on the dialect, and the point of this gate is "the
      // compiler actually spoke", not "Mesa spoke":
      //   Mesa/Linux   LOG: 0:12(1): error: ...
      //   ANGLE/Apple  LOG: ERROR: 0:12: '...' : ...
      const compilerMsg = lines.some(l =>
        /^LOG: \d+:\d+\(\d+\): error/.test(l) ||      // Mesa
        /^LOG: ERROR: \d+:\d+:/.test(l));                // ANGLE / Apple GL
      const noGl = lines.some(l => /no GL context on this host/.test(l));
      if (noGl) {
        console.log('\nFAIL  shaderfail   ran without a GL context, so the refusals prove nothing');
        failed++;
      } else if (refusals < 4) {
        console.log(`\nFAIL  shaderfail   only ${refusals} refusal messages, expected >= 4`);
        for (const l of lines.slice(0, 10)) console.log(`      ${l}`);
        failed++;
      } else if (!compilerMsg) {
        console.log('\nFAIL  shaderfail   refused, but no GL info log reached the cart log');
        failed++;
      } else {
        console.log('\nok    shaderfail   4 broken shaders refused, driver messages logged');
      }
    } catch (err) {
      const txt = (err.stdout || '') + (err.stderr || '');
      if (/Cannot find|ERR_MODULE_NOT_FOUND|createWebGL2Context/.test(txt)) {
        console.log('\nskip  shaderfail   no GL context available on this machine');
      } else {
        console.log('\nFAIL  shaderfail   the cart did not run');
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

  // ── rumble: the ABI boundary the cart cannot see ──────────────────
  // Nothing about rumble reaches the framebuffer, so the smoke run above is
  // blind to it. The fake host records every call instead, which is the only
  // place the two conversions can be checked: love.pad is 1-BASED and the ABI
  // is 0-based, and LOVE durations are SECONDS while the ABI takes ms.
  const rumbleDir = path.join(ROOT, 'test', 'rumble');
  if (fs.existsSync(path.join(rumbleDir, 'main.lua'))) {
    // only pad id 0 has motors, so a per-pad query has something to get wrong
    const rr = await runCart(ENGINE, rumbleDir, 12, { rumblePads: [0] });
    const MAX = 5000;
    const want = [
      { id: 0, low: 0.5, high: 0.25, ms: 500 },   // implicit pad 1
      { id: 1, low: 1, high: 0, ms: 2000 },       // Lua pad 2 -> ABI id 1
      { id: 0, low: 0.75, high: 0.75, ms: MAX },  // no duration -> host cap
      { id: 0, stop: true },                      // zero strength is a stop
      { id: 2, stop: true },                      // stopVibration(3)
      { id: 0, stop: true },                      // no args stops pad 1
      { id: 0, low: 1, high: 0, ms: 100 },        // clamped both ways
      { id: 0, low: 0.5, high: 0.5, ms: MAX },    // duration past the cap
      { id: 3, low: 0.2, high: 0.3, ms: 250 },    // Joystick route, pad 4
      { id: 0, low: 0.6, high: 0.4, ms: 1000 },
      // case 11 asks for pad 9 and must produce NO call at all
    ];
    const problems = [];
    if (rr.trap || rr.fields.lua_ok === 0) {
      problems.push(`cart did not run: ${rr.trap || 'lua error'}`);
    }
    if (rr.rumbles.length !== want.length) {
      problems.push(`${rr.rumbles.length} host calls, expected ${want.length} ` +
        `(an out-of-range pad number must not reach the host)`);
    }
    for (let i = 0; i < Math.min(want.length, rr.rumbles.length); i++) {
      const got = rr.rumbles[i], w = want[i];
      const same = got.id === w.id && (w.stop
        ? got.stop === true
        : !got.stop && Math.abs(got.low - w.low) < 1e-5 &&
          Math.abs(got.high - w.high) < 1e-5 && got.ms === w.ms);
      if (!same) problems.push(`call ${i}: got ${JSON.stringify(got)} want ${JSON.stringify(w)}`);
    }
    const capLog = rr.logs.find(l => l.startsWith('cap1='));
    if (capLog !== 'cap1=true') problems.push(`hasVibration() = ${capLog}, expected cap1=true`);
    if (!rr.logs.includes('cap2=false')) problems.push('hasVibration(2) should be false');
    if (!rr.logs.includes('get=0.60,0.40')) problems.push('getVibration did not report the last request');
    if (!rr.logs.includes('bad=false')) problems.push('setVibration(9,...) should return false');
    if (problems.length) {
      console.log('\nFAIL  rumble');
      for (const p of problems) console.log(`      ${p}`);
      failed++;
    } else {
      console.log(`\nok    rumble       ${rr.rumbles.length} host calls, ids + ms conversion exact`);
    }

    // Control: a host that reports no rumble at all must still see the calls
    // (they are documented no-ops host-side), and hasVibration must flip. If
    // this does not move, the capability query is not wired to the host.
    const rr2 = await runCart(ENGINE, rumbleDir, 12, { rumblePads: [] });
    if (!rr2.logs.includes('cap1=false')) {
      console.log('\nFAIL  rumble control: hasVibration() stayed true with a host that has no motors');
      failed++;
    } else {
      console.log('ok    rumble-ctl   capability query tracks the host');
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
