#!/usr/bin/env node
/*
 * test/net.mjs - love.net conformance against a REAL WebSocket server.
 *
 * The rest of this suite drives the engine through a hand-rolled fake host in
 * run.js, which is right for graphics: the assertions are about pixels and a
 * fake host makes them reproducible. Networking is the opposite case. Every
 * interesting failure lives in the seams - a length that becomes a strlen, a
 * peer id that becomes an index, a callback that never gets drained - and a
 * fake host is exactly the thing that cannot see them, because a fake host is
 * written by the same person with the same assumptions.
 *
 * So this file uses the REFERENCE HOST from a wasmcart checkout and the real
 * WebSocket server that ships with it (test/wsserver.mjs). Bytes cross an
 * actual socket, the manifest gate is the real gate, and the peer ids are
 * host-assigned.
 *
 * Two scenarios:
 *   ECHO   - one cart dials the echo endpoint and gets its own bytes back.
 *   RELAY  - two carts join one room and talk to each other. Neither is a
 *            server; that is the point of the peer ABI.
 *
 *   node test/net.mjs
 *
 * Needs a wasmcart checkout (WASMCART_REPO, default ~/code/cliemu/wasmcart),
 * the same dependency runtime/build.sh already has. Without one this skips
 * rather than failing: a missing checkout is a missing tool, not a red test.
 */
import { spawn, spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, writeFileSync, rmSync, copyFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createRequire } from 'node:module';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, '..');
// Resolve the spec package: env override first (a checkout, for developing
// against unreleased wasmcart), then the installed npm package.
function resolveWasmcart() {
  if (process.env.WASMCART_REPO) return process.env.WASMCART_REPO;
  try {
    return dirname(createRequire(import.meta.url).resolve('wasmcart'));
  } catch {
    return join(process.env.HOME ?? '', 'code', 'cliemu', 'wasmcart');
  }
}
const WASMCART = resolveWasmcart();
const ENGINE = join(ROOT, 'build', 'engine-cpu.wasm');
const WORK = join(HERE, '.net-work');
const PORT = Number(process.env.WC_NET_TEST_PORT || 8791);
const HOSTNAME = '127.0.0.1';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

let failed = 0;

/* A Lua error inside a callback still lets the frames run, so nothing else
 * here would notice one. Every scenario checks its carts for this. */
function luaProblems(label, logs) {
  return logs.filter((l) => l.startsWith('lua error'))
    .map((l) => `${label}: ${l}`);
}
function ok(name, msg) { console.log(`ok    ${name.padEnd(12)} ${msg}`); }
function fail(name, problems) {
  console.log(`\nFAIL  ${name}`);
  for (const p of problems) console.log(`      ${p}`);
  failed++;
}

/** Pack the test cart with a role, an address, and a manifest net grant. */
function packCart(role, addr, { grantDomain = HOSTNAME } = {}) {
  const appDir = join(WORK, role, 'app');
  mkdirSync(appDir, { recursive: true });
  copyFileSync(join(HERE, 'net', 'main.lua'), join(appDir, 'main.lua'));
  writeFileSync(join(appDir, 'role.txt'), role);
  writeFileSync(join(appDir, 'addr.txt'), addr);
  const out = join(WORK, `${role}.wasc`);
  const args = ['--wasm', ENGINE, '--assets', appDir, '--name', `net-${role}`, '-o', out];
  if (grantDomain) args.push('--ws', grantDomain);
  const r = spawnSync(process.execPath,
    [join(WASMCART, 'bin', 'wasmcart-pack.js'), ...args], { encoding: 'utf8' });
  if (r.status !== 0) throw new Error(`pack failed: ${r.stderr}`);
  return out;
}

/* The host buffers wc_log lines rather than pushing them, so a run drains
 * them at the end. Every assertion below is a line the cart emitted. */
function drainLogs(cart, into) {
  for (const e of cart.drainDebugEvents().log) into.push(e.text);
}

/**
 * Run a cart for `frames` frames through the reference host, letting the
 * event loop breathe between frames. Networking is asynchronous by nature:
 * a synchronous frame loop would finish before the socket ever connected and
 * the whole test would assert on an empty log.
 */
async function runCart(CartHost, wascPath, frames, logs, opts = {}) {
  const cart = new CartHost();
  await cart.load(wascPath);
  if (opts.prePeers) opts.prePeers(cart);
  for (let i = 0; i < frames; i++) {
    cart.runFrame([]);
    drainLogs(cart, logs);
    await sleep(4);
  }
  drainLogs(cart, logs);
  cart.destroy();
  return cart;
}

