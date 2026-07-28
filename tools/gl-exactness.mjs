#!/usr/bin/env node
/*
 * tools/gl-exactness.mjs - CAN a GPU reproduce the software blitter exactly?
 *
 * This is the question that decides whether a GL rasterizer can share the
 * existing blit/prims goldens or needs a separate, weaker contract. The
 * answer is YES, but only with two specific corrections, and this is the
 * evidence for both.
 *
 * Nearest-neighbour, axis-aligned, integer placement: the most favourable
 * case there is. It sweeps every destination size from 8 to 200 px and
 * compares the GPU against a JS model of draw_image's sampling rule.
 *
 * The two corrections:
 *
 *   1. Sample position. A GPU interpolates UV and samples at the pixel
 *      CENTRE; draw_image indexes by destination pixel. Deriving the index
 *      from gl_FragCoord and using texelFetch removes the interpolator from
 *      the sampling rule entirely.
 *
 *   2. floor() on an exact boundary. At scales like 1.5x, idx*sw/dw lands
 *      EXACTLY on an integer for a third of columns, and fp32 division
 *      returns a hair under, so floor() drops a texel. WITHOUT the epsilon,
 *      30 of 193 swept sizes differ, up to 55% of a sprite's pixels. Delete
 *      the `+ 1.0/512.0` to watch it fail -- that is the control.
 *
 * Run: node tools/gl-exactness.mjs
 */
// GL rasterizer can share the existing goldens or needs its own contract.
// Nearest-neighbour, no blending, axis-aligned integer placement: the most
// favourable case possible. If it cannot match here, it cannot match at all.
const WEBGL_NODE = `${process.env.HOME}/code/cliemu/romdev/node_modules/webgl-node/index.mjs`;
const wn = await import(WEBGL_NODE);
const W=256,H=256, SW=64,SH=64;

const src=new Uint8Array(SW*SH*4);
for(let i=0;i<SW*SH;i++){src[i*4]=(i*7)&255;src[i*4+1]=(i*13)&255;src[i*4+2]=(i*29)&255;src[i*4+3]=255;}

// SOFTWARE: the same sampling rule draw_image uses -- floor(dstIndex * sw / dw)
function software(dstX,dstY,dw,dh){
  const fb=new Uint8Array(W*H*4);
  for(let yy=0;yy<dh;yy++)for(let xx=0;xx<dw;xx++){
    const sx=Math.floor(xx*SW/dw), sy=Math.floor(yy*SH/dh);
    const s=(sy*SW+sx)*4, d=((dstY+yy)*W+(dstX+xx))*4;
    fb[d]=src[s];fb[d+1]=src[s+1];fb[d+2]=src[s+2];fb[d+3]=255;
  }
  return fb;
}

function gpu(dstX,dstY,dw,dh){
  const {gl}=wn.createWebGL2Context(W,H);
  const vs=`#version 300 es
in vec2 aPos; in vec2 aUV; out vec2 vUV;
void main(){vUV=aUV;gl_Position=vec4(aPos,0.,1.);}`;
  const fs=`#version 300 es
precision highp float; in vec2 vUV; uniform sampler2D uTex;
uniform vec4 uRect;   // x0, y0, dw, dh in DESTINATION pixels
uniform vec2 uSrc;    // source texture size
out vec4 o;
void main(){
  // Reproduce floor(i * sw / dw) exactly: derive the integer destination
  // index from gl_FragCoord rather than reading an interpolated UV. The
  // interpolator is the problem -- at 1.5x, a third of columns land exactly
  // on a texel boundary and fp32 interpolation tips them either way.
  vec2 idx = floor(vec2(gl_FragCoord.x - uRect.x, uRect.y - gl_FragCoord.y));
  // idx*sw/dw lands EXACTLY on an integer for a third of the columns at
  // 1.5x, and fp32 division returns just under it, so floor() drops a texel.
  // Do the multiply in the numerator and nudge by a value far smaller than
  // any real spacing but larger than the fp32 error at these magnitudes.
  vec2 num = idx * uSrc;
  vec2 texel = floor((num + 1.0/512.0) / uRect.zw);
  texel = clamp(texel, vec2(0.0), uSrc - 1.0);
  o = texelFetch(uTex, ivec2(texel), 0);
}`;
  const mk=(t,s)=>{const x=gl.createShader(t);gl.shaderSource(x,s);gl.compileShader(x);
    if(!gl.getShaderParameter(x,gl.COMPILE_STATUS))throw new Error(gl.getShaderInfoLog(x));return x;};
  const p=gl.createProgram();
  gl.attachShader(p,mk(gl.VERTEX_SHADER,vs));gl.attachShader(p,mk(gl.FRAGMENT_SHADER,fs));
  gl.linkProgram(p);gl.useProgram(p);
  const tex=gl.createTexture();gl.bindTexture(gl.TEXTURE_2D,tex);
  gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA8,SW,SH,0,gl.RGBA,gl.UNSIGNED_BYTE,src);
  gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,gl.CLAMP_TO_EDGE);
  const X0=dstX/W*2-1, X1=(dstX+dw)/W*2-1;
  const Y0=1-dstY/H*2, Y1=1-(dstY+dh)/H*2;   // top-left -> clip space
  const v=[X0,Y0,0,0, X1,Y0,1,0, X1,Y1,1,1, X0,Y0,0,0, X1,Y1,1,1, X0,Y1,0,1];
  const buf=gl.createBuffer();gl.bindBuffer(gl.ARRAY_BUFFER,buf);
  gl.bufferData(gl.ARRAY_BUFFER,new Float32Array(v),gl.STATIC_DRAW);
  const aPos=gl.getAttribLocation(p,'aPos'),aUV=gl.getAttribLocation(p,'aUV');
  gl.enableVertexAttribArray(aPos);gl.vertexAttribPointer(aPos,2,gl.FLOAT,false,16,0);
  gl.enableVertexAttribArray(aUV);gl.vertexAttribPointer(aUV,2,gl.FLOAT,false,16,8);
  // Shift UV back by half a DESTINATION pixel, expressed in UV units.
  // uRect.y is the destination TOP edge in readback (bottom-left) space
  gl.uniform4f(gl.getUniformLocation(p,'uRect'), dstX, H-dstY, dw, dh);
  gl.uniform2f(gl.getUniformLocation(p,'uSrc'), SW, SH);
  gl.clearColor(0,0,0,1);gl.clear(gl.COLOR_BUFFER_BIT);
  gl.drawArrays(gl.TRIANGLES,0,6);gl.finish();
  const out=new Uint8Array(W*H*4);
  gl.readPixels(0,0,W,H,gl.RGBA,gl.UNSIGNED_BYTE,out);
  // flip to top-left origin
  const fb=new Uint8Array(W*H*4);
  for(let y=0;y<H;y++) fb.set(out.subarray((H-1-y)*W*4,(H-y)*W*4), y*W*4);
  return fb;
}

