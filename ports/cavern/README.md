# Cavern — a real LÖVE game, ported

[Cavern](https://github.com/challacade/cavern) is a complete 2D adventure
game by challacade, written in Lua for LÖVE and published as a reference
project for LÖVE developers. This directory runs it on the wasmcart Lua
engine.

It is the port that proves the engine: not a demo written to fit, but an
existing finished game with 5,251 lines of its own code and 5,597 lines of
bundled libraries, driving 77 sprites, 36 rooms, and a full audio set.

## Everything here is freely licensed

**No CC BY-NC-ND content ships in this port.** Cavern's original art, audio
and level designs are non-commercial / no-derivatives, so all of it was
replaced with material generated for this project (MIT):

| Replaced | Count | Generator |
|---|---|---|
| sprites | 73 | `tools/make-assets.py` |
| tilesheets | 4 | `tools/make-assets.py` |
| level layouts | 36 | `tools/make-maps.py` |
| music | 6 | `tools/make-audio.py` |
| sound effects | 24 | `tools/make-audio.py` |

Each generator is deterministic and dependency-free (hand-rolled PNG and
WAV writers, no PIL/numpy). Re-run any of them to rebuild that asset class
from scratch. The cart went from 17.8 MB to 4.6 MB in the process.

Two constraints made this non-trivial, and both are documented in the
scripts:

- **Sprites must match the original pixel dimensions exactly.** The game
  positions art by width and height (arm pivots, spritesheet frame math,
  tile seams), so a differently-sized replacement misplaces or tears the
  art even when it looks fine alone. All 77 images verified against the
  originals.
- **Maps must satisfy the loader's layer contract** in
  `source/levels/map_loader.lua`: Walls (with up/down/left/right face
  properties), Transitions (named for the destination map, with
  spawn/relative properties), a single Room rect for the camera, and a
  Main_Tiles layer. Rooms also have a minimum size, because the camera
  insets its bounds by a full `gameWidth`/`gameHeight`.

What is still upstream: the game code (MIT), the two fonts (SIL OFL), and
the original windfield kept for reference (MIT). See `app/ASSETS-LICENSE.txt`.

## The game's own source is unmodified

**All 97 of the game's Lua files are byte-identical to upstream**, verified
by `cmp` against a fresh clone. `main.lua`, `player.lua`, every enemy, every
level script — untouched.

Exactly one file was replaced:

- `source/libraries/windfield/init.lua` → a ~30-line shim forwarding to the
  engine's native `wf`. The original is preserved next to it as
  `source/libraries/windfield.love2d.orig/` for comparison.

## Why windfield was swapped, and why that is not cheating

Cavern's collision comes from [windfield](https://github.com/a327ex/windfield),
a 929-line wrapper over LÖVE's Box2D **v2** object API — `love.physics.newBody`
+ `newFixture` + `newShape`, `world:setCallbacks`, joints.

This engine embeds Box2D **v3**, which is a different model on purpose:
opaque handles, no fixtures, contact events polled per step rather than
delivered by callback. Rebuilding LÖVE's v2 object graph on top of v3 purely
so a wrapper could flatten it again would add a translation layer with real
semantic risk and no benefit.

So the engine implements `wf` natively over Box2D v3, with the same public
API. The swap is at the *library* boundary, not inside the game:

- The game calls **23 distinct windfield methods**. The engine implements
  all 23 (`newRectangleCollider`, `newBSGRectangleCollider`,
  `newCircleCollider`, `addCollisionClass`, `setCollisionClass`, `enter`,
  `getEnterCollisionData`, `applyForce`, `applyLinearImpulse`, `setType`,
  `setFixedRotation`, `setLinearDamping`, `queryCircleArea`,
  `queryPolygonArea`, `getPosition`/`getX`/`getY`, `setPosition`/`setX`/`setY`,
  `getLinearVelocity`, `setAngle`, `destroy`, `update`).
- `newBSGRectangleCollider` builds the real bevelled octagon rather than
  approximating with a box — a plain box would snag on tile seams and change
  how the character moves.
- What the game never calls (joints, preSolve/postSolve, chain colliders,
  ray casts, debug draw) raises a clear error instead of silently no-opping.

Collision classes map onto Box2D category/mask bits, and `ignores` is applied
symmetrically to match windfield's semantics.

## What porting this game forced into the engine

Every item here was discovered by the game failing, and each is a real gap
that any other LÖVE port would have hit:

| Gap | Fix |
|---|---|
| Box2D absent entirely | Box2D 3.2.0 embedded, wasm SIMD (`-msimd128 -msse2`), bound as `b2` |
| One world only | multi-world support — Cavern runs a zero-gravity gameplay world *and* a gravity world for particles/debris |
| `collider.body` was an int | windfield exposes the underlying Body object and games reach through it (`self.physics.body:getPosition()`); a raw handle broke that idiom, so `body` is now a proxy |
| `require` dropped `...` | standard Lua passes the module name to the chunk; windfield does `local path = ...` then requires its own submodules |
| Lua 5.1 dialect | LÖVE ships LuaJIT, so the whole ecosystem is 5.1: added `unpack`, `table.getn`, `loadstring`, `setfenv`/`getfenv`, `math.atan2`, `math.mod`, `math.pow`, `math.ldexp`, `table.foreach` |
| `love.mouse` missing | wired to the wasmcart pointer ABI, with a right-stick virtual cursor so mouse-aimed games stay playable on a pad |
| `love.mousepressed` never fired | menus are callback-driven, not polled — a poll-only implementation leaves games stuck on their title screen |
| WASD unmapped | Cavern advances its intro only while space/return/**w/a/s/d** is held; an incomplete keymap left it silently stuck |
| `SpriteBatch` missing | implemented as a retained draw list (tile-map libraries depend on it) |
| `love.filesystem.load` missing | Tiled `.lua` maps are loaded as chunks |
| `Image:setFilter` missing | GPU sampling knobs accepted and ignored; map libraries call them unconditionally |
| `printf` ignored `limit` | it is a **wrap width**, not just alignment — and line advance must happen in *screen* space, or lines overlap under a scaled camera |
| `newFont` rejected float sizes | games compute sizes from screen scale; fractional is normal |
| `love.image` / `love.sound` / `love.data` absent | icon-loading, `newSoundData`, and base64 decode implemented; `decompress` fails loudly |
| `os` stripped | deterministic `os.time`/`os.clock`/`os.date` — libraries seed RNGs from them |

## Difficulty

Cavern's combat numbers are upstream and unmodifiable: bats and spikes hit
for 5, fish for 6, against 20 max HP, with a 1-second invulnerability window
and **no health regeneration**. Three or four touches kill.

The decisive fact is that bats and fish are **chasers** -- `enemy:inSight()`
aggros them within 2300px whenever no wall breaks the line -- so in a 4000px
room "place it far away" achieves nothing. The generated levels therefore
budget difficulty by count and line-of-sight, not distance:

- at most one chaser per room, tucked behind geometry
- the last room before the boss uses a *stationary* spike, so the player can
  arrive with health intact
- the two one-time `health1` / `health2` upgrades (+5 max HP, full heal) sit
  on the route
- platforms stay out of the mid-height band an exit uses

`tools/survive.js` measures this rather than guessing: it walks the whole
route with a deliberately non-dodging bot and reports the health left in
each room. Current result across seeds:

```
  -> rm2      reached  hp=20/20
  -> rm3      reached  hp=20/20
  -> rm2      reached  hp=20/20
  -> rm4      reached  hp=15/20
  -> rmBoss   reached  hp=15/20
  deaths on the route: 0
```

Before tuning, the same bot died in rm2/rm3 and never reached the boss. If a
bot that cannot dodge clears the route, a human has ample headroom.

## Screenshots

Every room, captured from the running cart: see `screenshots/`
(and `screenshots/README.md` for how they were taken).

Captured from a real walked playthrough -- title screen to boss fight,
through the game's own transition colliders, at **20/20 health the whole
way**.

| Room | |
|---|---|
| title / intro | `01-title.png` `02-intro.png` |
| rm1 opening cavern | `03-rm1.png` |
| rm2 vertical shaft | `04-rm2.png` |
| rm3 water room | `05-rm3.png` |
| rm4 lower gallery | `06-rm4.png` |
| rmBoss arena | `07-rmBoss.png` |
| boss fight | `08-rmBoss-fight.png` |

## Status

Boots, plays, and is verified through the romdev MCP:

- main menu renders (TTF fonts, tiled parallax background, icons)
- "New Game" click works; the intro cutscene types out and word-wraps
- transitions to room 1, fade-in resolves
- the player swims freely (Cavern is a zero-gravity game -- the player
  applies force in all four directions rather than jumping), with the
  camera scrolling and clamping to the room bounds
- room-to-room transitions fire: rmIntro -> rm1 -> rm2 verified at full
  health
- `wasm({op:'conformance'})` → `conforms: true, issues: []`
- `lua_ok = 1` throughout

## Licensing

Everything in this port is MIT or SIL OFL — safe to redistribute, fork, and
ship commercially. See `app/ASSETS-LICENSE.txt` for the file-by-file
breakdown.

Upstream credit: Cavern's game code is (c) 2018 Kyle Schaub (challacade),
MIT — <https://github.com/challacade/cavern>. The art, audio and level
designs in *that* repository are CC BY-NC-ND and are **not** used here.

## Running it

```bash
cd ports/cavern
cp ../../build/engine.wasm main.wasm
npx wasmcart .
```

Or pack the single-file cart (4.6 MB):

```bash
npx wasmcart pack --wasm ../../build/engine.wasm --assets app \
  --name cavern --players 4 --pointer -o cavern.wasc
```

`--pointer` matters: without it the host does not deliver pointer state and
the menu cannot be clicked.
