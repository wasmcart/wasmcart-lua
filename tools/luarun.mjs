#!/usr/bin/env node
/*
 * tools/luarun.mjs - run a plain Lua script inside the engine, headless.
 *
 * For VM-level tests (the official Lua test suite, three.lua's unit tests,
 * the JIT differential runs): the script runs as the cart's main chunk under
 * a wrapper that provides dofile() from the cart's files, reports the
 * outcome on one line ("LUARUN OK" / "LUARUN FAIL: ..."), and exits.
 *
 *   node tools/luarun.mjs [options] script.lua [more files or dirs...]
 *     --engine PATH      engine wasm (default build/engine-cpu.wasm)
 *     --dir DIR          serve every file under DIR as a cart asset
 *     --jit N            force the JIT hotness threshold to N (0 = everything)
 *     --nojit            the host refuses to link (pure interpreter)
 *     --async            compile asynchronously (the browser path)
 *     --frames N         also run N frames after the main chunk (default 0)
 *     --pre CODE         Lua run before the script (e.g. "_soft=true")
 *     --quiet            print only the result line and JIT stats
 *
 * Exit status: 0 when the script ran to completion, 1 otherwise.
 */
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createJitImports } from 'wasmcart';

const ROOT = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');

function parseArgs(argv) {
  const o = { engine: path.join(ROOT, 'build', 'engine-cpu.wasm'), dirs: [], files: [],
    jit: undefined, nojit: false, async: false, frames: 0, pre: '', quiet: false };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--engine') o.engine = argv[++i];
    else if (a === '--dir') o.dirs.push(argv[++i]);
    else if (a === '--jit') o.jit = Number(argv[++i]);
    else if (a === '--nojit') o.nojit = true;
    else if (a === '--async') o.async = true;
    else if (a === '--frames') o.frames = Number(argv[++i]);
    else if (a === '--pre') o.pre += argv[++i] + '\n';
    else if (a === '--quiet') o.quiet = true;
    else o.files.push(a);
  }
  return o;
}

function addDir(assets, dir, prefix = '') {
  for (const name of fs.readdirSync(dir)) {
    const p = path.join(dir, name);
    const rel = prefix ? `${prefix}/${name}` : name;
    if (fs.statSync(p).isDirectory()) addDir(assets, p, rel);
    else assets[rel] = fs.readFileSync(p);
  }
}

const WRAPPER = (script, pre) => `
local wc, string, xpcall, debug, tostring = wc, string, xpcall, debug, tostring
function dofile(name)
  local src = wc.asset_read(name)
  if not src then error("cannot open " .. tostring(name), 2) end
  local f = assert(load(src, "@" .. name))
  return f()
end
loadfile = function(name, mode, env)
  local src = wc.asset_read(name)
  if not src then return nil, "cannot open " .. tostring(name) end
  return load(src, "@" .. name, mode, env)
end
${pre}
local ok, err = xpcall(dofile, debug.traceback, ${JSON.stringify(script)})
if ok then wc.log("LUARUN OK") else wc.log("LUARUN FAIL: " .. tostring(err)) end
local s = wc.jit.stats()
wc.log(string.format("LUARUN JIT compiled=%d failed=%d rejected=%d modules=%d bytes=%d bails=%d pending=%d threshold=%d lasterr=%s",
  s.compiled, s.failed, s.rejected, s.modules, s.bytes, s.bails, s.pending, s.threshold, s.lasterr))
`;

export async function runLua(opts) {
  opts = { engine: path.join(ROOT, 'build', 'engine-cpu.wasm'), dirs: [], pre: '', frames: 0, ...opts };
  const assets = {};
  for (const d of opts.dirs) addDir(assets, d);
  const script = opts.files[0];
  for (const f of opts.files) {
    if (fs.statSync(f).isDirectory()) addDir(assets, f);
    else assets[path.basename(f)] = fs.readFileSync(f);
  }
  assets['main.lua'] = Buffer.from(WRAPPER(path.basename(script), opts.pre));

  const dec = new TextDecoder();
  const lines = [];
  let instance = null;
  const jit = createJitImports({
    getInstance: () => instance,
    mode: opts.async ? 'async' : 'sync',
    disabled: opts.nojit,
    threshold: Number.isInteger(opts.jit) ? opts.jit : undefined,
  });
  const mem = () => instance.exports.memory;
  const env = {
    wc_log: (p, l) => {
      const s = dec.decode(new Uint8Array(mem().buffer, p, l));
      lines.push(s);
      if (!opts.quiet || s.startsWith('LUARUN')) console.log(s);
    },
    wc_asset_size: (p, l) => {
      const n = dec.decode(new Uint8Array(mem().buffer, p, l));
      return assets[n] ? assets[n].length : -1;
    },
    wc_load_asset: (p, l, d, max) => {
      const n = dec.decode(new Uint8Array(mem().buffer, p, l));
      if (!assets[n]) return -1;
      const b = assets[n];
      const len = Math.min(b.length, max);
      new Uint8Array(mem().buffer, d, len).set(b.subarray(0, len));
      return len;
    },
    ...jit.imports,
  };
  const mod = new WebAssembly.Module(fs.readFileSync(opts.engine));
  const imports = { env: {}, wasi_snapshot_preview1: {}, gl: {} };
  for (const imp of WebAssembly.Module.imports(mod)) {
    if (imp.kind !== 'function') continue;
    const ns = imports[imp.module] || (imports[imp.module] = {});
    if (imp.module === 'env' && env[imp.name]) ns[imp.name] = env[imp.name];
    else if (!ns[imp.name]) ns[imp.name] = () => (imp.module === 'env' ? -1 : 0);
  }
  instance = new WebAssembly.Instance(mod, imports);
  const ex = instance.exports;
  ex.wc_get_info();
  if (ex.wc_set_seed) ex.wc_set_seed(0x1234abcd);
  let trap = null;
  const t0 = performance.now();
  try {
    ex.wc_init();
    for (let i = 0; i < opts.frames; i++) {
      ex.wc_render();
      if (opts.async) await new Promise((r) => setImmediate(r));
    }
  } catch (e) {
    trap = e;
  }
  const ms = performance.now() - t0;
  const ok = !trap && lines.includes('LUARUN OK');
  return { ok, trap, lines, ms, jit: jit.stats };
}

if (process.argv[1] && fileURLToPath(import.meta.url) === path.resolve(process.argv[1])) {
  const opts = parseArgs(process.argv.slice(2));
  if (!opts.files.length) {
    console.error('usage: node tools/luarun.mjs [options] script.lua [files...]');
    process.exit(2);
  }
  const r = await runLua(opts);
  if (r.trap) console.log('LUARUN TRAP:', r.trap && (r.trap.stack || r.trap.message || r.trap));
  if (!opts.quiet) {
    console.log(`host: ${r.ms.toFixed(1)} ms, modules=${r.jit.modules} functions=${r.jit.functions} ` +
      `bytes=${r.jit.bytes} compileMs=${r.jit.compileMs.toFixed(1)} failures=${r.jit.failures}` +
      (r.jit.lastError ? ` lastError=${r.jit.lastError}` : ''));
  }
  process.exit(r.ok ? 0 : 1);
}
