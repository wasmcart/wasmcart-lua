/*
 * test/jit-host.js - wire wasmcart's runtime-JIT import (wc_jit_link) into a
 * test harness that builds its own imports. The engine imports it; a host
 * that lacks it auto-stubs it and the engine simply never compiles, but these
 * harnesses list their imports explicitly, so they must provide it.
 *
 *   const jit = await require('./jit-host.js')();
 *   imports.env = { ..., ...jit.imports };
 *   const { instance } = await WebAssembly.instantiate(bytes, imports);
 *   jit.bind(instance);
 *
 * WASMCART_JIT=0 runs the interpreter only; WASMCART_JIT_THRESHOLD=n forces
 * the hotness threshold (0 compiles every function on first use).
 */
module.exports = async function makeJitHost() {
  const { createJitImports, jitOptionsFromEnv } = await import('wasmcart');
  let inst = null;
  const j = createJitImports({ getInstance: () => inst, mode: 'sync', ...jitOptionsFromEnv() });
  return { imports: j.imports, stats: j.stats, bind(i) { inst = i; } };
};
