#!/usr/bin/env node
/*
 * abi-drift.mjs -- the vendored wasmcart.h must match the spec's.
 *
 * runtime/wasmcart.h is a COPY of include/wasmcart.h from the wasmcart repo.
 * A copy drifts, and this one drifts silently: runtime.c and render2d_gl.c say
 *
 *     #include "wasmcart.h"
 *
 * and a QUOTED include searches the including file's own directory first. So
 * runtime/wasmcart.h wins over the -I "$WASMCART_REPO/include" that build.sh
 * also passes, no matter which order the -I flags appear in. Verified, not
 * assumed. If the copy goes stale the engine is built against the old ABI and
 * nothing says so: no warning, no error, just a cart whose idea of the struct
 * layout quietly disagrees with the host's.
 *
 *   node tools/abi-drift.mjs [--wasmcart <path-to-wasmcart-checkout>]
 *
 * Resolution order: --wasmcart, $WASMCART_REPO, $WASMCART_DIR, ../wasmcart.
 * Skips with a clear message if none resolve, so this never fails a machine
 * that simply lacks a checkout.
 */
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.join(HERE, '..');
const VENDORED = path.join(ROOT, 'runtime', 'wasmcart.h');

function specDir() {
  const i = process.argv.indexOf('--wasmcart');
  if (i >= 0 && process.argv[i + 1]) return process.argv[i + 1];
  return process.env.WASMCART_REPO || process.env.WASMCART_DIR
    || path.join(ROOT, '..', 'wasmcart');
}

const dir = specDir();
const specHeader = path.join(dir, 'include', 'wasmcart.h');

let spec;
try {
  spec = readFileSync(specHeader, 'utf8');
} catch {
  console.log(`skip  abi-drift   no wasmcart checkout at ${dir}`);
  console.log('      pass --wasmcart <path> or set WASMCART_REPO');
  process.exit(0);
}

let mine;
try {
  mine = readFileSync(VENDORED, 'utf8');
} catch {
  console.error(`FAIL  abi-drift   runtime/wasmcart.h is missing`);
  process.exit(1);
}

if (mine === spec) {
  console.log(`ok    abi-drift    runtime/wasmcart.h matches ${specHeader}`);
  process.exit(0);
}

/* Not identical. Say WHAT differs rather than just that something does, so the
 * reader can tell a real ABI change from a reflowed comment. */
const grab = (src, re) => {
  const out = new Map();
  for (const m of src.matchAll(re)) out.set(m[1], m[2].trim());
  return out;
};
const DEFINES = /^#define\s+(WC_[A-Z0-9_]+)\s+(.+?)\s*(?:\/\*|\/\/|$)/gm;
const specDefs = grab(spec, DEFINES);
const myDefs = grab(mine, DEFINES);

const problems = [];
for (const [name, val] of specDefs) {
  if (!myDefs.has(name)) problems.push(`missing ${name} (spec: ${val})`);
  else if (myDefs.get(name) !== val) {
    problems.push(`${name} = ${myDefs.get(name)}, spec says ${val}`);
  }
}
for (const name of myDefs.keys()) {
  if (!specDefs.has(name)) problems.push(`${name} is not in the spec any more`);
}

/* Struct field order, which a reflowed comment cannot change but a real ABI
 * edit does. */
const structOf = (src, name) => {
  const m = src.match(new RegExp(`typedef struct \\{([^}]*)\\} ${name};`, 's'));
  return m ? [...m[1].matchAll(/\b([a-z_][a-z_0-9]*)\s*(?:\[\d+\])?\s*;/g)].map((x) => x[1]) : null;
};
for (const s of ['wc_info_t', 'wc_pad_t', 'wc_time_t', 'wc_host_info_t', 'wc_pointer_t']) {
  const a = structOf(spec, s);
  const b = structOf(mine, s);
  if (!a || !b) { problems.push(`could not read struct ${s} from both headers`); continue; }
  if (a.join(',') !== b.join(',')) {
    problems.push(`${s} fields differ:\n      got  ${b.join(' ')}\n      want ${a.join(' ')}`);
  }
}

if (problems.length === 0) {
  console.log(`ok    abi-drift    runtime/wasmcart.h differs from the spec only in comments/whitespace`);
  console.log(`      refresh it anyway: cp ${specHeader} runtime/wasmcart.h`);
  process.exit(0);
}

console.error(`FAIL  abi-drift   runtime/wasmcart.h has drifted from ${specHeader}`);
for (const p of problems) console.error(`      ${p}`);
console.error(`      fix: cp ${specHeader} runtime/wasmcart.h`);
process.exit(1);
