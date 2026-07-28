#!/usr/bin/env node
/*
 * tools/gl-blend-exactness.mjs - can GPU PRIMITIVES match the software ones?
 *
 * Sprites can (see gl-exactness.mjs). Primitives are the harder half, and
 * this is why: GPU fixed-function blending computes src*a + dst*(1-a) in
 * normalized floats and rounds at 8 bit, while blend_span uses the exact
 * multiply-shift div255. Those disagree on 39.7% of (alpha,src,dst)
 * combinations, always by exactly 1.
 *
 * Opaque draws DO match once colours are passed as 0..255 integers rather
 * than floats -- a float uniform of 0.35 is not the same value as 89/255,
 * which alone caused an off-by-one on every pixel.
 *
 * Blended draws cannot match with fixed-function blending at all. Matching
 * them requires the shader to READ the destination and do div255 in
 * integers, which means a destination read per draw (EXT_shader_framebuffer_
 * fetch where available, or a ping-pong FBO otherwise).
 */
// triangle coverage; blend_span fills integer spans. If those disagree, the
// prims golden cannot be shared and the whole determinism story changes.
const WEBGL_NODE=`${process.env.HOME}/code/cliemu/romdev/node_modules/webgl-node/index.mjs`;
const wn=await import(WEBGL_NODE);
const W=256,H=256;
const {gl}=wn.createWebGL2Context(W,H);
const mk=(t,s)=>{const x=gl.createShader(t);gl.shaderSource(x,s);gl.compileShader(x);
  if(!gl.getShaderParameter(x,gl.COMPILE_STATUS))throw new Error(gl.getShaderInfoLog(x));return x;};
const p=gl.createProgram();
gl.attachShader(p,mk(gl.VERTEX_SHADER,`#version 300 es
in vec2 aPos; uniform vec4 uR;
void main(){gl_Position=vec4(uR.xy+aPos*uR.zw,0.,1.);}`));
gl.attachShader(p,mk(gl.FRAGMENT_SHADER,`#version 300 es
precision highp float; uniform vec4 uC; out vec4 o; void main(){o=vec4(uC.rgb/255.0,uC.a/255.0);}`));
gl.linkProgram(p);gl.useProgram(p);
const b=gl.createBuffer();gl.bindBuffer(gl.ARRAY_BUFFER,b);
gl.bufferData(gl.ARRAY_BUFFER,new Float32Array([0,0,1,0,0,1,1,1]),gl.STATIC_DRAW);
const a=gl.getAttribLocation(p,'aPos');gl.enableVertexAttribArray(a);
gl.vertexAttribPointer(a,2,gl.FLOAT,false,0,0);
gl.enable(gl.BLEND);gl.blendFunc(gl.SRC_ALPHA,gl.ONE_MINUS_SRC_ALPHA);

// software: blend_span over [x0,x1) x [y0,y1), src over dst, div255 rounding
function soft(x0,y0,w,h,r,g,bb,alpha){
  const fb=new Uint8Array(W*H*4);
  const A=Math.round(alpha*255), ia=255-A;
  const d255=v=>(v*257+257)>>16;
  for(let y=y0;y<y0+h;y++)for(let x=x0;x<x0+w;x++){
    const i=(y*W+x)*4;
    fb[i]  =d255(Math.round(r*255)*A+fb[i]*ia);
    fb[i+1]=d255(Math.round(g*255)*A+fb[i+1]*ia);
    fb[i+2]=d255(Math.round(bb*255)*A+fb[i+2]*ia);
    fb[i+3]=255;
  }
  return fb;
}
function gpu(x0,y0,w,h,r,g,bb,alpha){
  gl.clearColor(0,0,0,1);gl.clear(gl.COLOR_BUFFER_BIT);
  gl.uniform4f(gl.getUniformLocation(p,'uR'),
    x0/W*2-1, 1-y0/H*2, w/W*2, -(h/H*2));
  gl.uniform4f(gl.getUniformLocation(p,'uC'),Math.round(r*255),Math.round(g*255),Math.round(bb*255),Math.round(alpha*255));
  gl.drawArrays(gl.TRIANGLE_STRIP,0,4);gl.finish();
  const o=new Uint8Array(W*H*4);
  gl.readPixels(0,0,W,H,gl.RGBA,gl.UNSIGNED_BYTE,o);
  const fb=new Uint8Array(W*H*4);
  for(let y=0;y<H;y++)fb.set(o.subarray((H-1-y)*W*4,(H-y)*W*4),y*W*4);
  return fb;
}
let bad=0,cases=0,worst=0;
for(const alpha of [1.0,0.5,0.25,0.75,0.1]){
  for(const [x,y,w,h] of [[10,10,50,30],[0,0,256,256],[7,13,33,47],[100,100,101,99]]){
    const s=soft(x,y,w,h,1.0,0.35,0.1,alpha), g=gpu(x,y,w,h,1.0,0.35,0.1,alpha);
    let d=0,mx=0;
    for(let i=0;i<s.length;i+=4){
      const dd=Math.max(Math.abs(s[i]-g[i]),Math.abs(s[i+1]-g[i+1]),Math.abs(s[i+2]-g[i+2]));
      if(dd){d++;if(dd>mx)mx=dd;}
    }
    cases++; if(d){bad++;if(mx>worst)worst=mx;
      if(bad<=5)console.log(`  a=${alpha} rect ${w}x${h}: ${d} px differ, max delta ${mx}`);}
  }
}
console.log(`\n${cases-bad}/${cases} rect cases bit-exact; worst channel delta ${worst}`);
console.log(bad===0?'PRIMITIVES ALSO BIT-EXACT':'primitives DIFFER -> blending rounding does not match');
