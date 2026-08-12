#!/usr/bin/env node
// gen-status.mjs - regenerate API_STATUS.md from a live engine.
//
// WHY THIS IS GENERATED, NOT HAND-WRITTEN: a status table that someone
// updates by hand drifts the moment it is inconvenient, and then it is
// worse than nothing because people trust it. This one is produced by
// ENUMERATING THE RUNNING ENGINE -- tools/apistatus/enumerate.lua walks the
// love table by reflection and prints what is really there -- so the table
// cannot claim a function the engine does not export.
//
// It still cannot tell you a function WORKS. That is what
// test/apiconform/ is for: it asserts on values, not presence. The two
// together are the honest claim. Presence alone is how you end up with
// Lutro's `lutro.shaders` line meaning something quite different from
// love.graphics.newShader.
//
// Usage:
//   node tools/apistatus/gen-status.mjs <api-dump.txt> [-o API_STATUS.md]
//
// where api-dump.txt is the WCLUA_API output of enumerate.lua run under
// the engine. See the header of API_STATUS.md for the full loop.

import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const dumpPath = args.find((a) => !a.startsWith("-"));
const outIdx = args.indexOf("-o");
const outPath = outIdx >= 0 ? args[outIdx + 1] : join(HERE, "..", "..", "API_STATUS.md");

if (!dumpPath) {
  console.error("usage: gen-status.mjs <api-dump.txt> [-o API_STATUS.md]");
  process.exit(1);
}

const ours = new Set(
  readFileSync(dumpPath, "utf8")
    .split("\n")
    .map((l) => l.trim())
    .filter((l) => l.startsWith("love.")),
);

// Functions that EXIST only to refuse.
//
// The enumerator walks the love table and cannot tell a working function
// from one whose whole body is error("not available"). Counting those as
// implemented is precisely the lie this generated table exists to prevent:
// it would have taken love.physics to a triumphant 22/22 while two of the
// entries raise the moment a cart calls them.
//
// They are still WORTH HAVING as stubs -- a named error that says why and
// what to do instead beats a nil-index crash inside somebody's library --
// but they are listed separately and excluded from the count.
const REFUSED = {
  "love.physics.newGearJoint":
    "Box2D 3.x removed the gear joint; no primitive to build one on",
  "love.physics.newPulleyJoint":
    "Box2D 3.x removed the pulley joint; no primitive to build one on",
};
for (const name of Object.keys(REFUSED)) ours.delete(name);

// The LOVE surface we measure against. Taken from libretro's lutro-status
// list so our percentage and theirs mean the same thing -- comparing to a
// denominator we picked ourselves would be marking our own homework.
const love = readFileSync(join(HERE, "love-api.txt"), "utf8")
  .split("\n")
  .map((l) => l.trim())
  .filter(Boolean);

// Functions this engine has that LOVE does not. Not padding for the score:
// they are listed separately and excluded from the percentage, because a
// cart author needs to know which calls will NOT port back to desktop LOVE.
const extras = [...ours].filter((f) => !love.includes(f)).sort();

const moduleOf = (f) => {
  const p = f.split(".");
  return p.length > 2 ? `love.${p[1]}` : "love";
};

const byModule = new Map();
for (const fn of love) {
  const m = moduleOf(fn);
  if (!byModule.has(m)) byModule.set(m, []);
  byModule.get(m).push(fn);
}

let have = 0;
for (const fn of love) if (ours.has(fn)) have++;
const pct = ((100 * have) / love.length).toFixed(0);

const lines = [];
lines.push("# wasmcart-lua: LOVE API status");
lines.push("");
lines.push("**Generated, not hand-maintained.** Every row comes from walking the");
lines.push("`love` table of a running engine, so this file cannot claim a function");
lines.push("that is not actually exported.");
lines.push("");
lines.push("To regenerate:");
lines.push("");
lines.push("```sh");
lines.push("# 1. run the enumerator as a cart and capture its log");
lines.push("#    (tools/apistatus/enumerate.lua prints WCLUA_API_BEGIN ... END)");
lines.push("# 2. feed the dump in:");
lines.push("node tools/apistatus/gen-status.mjs api-dump.txt");
lines.push("```");
lines.push("");
lines.push(`**${have} of ${love.length} LOVE functions implemented (${pct}%).**`);
lines.push("");
lines.push("A caveat worth stating plainly: this table measures PRESENCE. A");
lines.push("function can be exported and still be wrong. `test/apiconform/`");
lines.push("exists for that half -- it asserts on values (round-trips, known-answer");
lines.push("maths, conserved areas) rather than on whether a name resolves.");
lines.push("");
lines.push("The denominator is libretro's [lutro-status](https://github.com/libretro/lutro-status)");
lines.push("list, so this percentage and Lutro's are directly comparable.");
lines.push("");
lines.push("## Coverage by module");
lines.push("");
lines.push("| module | implemented | total | coverage |");
lines.push("|---|---:|---:|---:|");
for (const m of [...byModule.keys()].sort()) {
  const fns = byModule.get(m);
  const n = fns.filter((f) => ours.has(f)).length;
  lines.push(`| \`${m}\` | ${n} | ${fns.length} | ${((100 * n) / fns.length).toFixed(0)}% |`);
}
lines.push("");
lines.push("## Function by function");
lines.push("");
for (const m of [...byModule.keys()].sort()) {
  const fns = byModule.get(m).slice().sort();
  const n = fns.filter((f) => ours.has(f)).length;
  lines.push(`### \`${m}\` — ${n}/${fns.length}`);
  lines.push("");
  lines.push("| | function |");
  lines.push("|---|---|");
  for (const fn of fns) {
    lines.push(`| ${ours.has(fn) ? ":white_check_mark:" : ":white_medium_square:"} | \`${fn}\` |`);
  }
  lines.push("");
}

const refusedList = Object.entries(REFUSED);
if (refusedList.length) {
  lines.push("## Present but refusing");
  lines.push("");
  lines.push("These exist so that calling them raises a clear, named error");
  lines.push("instead of crashing somewhere unhelpful. They are **not**");
  lines.push("implemented and are excluded from the count above.");
  lines.push("");
  lines.push("| function | why |");
  lines.push("|---|---|");
  for (const [fn, why] of refusedList) lines.push(`| \`${fn}\` | ${why} |`);
  lines.push("");
}

if (extras.length) {
  lines.push("## Beyond LOVE");
  lines.push("");
  lines.push("Functions this engine adds. **These do not exist in desktop LOVE**, so a");
  lines.push("cart using them is not portable back to it. Excluded from the");
  lines.push("percentage above.");
  lines.push("");
  for (const fn of extras) lines.push(`- \`${fn}\``);
  lines.push("");
}

writeFileSync(outPath, lines.join("\n"));
console.log(`${outPath}: ${have}/${love.length} (${pct}%), ${extras.length} beyond LOVE`);