async function main() {
  if (!existsSync(ENGINE)) {
    console.error('engine not built: run runtime/build.sh first');
    process.exit(1);
  }
  const hostEntry = join(WASMCART, 'index.js');
  // The WS test server is VENDORED (test/wsserver.mjs): the npm package does
  // not ship its test/ directory, and the server is dependency-free.
  const wsserver = join(HERE, 'wsserver.mjs');
  if (!existsSync(hostEntry)) {
    console.log(`skip  net          no wasmcart package or checkout at ${WASMCART} ` +
      `(npm install, or set WASMCART_REPO)`);
    return;
  }
  const { CartHost } = await import(pathToFileURL(hostEntry).href);

  rmSync(WORK, { recursive: true, force: true });
  mkdirSync(WORK, { recursive: true });

  const server = spawn(process.execPath, [wsserver, '--port', String(PORT)],
    { stdio: ['ignore', 'ignore', 'inherit'] });
  await sleep(400);

  try {
    // ── echo: bytes make a full round trip ────────────────────────
    {
      const wasc = packCart('echo', `ws://${HOSTNAME}:${PORT}/echo`);
      const logs = [];
      // Register a host-managed peer at a high id BEFORE the cart dials, so
      // the dialed peer's id and its position in the enumeration are
      // different numbers. Without this the reference host hands the first
      // dialed peer id 0, index and id coincide, and an engine that confused
      // the two would sail straight through every assertion below.
      const sink = { send() {}, close() {}, onmessage: null, onclose: null };
      await runCart(CartHost, wasc, 60, logs, {
        prePeers: (cart) => cart.addPeer(77, 'host-peer', sink),
      });
      const problems = [];
      const has = (s) => logs.some((l) => l.startsWith(s));
      const find = (s) => logs.find((l) => l.startsWith(s));

      if (!logs.includes('open=true')) problems.push('love.net.open did not return a peer id');
      if (!logs.includes('badopen=nil')) problems.push('an unroutable address must return nil');
      if (!logs.includes('badtype=nil')) problems.push('open(non-string) must return nil');
      if (!logs.includes('ghost=closed')) problems.push('state() of an unknown peer must read closed');
      if (!has('connected=')) problems.push('love.net.connected never fired');
      if (!logs.some((l) => l.startsWith('connected=77 kind=host name=host-peer'))) {
        problems.push('the host-registered peer never reached love.net.connected');
      }
      if (!logs.includes('count=2')) problems.push(
        `count() wrong: ${find('count=')} (one host-registered peer plus the dialed one)`);
      // The host names a dialed peer after its hostname. Display only, but it
      // must survive the C name buffer without a stray terminator.
      const conn = logs.find((l) => l.startsWith('connected=') && l.includes('kind=dialed'));
      if (!conn) problems.push('the dialed peer never reached love.net.connected');
      else if (!conn.endsWith(`name=${HOSTNAME}`)) {
        problems.push(`peer name mangled: ${conn}`);
      }
      // peers() must list the SAME id the callback carried. If the C layer
      // ever turned a peer id into a 0-based index these disagree.
      const id = conn && conn.match(/^connected=(\d+)/)?.[1];
      if (!id) problems.push('could not read the peer id from the connect callback');
      else if (!logs.includes(`state=${id}=open`)) problems.push(
        `state(${id}) did not report open inside the connect callback`);
      else {
        const listed = (find('peers=') ?? '').slice('peers='.length).split(',');
        // Membership, not order: the host enumerates in its own order and
        // that is not the engine's business. What IS the engine's business
        // is that these are host-assigned IDS. An engine returning
        // enumeration indices would say 0,1 and miss 77 entirely.
        if (!listed.includes('77') || !listed.includes(id)) problems.push(
          `peers() = ${find('peers=')}, expected the host peer 77 and the dialed id ${id} ` +
          `(an engine returning enumeration INDICES would say 0,1 here)`);
      }

      if (!logs.includes('sent=10')) problems.push(
        `send() returned ${find('sent=')}, expected 10 bytes ` +
        `(a strlen on this path stops at the embedded NUL and reports 2)`);
      // Both peers are open, so a broadcast reaches two of them: the dialed
      // socket and the host-registered one.
      if (!logs.includes('bcast=2')) problems.push(`broadcast() returned ${find('bcast=')}, expected 2`);
      if (!logs.includes('badsend=nil')) problems.push('send() to an unknown peer must return nil');
      if (!has('msg=')) problems.push('love.net.message never fired');
      // The exact bytes matter: NUL in the middle, 0xff, trailing NULs.
      const want = '4142004344ff00004546';
      const msg = logs.find((l) => l.startsWith('msg=') && l.includes(want));
      if (!msg) problems.push(
        `echoed payload wrong: ${find('msg=')} expected hex ${want}`);
      if (!logs.includes('roundtrip=exact')) problems.push(
        'the echoed string did not compare equal to what was sent');
      problems.push(...luaProblems('echo cart', logs));

      if (problems.length) fail('net-echo', problems);
      else ok('net-echo', `${logs.length} facts, 10-byte binary payload round-tripped exactly`);
    }

    // ── relay: two carts, no server ───────────────────────────────
    // The relay endpoint forwards whatever one client sends to the others in
    // the room. That is the shape a data channel presents to a cart, so it
    // exercises the ABI honestly: neither cart can tell it is not talking to
    // the other one directly.
    {
      const room = `ws://${HOSTNAME}:${PORT}/relay/lua`;
      const wascB = packCart('relay-b', room);
      const wascA = packCart('relay-a', room);
      const logsA = [], logsB = [];

      const cartB = new CartHost();
      await cartB.load(wascB);
      // B joins first and stays in the room; A dials in afterwards.
      for (let i = 0; i < 12; i++) { cartB.runFrame([]); drainLogs(cartB, logsB); await sleep(4); }

      const cartA = new CartHost();
      await cartA.load(wascA);
      for (let i = 0; i < 60; i++) {
        cartA.runFrame([]);
        cartB.runFrame([]);
        drainLogs(cartA, logsA);
        drainLogs(cartB, logsB);
        await sleep(4);
      }
      drainLogs(cartA, logsA);
      drainLogs(cartB, logsB);
      cartA.destroy();
      cartB.destroy();

      const problems = [];
      if (!logsA.includes('open=true')) problems.push('cart A never opened a connection');
      if (!logsB.includes('ready=b')) problems.push('cart B never reached the open state');
      if (!logsA.includes('sent=10')) problems.push('cart A did not send its probe');
      // B receives A's bytes. Same payload, but this time it crossed between
      // two independent cart instances rather than bouncing off an echo.
      const got = logsB.find((l) => l.startsWith('msg=') && l.includes('4142004344ff00004546'));
      if (!got) problems.push(
        `cart B did not receive A's payload intact: ${logsB.find((l) => l.startsWith('msg=')) ?? '(no message at all)'}`);
      if (!logsB.some((l) => l.startsWith('msg=') && l.includes('len=1'))) {
        problems.push("cart B did not receive A's broadcast");
      }
      // close() must move the state, which is also the only assertion here
      // that a cart-initiated close reaches the host at all.
      if (!logsB.includes('bclose=closing') && !logsB.includes('bclose=closed')) {
        problems.push(`cart B's close() did not move the state: ${logsB.find((l) => l.startsWith('bclose='))}`);
      }
      problems.push(...luaProblems('cart A', logsA), ...luaProblems('cart B', logsB));

      if (problems.length) fail('net-relay', problems);
      else ok('net-relay', 'two carts exchanged binary payloads through a relay');
    }

    // ── the manifest gate ─────────────────────────────────────────
    // The engine sets WC_FLAG_NET_PEER for every cart, so the manifest is the
    // ONLY thing standing between a Lua game and the network. A cart packed
    // without a domain grant must be refused by the host even though its code
    // is byte-identical to the one that just worked.
    {
      const wasc = packCart('nogrant', `ws://${HOSTNAME}:${PORT}/echo`, { grantDomain: null });
      const logs = [];
      await runCart(CartHost, wasc, 20, logs);
      if (!logs.includes('open=false')) {
        fail('net-gate', [
          `an ungranted cart opened a connection anyway: ${logs.find((l) => l.startsWith('open=')) }`,
          'the manifest half of the dual gate is not being enforced',
        ]);
      } else if (logs.some((l) => l.startsWith('connected='))) {
        fail('net-gate', ['an ungranted cart still received a connect callback']);
      } else {
        ok('net-gate', 'no net.domains grant means no connection, same cart code');
      }
    }

    // ── a domain grant for somewhere else ─────────────────────────
    // Proves the allowlist is compared, not merely present.
    {
      const wasc = packCart('wrongdomain', `ws://${HOSTNAME}:${PORT}/echo`,
        { grantDomain: 'example.com' });
      const logs = [];
      await runCart(CartHost, wasc, 20, logs);
      if (!logs.includes('open=false')) {
        fail('net-domain', ['a cart granted example.com reached 127.0.0.1']);
      } else {
        ok('net-domain', 'the allowlist is matched against the address, not just checked for existence');
      }
    }
  } finally {
    server.kill();
    rmSync(WORK, { recursive: true, force: true });
  }

  console.log(failed ? `\n${failed} FAILED` : '\nnet: all green');
  process.exit(failed ? 1 : 0);
}

main().catch((e) => { console.error(e); process.exit(1); });
