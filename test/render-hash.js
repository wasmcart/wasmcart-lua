#!/usr/bin/env node
/*
 * test/render-hash.js - hash every cart's rendered frames.
 *
 * This is the bit-exactness gate for rasterizer work. The rule the perf work
 * follows is that an optimization must not move a single pixel: "faster" is
 * only acceptable if the output is byte-identical, because carts are meant to
 * render the same everywhere and a golden hash is the only cheap way to prove
 * it.
 *
 * Usage:
 *   node test/render-hash.js            # print hashes for every cart
 *   node test/render-hash.js --save F   # write them to F
 *   node test/render-hash.js --check F  # compare against F, exit 1 on drift
 *
 * The workflow for a rasterizer change is: --save before, --check after. This
 * lives in the repo (not /tmp) precisely because it is the thing that catches
 * a blitter regression, and the last one survived an entire session by hiding
 * from a suite that never drew a sprite.
 */
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const ROOT = path.join(__dirname, '..');
// The CPU comparator: these hashes assert BIT-equality, which only the
// software rasterizer promises. build/engine.wasm is the GL build now.
const ENGINE = path.join(ROOT, 'build', 'engine-cpu.wasm');
const FRAMES = 120;

// Every cart that renders. Each is hashed over FRAMES frames so animation and
// state-dependent drawing are covered, not just the first frame.
function carts() {
  const out = [];
  for (const [group, dir] of [['examples', path.join(ROOT, 'examples')],
                              ['ports', path.join(ROOT, 'ports')],
                              ['test', path.join(ROOT, 'test')]]) {
    if (!fs.existsSync(dir)) continue;
    for (const n of fs.readdirSync(dir)) {
      for (const sub of ['', 'app']) {
        const p = path.join(dir, n, sub);
        if (fs.existsSync(path.join(p, 'main.lua'))) { out.push([n, p]); break; }
      }
    }
  }
  return out.sort((a, b) => a[0].localeCompare(b[0]));
}

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

async function hashCart(dir) {
  const assets = loadAssets(dir);
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
  // wc_get_info returns a pointer to wc_info_t; the fields are u32 in
  // declaration order, so width/height/fb_ptr are words 1/2/3.
  const info = e.wc_get_info();
  const words = () => new Uint32Array(mem.buffer, info, 4);
  e.wc_set_seed(1);
  e.wc_init();

  const h = crypto.createHash('sha256');
  for (let f = 0; f < FRAMES; f++) {
    e.wc_render();
    // Re-read every frame: memory growth detaches the old view, and a cart
    // may resize its framebuffer during wc_init.
    const [, w, ht, fb] = words();
    h.update(new Uint8Array(mem.buffer, fb, w * ht * 4));
  }
  return h.digest('hex').slice(0, 16);
}

// Minimal PNG writer, so a frame can be LOOKED AT and not just hashed. A
// golden hash of a frame nobody opened only proves the frame is stable, not
// that it is correct -- three real bugs in this engine were found by looking
// at output while the suite was fully green.
function writePNG(file, w, h, rgba) {
  const zlib = require('zlib');
  const raw = Buffer.alloc((w * 4 + 1) * h);
  for (let y = 0; y < h; y++) {
    raw[y * (w * 4 + 1)] = 0; // filter: none
    for (let x = 0; x < w; x++) {
      const s = (y * w + x) * 4, d = y * (w * 4 + 1) + 1 + x * 4;
      // framebuffer is 0xAARRGGBB little-endian: B,G,R,A in memory order
      raw[d] = rgba[s + 2]; raw[d + 1] = rgba[s + 1];
      raw[d + 2] = rgba[s]; raw[d + 3] = 255;
    }
  }
  const chunk = (type, data) => {
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type), data]);
    const crc = Buffer.alloc(4); crc.writeUInt32BE(zlib.crc32 ? zlib.crc32(td) >>> 0 : crc32(td));
    return Buffer.concat([len, td, crc]);
  };
  function crc32(buf) {
    let c = ~0;
    for (let i = 0; i < buf.length; i++) {
      c ^= buf[i];
      for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (0xEDB88320 & -(c & 1));
    }
    return ~c >>> 0;
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8; ihdr[9] = 6; // 8-bit RGBA
  fs.writeFileSync(file, Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]),
    chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw)),
    chunk('IEND', Buffer.alloc(0)),
  ]));
}

async function shot(dir, out, frames) {
  const assets = loadAssets(dir);
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
  const info = e.wc_get_info();
  e.wc_set_seed(1);
  e.wc_init();
  for (let f = 0; f < frames; f++) e.wc_render();
  const [, w, h, fb] = new Uint32Array(mem.buffer, info, 4);
  writePNG(out, w, h, new Uint8Array(mem.buffer, fb, w * h * 4));
  console.log(`wrote ${out} (${w}x${h}, after ${frames} frames)`);
}

async function main() {
  const argv0 = process.argv.slice(2);
  const si = argv0.indexOf('--shot');
  if (si >= 0) {
    // --shot <cartdir> <out.png> [frames]
    return shot(argv0[si + 1], argv0[si + 2], +(argv0[si + 3] || 3));
  }
  const list = carts();
  const res = {};
  for (const [name, dir] of list) {
    try { res[name] = await hashCart(dir); }
    catch (err) { res[name] = 'ERROR: ' + err.message; }
  }

  const argv = process.argv.slice(2);
  const save = argv.indexOf('--save');
  const check = argv.indexOf('--check');

  if (save >= 0) {
    fs.writeFileSync(argv[save + 1], JSON.stringify(res, null, 2) + '\n');
    console.log(`saved ${Object.keys(res).length} hashes to ${argv[save + 1]}`);
    return;
  }

  if (check >= 0) {
    const want = JSON.parse(fs.readFileSync(argv[check + 1], 'utf8'));
    let bad = 0;
    for (const k of Object.keys(want)) {
      const got = res[k];
      if (got === want[k]) console.log(`  IDENTICAL  ${k}`);
      else { console.log(`  CHANGED    ${k}  ${want[k]} -> ${got}`); bad++; }
    }
    for (const k of Object.keys(res)) if (!(k in want)) console.log(`  NEW        ${k}`);
    if (bad) { console.log(`\n${bad} cart(s) changed - pixels moved`); process.exit(1); }
    console.log(`\nall ${Object.keys(want).length} carts bit-identical`);
    return;
  }

  for (const k of Object.keys(res)) console.log(`  ${k.padEnd(20)} ${res[k]}`);
}

main().catch(e => { console.error(e); process.exit(1); });