{
  const g=gpu(32,32,64,64);
  let lit=0; for(let i=0;i<g.length;i+=4) if(g[i]|g[i+1]|g[i+2]) lit++;
  console.log(`CONTROL: gpu drew ${lit} non-black pixels of ${W*H}`);
  if(!lit) console.log('  -> GPU RENDERED NOTHING; every diff below is meaningless');
}

// Sweep every destination size in range, including the awkward ones Cavern
// actually produces. Four hand-picked sizes is not evidence.
const CASES=[];
for(let d=8; d<=200; d++) CASES.push([d,d,`dw=${d}`]);
let bad=0, worstPct=0;
for(const [dw,dh,label] of CASES){
  const s=software(32,32,dw,dh), g=gpu(32,32,dw,dh);
  // Count ONLY inside the destination rect. Comparing the whole frame counts
  // background twice over and reports more differences than there are sprite
  // pixels, which is nonsense rather than evidence.
  let diff=0, maxd=0, first=null;
  for(let yy=0;yy<dh;yy++)for(let xx=0;xx<dw;xx++){
    const i=((32+yy)*W+(32+xx))*4;
    const d=Math.max(Math.abs(s[i]-g[i]),Math.abs(s[i+1]-g[i+1]),Math.abs(s[i+2]-g[i+2]));
    if(d){diff++; if(d>maxd)maxd=d; if(!first)first={x:xx,y:yy,soft:[s[i],s[i+1],s[i+2]],gpu:[g[i],g[i+1],g[i+2]]};}
  }
  let rowDiff=0, colDiff=0;
  for(let xx=0;xx<dw;xx++){const i=((32+0)*W+(32+xx))*4;
    if(s[i]!==g[i]||s[i+1]!==g[i+1]||s[i+2]!==g[i+2])rowDiff++;}
  for(let yy=0;yy<dh;yy++){const i=((32+yy)*W+(32+0))*4;
    if(s[i]!==g[i]||s[i+1]!==g[i+1]||s[i+2]!==g[i+2])colDiff++;}
  const pct=diff/(dw*dh)*100;
  if(diff){bad++; if(pct>worstPct)worstPct=pct;
    if(bad<=6) console.log(`  FAIL ${label}: ${diff}/${dw*dh} (${pct.toFixed(1)}%) max delta ${maxd}`);}
}
console.log(`\nswept ${CASES.length} destination sizes (8..200 px)`);
console.log(bad===0 ? 'ALL BIT-EXACT vs the software blitter'
                    : `${bad} sizes differ, worst ${worstPct.toFixed(1)}%`);

// Where did the GPU actually put its pixels? If the bounding boxes differ,
// the harness has a placement bug and the "99% differ" number is about
// geometry, not sampling.
{
  const g=gpu(32,32,64,64), s=software(32,32,64,64);
  const bbox=(fb)=>{let x0=1e9,y0=1e9,x1=-1,y1=-1;
    for(let y=0;y<H;y++)for(let x=0;x<W;x++){const i=(y*W+x)*4;
      if(fb[i]|fb[i+1]|fb[i+2]){if(x<x0)x0=x;if(x>x1)x1=x;if(y<y0)y0=y;if(y>y1)y1=y;}}
    return [x0,y0,x1,y1];};
  console.log('software bbox:', bbox(s));
  console.log('gpu bbox     :', bbox(g));
}
