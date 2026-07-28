#!/usr/bin/env node
/*
 * tools/gl-probe.js - what would the GLES3 path actually buy us?
 *
 * The software rasterizer's bottleneck is the sprite blitter: Cavern spends
 * 97% of its 5.5 ms frame there. This measures the SAME workload as textured
 * quads on a real GPU through the offscreen WebGL2 context romdev already
 * wires up for GL carts, so the comparison is against a path that exists
 * rather than a hypothetical one.
 *
 * It is a floor, not a promise: a real cart also pays per-draw JS/wasm call
 * overhead, which is why batched and unbatched are both measured.
 */
const W = 1280, H = 720;
const SPRITES = 2000;

// webgl-node is romdev's dependency, not ours: this probe deliberately uses
// the SAME offscreen context romdev wires up for GL carts, rather than adding
// a GPU dependency to an engine that is still pure software. Point
// WEBGL_NODE at another checkout if yours lives elsewhere.
const WEBGL_NODE = process.env.WEBGL_NODE
  || `${process.env.HOME}/code/cliemu/romdev/node_modules/webgl-node/index.mjs`;

async function main() {
  const wn = await import(WEBGL_NODE);
  const { gl } = wn.createWebGL2Context(W, H);
  console.log('renderer:', gl.getParameter(gl.RENDERER));

  const vs = `#version 300 es
in vec2 aPos; in vec2 aUV; out vec2 vUV;
void main(){ vUV=aUV; gl_Position=vec4(aPos,0.0,1.0); }`;
  const fs = `#version 300 es
precision mediump float; in vec2 vUV; uniform sampler2D uTex;
out vec4 o; void main(){ o = texture(uTex, vUV); }`;

  const mk = (t, s) => { const x = gl.createShader(t); gl.shaderSource(x, s); gl.compileShader(x);
    if (!gl.getShaderParameter(x, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(x)); return x; };
  const p = gl.createProgram();
  gl.attachShader(p, mk(gl.VERTEX_SHADER, vs));
  gl.attachShader(p, mk(gl.FRAGMENT_SHADER, fs));
  gl.linkProgram(p);
  if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p));
  gl.useProgram(p);

  // a 64x64 texture, the size the blit conformance cart uses
  const tex = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, tex);
  const px = new Uint8Array(64 * 64 * 4);
  for (let i = 0; i < 64 * 64; i++) { px[i*4]=i&255; px[i*4+1]=(i>>2)&255; px[i*4+2]=180; px[i*4+3]=255; }
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 64, 64, 0, gl.RGBA, gl.UNSIGNED_BYTE, px);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.enable(gl.BLEND);
  gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);

  // Build one big vertex buffer of SPRITES quads (128x128 dst, like the
  // 2x-scaled sprites the bench draws).
  const quads = new Float32Array(SPRITES * 6 * 4);
  let o = 0;
  for (let i = 0; i < SPRITES; i++) {
    const x = (i * 37) % (W - 128), y = (i * 53) % (H - 128);
    const x0 = (x / W) * 2 - 1, y0 = 1 - (y / H) * 2;
    const x1 = ((x + 128) / W) * 2 - 1, y1 = 1 - ((y + 128) / H) * 2;
    const v = [x0,y0,0,0, x1,y0,1,0, x1,y1,1,1, x0,y0,0,0, x1,y1,1,1, x0,y1,0,1];
    for (const n of v) quads[o++] = n;
  }
  const buf = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, buf);
  gl.bufferData(gl.ARRAY_BUFFER, quads, gl.STATIC_DRAW);
  const aPos = gl.getAttribLocation(p, 'aPos'), aUV = gl.getAttribLocation(p, 'aUV');
  gl.enableVertexAttribArray(aPos); gl.vertexAttribPointer(aPos, 2, gl.FLOAT, false, 16, 0);
  gl.enableVertexAttribArray(aUV);  gl.vertexAttribPointer(aUV,  2, gl.FLOAT, false, 16, 8);

  const time = (label, fn, reps = 20) => {
    for (let i = 0; i < 5; i++) fn();             // warm up
    const t = [];
    for (let r = 0; r < reps; r++) {
      const a = process.hrtime.bigint();
      fn();
      gl.finish();                                 // do NOT time an empty queue
      t.push(Number(process.hrtime.bigint() - a) / 1e6);
    }
    t.sort((x, y) => x - y);
    console.log(`  ${label.padEnd(34)} ${t[reps >> 1].toFixed(2).padStart(7)} ms`);
    return t[reps >> 1];
  };

  console.log(`\n${SPRITES} textured 128x128 quads at ${W}x${H}:`);
  const batched = time('batched (1 draw call)', () => {
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.drawArrays(gl.TRIANGLES, 0, SPRITES * 6);
  });
  const unbatched = time('unbatched (2000 draw calls)', () => {
    gl.clear(gl.COLOR_BUFFER_BIT);
    for (let i = 0; i < SPRITES; i++) gl.drawArrays(gl.TRIANGLES, i * 6, 6);
  });
  // The cart's framebuffer has to come back across the bus if the host
  // presents it as a 2D framebuffer, so this cost is part of the honest total.
  const back = new Uint8Array(W * H * 4);
  const read = time('readPixels 1280x720 (if needed)', () => {
    gl.readPixels(0, 0, W, H, gl.RGBA, gl.UNSIGNED_BYTE, back);
  });

  // CONTROL: a GPU result that is "too fast" is usually a GPU that drew
  // nothing. Prove pixels actually landed, and that the timing responds to
  // real work, or every number above is meaningless.
  gl.clear(gl.COLOR_BUFFER_BIT);
  gl.drawArrays(gl.TRIANGLES, 0, SPRITES * 6);
  gl.finish();
  const probe = new Uint8Array(W * H * 4);
  gl.readPixels(0, 0, W, H, gl.RGBA, gl.UNSIGNED_BYTE, probe);
  let lit = 0;
  for (let i = 0; i < W * H; i++) if (probe[i * 4 + 3] !== 0 && (probe[i*4] | probe[i*4+1] | probe[i*4+2])) lit++;
  const pct = (lit / (W * H)) * 100;
  console.log(`\ncontrol: ${lit} of ${W*H} pixels drawn (${pct.toFixed(1)}%)`);
  if (pct < 5) { console.log('  FAILED -- the GPU drew nothing; timings above are meaningless'); process.exit(1); }

  // and a heavier load must cost measurably more, or we are timing an
  // empty queue rather than the draw
  const heavy = time('control: 10x the fill (20k quads)', () => {
    gl.clear(gl.COLOR_BUFFER_BIT);
    for (let k = 0; k < 10; k++) gl.drawArrays(gl.TRIANGLES, 0, SPRITES * 6);
  });
  if (heavy < batched * 2) console.log('  WARNING -- 10x work did not cost more; timing is not measuring the draw');
  else console.log(`  ok: 10x work costs ${(heavy / batched).toFixed(1)}x the time`);

  console.log('\nsoftware rasterizer, same workload:');
  console.log('  draw/sprite(2000)                     7.45 ms');
  console.log('  draw/sprite rot(2000)                13.87 ms');
  console.log(`\nspeedup vs software sprite: batched ${(7.45/batched).toFixed(0)}x, ` +
              `unbatched ${(7.45/unbatched).toFixed(1)}x`);
  console.log(`readback alone costs ${read.toFixed(2)} ms -- a GL cart should present ` +
              `via GL, not hand back a framebuffer.`);
}
main().catch(e => { console.error(e); process.exit(1); });
