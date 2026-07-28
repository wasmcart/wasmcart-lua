# Playthrough

Every room of the port, captured from a real walked playthrough. Nothing
here is a mockup or a room-jump: the bot started at the title screen, chose
New Game, sat through the intro, and then **walked the whole world through
the game's own transition colliders**, screenshotting on arrival.

Result: **20/20 health the entire way, zero damage taken, zero deaths.**

| | Room | What it shows |
|---|---|---|
| `01-title` | rmMainMenu | TTF fonts, tiled parallax backdrop, UI icons |
| `02-intro` | rmIntro | the intro cutscene typing out, with word wrap |
| `03-rm1` | rm1 | opening cavern after the intro drop; blaster pickup ahead |
| `04-rm2` | rm2 | vertical shaft; player firing the blaster at the lone bat |
| `05-rm3` | rm3 | water room; animated surface + ripples, health upgrade on the ledge, one starfish |
| `06-rm4` | rm4 | lower gallery; rocket launcher on the route, spike parked off it |
| `07-rmBoss` | rmBoss | boss arena, entered at full health with the rocket launcher |
| `08-rmBoss-fight` | rmBoss | the boss fight underway (note the room's lighting change) |

## Why this run is the interesting one

An earlier attempt at the same capture could not do this. With the original
enemy placement, a bot that cannot dodge died in rm2/rm3 and never reached
the boss, so those screenshots had to be taken by calling the game's
`changeToMap()` directly rather than by playing.

After the difficulty pass (see the port README, and `tools/survive.js` which
measures it), the same non-dodging bot clears the entire route untouched.
That is the difference between "the rooms render" and "the game is
playable", and it is why these shots were re-taken.

All art, audio and level layouts are generated (MIT) — see
`../app/ASSETS-LICENSE.txt`.
