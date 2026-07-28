// survive.js - measure whether the port is actually playable.
//
// "Less brutal" needs a number, not a vibe. This walks the intended route
// with a deliberately MEDIOCRE controller (steers toward the exit, fires
// forward, never dodges) and reports the health left in each room. If a bot
// that cannot dodge survives the route, a human comfortably can.
//
// It is also the control for the tuning: run it against the old maps and
// the new ones and compare.
const fs = require('fs');
const path = require('path');

function loadAssets(dir,pre=''){const o={};for(const n of fs.readdirSync(dir)){const p=path.join(dir,n);const r=pre?pre+'/'+n:n;if(fs.statSync(p).isDirectory())Object.assign(o,loadAssets(p,r));else o[r]=fs.readFileSync(p);}return o;}

const B = { up:256, down:512, left:1024, right:2048 };
const APP = require('path').join(__dirname, '..', 'app');

// route: [destination room, target x, y] using the real transition gaps
const ROUTE = [
  ['rm2',  4750,  950],
  ['rm3',  2450, 1460],
  ['rm2',  -120, 1460],
  ['rm4',  1300, 2750],
  ['rmBoss', 4250, 950],
];

async function main() {
  const mainLua = path.join(APP, 'main.lua');
  const lock = mainLua + '.survive-lock';
  // Refuse to run concurrently: this tool temporarily instruments main.lua,
  // and two overlapping runs will clobber each other's restore, leaving
  // debug code in the game source.
  try { fs.writeFileSync(lock, String(process.pid), { flag: 'wx' }); }
  catch { console.error('another survive.js run holds the lock; aborting'); process.exit(2); }
  const orig = fs.readFileSync(mainLua);
  fs.writeFileSync(mainLua, orig + `
local __d = love.draw
function love.draw()
  __d()
  love.log(string.format("@@%s|%.0f|%.0f|%d|%d",
    tostring(gameState and gameState.room),
    (player and player.physics) and player.physics:getX() or -1,
    (player and player.physics) and player.physics:getY() or -1,
    (player and player.health) or -1,
    (gameState and gameState.player and gameState.player.maxHealth) or 20))
end
`);

  try {
    const assets = loadAssets(APP);
    let mem; const dec = new TextDecoder(); let st = {};
    const imports = { env: {
      wc_log: (p,l)=>{ const s=dec.decode(new Uint8Array(mem.buffer,p,l));
        if (s.startsWith('@@')) { const a=s.slice(2).split('|');
          st={room:a[0],x:+a[1],y:+a[2],hp:+a[3],max:+a[4]}; } },
      wc_asset_size:(p,l)=>{const n=dec.decode(new Uint8Array(mem.buffer,p,l));return assets[n]?assets[n].length:-1;},
      wc_load_asset:(p,l,d,m)=>{const n=dec.decode(new Uint8Array(mem.buffer,p,l));if(!assets[n])return -1;
        const b=assets[n];const len=Math.min(b.length,m);new Uint8Array(mem.buffer,d,len).set(b.subarray(0,len));return len;},
      wc_debug_mark:()=>{}, emscripten_notify_memory_growth:()=>{},
    }, wasi_snapshot_preview1:new Proxy({},{get:()=>()=>0}) };

    const { instance } = await WebAssembly.instantiate(fs.readFileSync(require('path').join(__dirname,'..','..','..','build','engine.wasm')), imports);
    const e = instance.exports; mem = e.memory;
    const ip = e.wc_get_info(); const D=()=>new DataView(mem.buffer);
    const inputPtr = D().getUint32(ip+28,true), ptrPtr = D().getUint32(ip+56,true);
    e.wc_set_seed(12345); e.wc_init();

    let btn=0,mx=640,my=400,click=false;
    const tick=(n=1)=>{for(let i=0;i<n;i++){const d=D();
      d.setUint16(inputPtr,btn,true);
      d.setInt16(ptrPtr,mx,true);d.setInt16(ptrPtr+2,my,true);
      d.setUint8(ptrPtr+4,click?1:0);d.setUint8(ptrPtr+5,1);
      e.wc_render();}};

    // menu -> new game -> finish intro
    tick(90);
    mx=416;my=308;click=true;tick(8);click=false;tick(16);
    for(let k=0;k<26;k++){click=true;tick(6);click=false;tick(10);}
    tick(600);

    let deaths = 0, minHp = 99, wasParked = false;
    console.log(`  start           room=${st.room} hp=${st.hp}/${st.max}`);

    for (const [room, tx, ty] of ROUTE) {
      let f = 0, lastX=st.x, lastY=st.y, stuck=0, detour=0, dir=1;
      const hpBefore = st.hp;
      while (st.room !== room && f < 5000) {
        const dx = tx-st.x, dy = ty-st.y;
        btn = 0;
        if (detour>0){ detour--;
          btn |= (Math.abs(dx)>Math.abs(dy)) ? (dir>0?B.down:B.up) : (dir>0?B.right:B.left);
          if (Math.abs(dx)>60) btn |= dx>0?B.right:B.left;
        } else {
          if (dx<-50) btn|=B.left; else if (dx>50) btn|=B.right;
          if (dy<-50) btn|=B.up;   else if (dy>50) btn|=B.down;
        }
        mx = Math.max(0,Math.min(1279, 640 + (dx>0?400:dx<0?-400:0)));
        my = Math.max(0,Math.min(719,  360 + (dy>0?220:dy<0?-220:0)));
        click = (f % 10) < 4;
        tick(1); click=false; f++;

        if (st.hp > 0 && st.hp < minHp) minHp = st.hp;
        // Cavern parks a dead player at y=-300 for ~2 seconds; count the
        // TRANSITION into that state, not every frame spent in it.
        const parked = st.y < -200 && st.y > -400;
        if (parked && !wasParked) { deaths++;
          console.log(`     !! died heading to ${room} (frame ${f}, hp was ${hpBefore})`); }
        wasParked = parked;
        if (Math.hypot(st.x-lastX, st.y-lastY) < 0.8) {
          if (++stuck>30 && detour===0){ detour=45; dir=-dir; stuck=0; }
        } else stuck=0;
        lastX=st.x; lastY=st.y;
      }
      btn=0; tick(60);
      const reached = st.room === room;
      console.log(`  -> ${room.padEnd(8)} ${reached?'reached':'FAILED '}  hp=${st.hp}/${st.max}` +
                  `  (was ${hpBefore}, ${f} frames)`);
      if (!reached) break;
    }

    console.log(`\n  lowest health seen: ${minHp}`);
    console.log(`  deaths on the route: ${deaths}`);
    console.log(deaths === 0 ? '  VERDICT: survivable by a non-dodging bot'
                             : '  VERDICT: still too punishing');
  } finally {
    fs.writeFileSync(mainLua, orig);
    // prove the restore actually took
    if (Buffer.compare(fs.readFileSync(mainLua), orig) !== 0) {
      console.error('!! main.lua was NOT restored cleanly');
      process.exitCode = 3;
    }
    fs.rmSync(lock, { force: true });
  }
}
main().catch(e=>{console.error(e);process.exit(1);});
