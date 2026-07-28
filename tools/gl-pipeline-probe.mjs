#!/usr/bin/env node
/*
 * tools/gl-pipeline-probe.mjs - what would a FULL 2D GPU pipeline be worth?
 *
 * Models a Cavern-shaped frame (34 textured sprite quads + 36 vector draws,
 * each with its own uniforms) entirely on the GPU, and a 2000+2000 stress
 * frame, against the measured software costs.
 *
 * Result on an AMD 890M: 0.11 ms for the Cavern-shaped frame against 5.50 ms
 * software, ~49x. Per-draw overhead is NOT the blocker -- gl calls from wasm
 * cost about 0.002 ms each and account for 12% of a GL frame, the rest being
 * the software rasterizer that would go away.
 *
 * The blocker is exactness of BLENDED primitives; see gl-blend-exactness.mjs.
 */
// entirely on GPU -- 34 textured sprite quads + 36 vector draws, each with
// its own uniforms -- and compare against measured software cost.
const WEBGL_NODE=`${process.env.HOME}/code/cliemu/romdev/node_modules/webgl-node/index.mjs`;
const wn=await import(WEBGL_NODE);
const W=1280,H=720;
const {gl}=wn.createWebGL2Context(W,H);
const mk=(t,s)=>{const x=gl.createShader(t);gl.shaderSource(x,s);gl.compileShader(x);
  if(!gl.getShaderParameter(x,gl.COMPILE_STATUS))throw new Error(gl.getShaderInfoLog(x));return x;};
const prog=(v,f)=>{const p=gl.createProgram();gl.attachShader(p,mk(gl.VERTEX_SHADER,v));
  gl.attachShader(p,mk(gl.FRAGMENT_SHADER,f));gl.linkProgram(p);return p;};
const VS=`#version 300 es
in vec2 aPos; uniform vec4 uR; out vec2 vUV;
void main(){vUV=aPos;gl_Position=vec4(uR.xy+aPos*uR.zw,0.,1.);}`;
const FS_SPR=`#version 300 es
precision highp float; in vec2 vUV; uniform sampler2D uT; uniform vec4 uC; out vec4 o;
void main(){vec4 c=texture(uT,vUV); if(c.a==0.0) discard; o=c*uC;}`;
const FS_SOL=`#version 300 es
precision mediump float; in vec2 vUV; uniform vec4 uC; out vec4 o;
void main(){o=uC;}`;
const pS=prog(VS,FS_SPR), pV=prog(VS,FS_SOL);
const quad=new Float32Array([0,0,1,0,0,1,1,1]);
const b=gl.createBuffer();gl.bindBuffer(gl.ARRAY_BUFFER,b);
gl.bufferData(gl.ARRAY_BUFFER,quad,gl.STATIC_DRAW);
for(const p of [pS,pV]){gl.useProgram(p);const a=gl.getAttribLocation(p,'aPos');
  gl.enableVertexAttribArray(a);gl.vertexAttribPointer(a,2,gl.FLOAT,false,0,0);}
const tex=gl.createTexture();gl.bindTexture(gl.TEXTURE_2D,tex);
const sp=new Uint8Array(128*128*4);for(let i=0;i<128*128;i++){sp[i*4]=i&255;sp[i*4+3]=255;}
gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA8,128,128,0,gl.RGBA,gl.UNSIGNED_BYTE,sp);
gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.NEAREST);
gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.NEAREST);
gl.enable(gl.BLEND);gl.blendFunc(gl.SRC_ALPHA,gl.ONE_MINUS_SRC_ALPHA);
const uRS=gl.getUniformLocation(pS,'uR'),uCS=gl.getUniformLocation(pS,'uC');
const uRV=gl.getUniformLocation(pV,'uR'),uCV=gl.getUniformLocation(pV,'uC');
const frame=(nSpr,nVec)=>{
  gl.clearColor(0,0,0,1);gl.clear(gl.COLOR_BUFFER_BIT);
  gl.useProgram(pS);
  for(let i=0;i<nSpr;i++){gl.uniform4f(uRS,(i%20)/10-1,(i%15)/8-1,0.2,0.35);
    gl.uniform4f(uCS,1,1,1,1);gl.drawArrays(gl.TRIANGLE_STRIP,0,4);}
  gl.useProgram(pV);
  for(let i=0;i<nVec;i++){gl.uniform4f(uRV,(i%25)/12-1,(i%18)/9-1,0.1,0.08);
    gl.uniform4f(uCV,0.4,0.7,0.9,0.6);gl.drawArrays(gl.TRIANGLE_STRIP,0,4);}
};
const t=(label,nS,nV,n=40)=>{for(let i=0;i<5;i++)frame(nS,nV);const a=[];
  for(let i=0;i<n;i++){const s=process.hrtime.bigint();frame(nS,nV);gl.finish();
    a.push(Number(process.hrtime.bigint()-s)/1e6);}a.sort((x,y)=>x-y);
  console.log(`  ${label.padEnd(40)} ${a[n>>1].toFixed(3).padStart(7)} ms`);return a[n>>1];};
console.log('a Cavern-shaped frame, fully on GPU:');
const cav=t('34 sprites + 36 vector draws',34,36);
const heavy=t('2000 sprites + 2000 vector draws',2000,2000);
console.log(`\nsoftware measured, same shapes:`);
console.log(`  Cavern frame (real)                       5.50 ms   (97% blitter)`);
console.log(`  draw/sprite(2000)                         7.62 ms`);
console.log(`  draw/rect alpha(2000)                     1.28 ms`);
console.log(`\nGPU Cavern-shaped frame: ${cav.toFixed(2)} ms vs 5.50 ms software -> ${(5.5/cav).toFixed(0)}x`);
console.log(`GPU 2000+2000:           ${heavy.toFixed(2)} ms vs ~8.9 ms software -> ${(8.9/heavy).toFixed(0)}x`);
