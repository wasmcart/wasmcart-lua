#!/usr/bin/env python3
"""
make-maps.py - generate the freely-licensed level layouts for the port.

WHY: Cavern's .lua/.tmx maps are level DESIGN, which is the upstream
author's creative work under CC BY-NC-ND. Replacing the art but keeping the
levels would leave non-commercial content in a repo we want fully MIT. So
the layouts are generated here too, and the whole port becomes shippable.

WHAT THE GAME REQUIRES (read out of source/levels/map_loader.lua -- these
are hard requirements, not style choices):

  layers["Walls"]        objects with x/y/width/height + properties
                         up/down/left/right/dontDraw (which faces get the
                         rocky lip drawn on them)
  layers["Transitions"]  objects whose NAME is the destination map key, plus
                         properties spawnX/spawnY/relativeX/relativeY.
                         Negative spawn values mean "from the far edge".
  layers["Room"]         exactly one rectangle -- the camera's bounds
  layers["Main_Tiles"]   a tilelayer the renderer draws
  optional               Pickups, Enemies, Breakables, Water, Vines, Saves

Enemy `type` must be one of: bat, fish, spike, boss.
Pickup `name` must be one of: blaster, rocket, harpoon, aquaPack, health.

The generated world is a small connected cave: a hub room, a few side
rooms, a water room, and a boss arena, wired so the player can actually
walk the graph. Deterministic -- same script, same maps.

DIFFICULTY BUDGET (measured, not guessed -- see tools/survive.js)
----------------------------------------------------------------
Cavern's own combat numbers are upstream and unmodifiable: bats and spikes
hit for 5, fish for 6, against 20 max HP, with a 1-second invulnerability
window and NO health regeneration. Contact damage is therefore permanent,
and three or four touches kill.

The decisive fact is that bats and fish are CHASERS: enemy:inSight() aggros
them within 2300px whenever no wall breaks the line, so in a 4000px room
"place it far away" achieves nothing. Only these levers work:

  * COUNT. At most one chaser per room.
  * LINE OF SIGHT. Tuck chasers behind platforms so they engage only if the
    player comes to them.
  * STATIONARY vs CHASER. The last room before the boss uses a spike (which
    only hurts on contact) so the player can arrive with health intact.
  * RECOVERY. health1 / health2 are one-time permanent +5 max-HP upgrades
    that also fully heal; put them on the route. Each name may be used ONCE
    in the whole world (gameState.pickups tracks them by name).
  * CLEAR LANES. Keep platforms out of the mid-height band an exit uses.
    Geometry there turns the approach into a maze -- but note that moving a
    platform ONTO an enemy is worse than leaving it: an attempt to widen
    rm4's lane dropped the arrival health from 15 to 8.

Current result: a deliberately non-dodging bot walks the whole route
rm1 -> rm2 -> rm3 -> rm2 -> rm4 -> rmBoss with ZERO deaths and never drops
below 15/20 health.
"""
import os

OUT = os.path.join(os.path.dirname(__file__), "..", "app", "maps")

TILE = 128
_next_id = [100]


def nid():
    _next_id[0] += 1
    return _next_id[0]


def obj(x, y, w, h, name="", typ="", props=None):
    return {
        "id": nid(), "name": name, "type": typ, "shape": "rectangle",
        "x": x, "y": y, "width": w, "height": h,
        "rotation": 0, "visible": True, "properties": props or {},
    }


# ── Lua serialization (Tiled's own export format) ───────────────────

def lua_val(v, ind):
    pad = "  " * ind
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return repr(v) if isinstance(v, float) else str(v)
    if isinstance(v, str):
        return '"%s"' % v.replace('\\', '\\\\').replace('"', '\\"')
    if isinstance(v, list):
        if not v:
            return "{}"
        inner = ",\n".join(pad + "  " + lua_val(x, ind + 1) for x in v)
        return "{\n" + inner + "\n" + pad + "}"
    if isinstance(v, dict):
        if not v:
            return "{}"
        items = []
        for k, val in v.items():
            key = k if k.isidentifier() else '["%s"]' % k
            items.append(pad + "  " + key + " = " + lua_val(val, ind + 1))
        return "{\n" + ",\n".join(items) + "\n" + pad + "}"
    if v is None:
        return "nil"
    raise TypeError(type(v))


def tilelayer(name, w, h, data):
    return {
        "type": "tilelayer", "name": name, "x": 0, "y": 0,
        "width": w, "height": h, "visible": True, "opacity": 1,
        "offsetx": 0, "offsety": 0, "properties": {},
        "encoding": "lua", "data": data,
    }


def objgroup(name, objects):
    return {
        "type": "objectgroup", "name": name, "visible": True, "opacity": 1,
        "offsetx": 0, "offsety": 0, "draworder": "topdown",
        "properties": {}, "objects": objects,
    }


TILESETS = [
    {
        "name": "main_sheet", "firstgid": 1, "tilewidth": TILE,
        "tileheight": TILE, "spacing": 0, "margin": 0,
        "image": "tilesheets/sheet1.png", "imagewidth": 128, "imageheight": 128,
        "tileoffset": {"x": 0, "y": 0},
        "grid": {"orientation": "orthogonal", "width": TILE, "height": TILE},
        "properties": {}, "terrains": {}, "tilecount": 1, "tiles": {},
    },
    {
        "name": "3by3", "firstgid": 2, "tilewidth": TILE,
        "tileheight": TILE, "spacing": 0, "margin": 4,
        "image": "tilesheets/3by3_1.png", "imagewidth": 392, "imageheight": 392,
        "tileoffset": {"x": 0, "y": 0},
        "grid": {"orientation": "orthogonal", "width": TILE, "height": TILE},
        "properties": {}, "terrains": {}, "tilecount": 9, "tiles": {},
    },
]


def write_map(name, cols, rows, layers):
    doc = {
        "version": "1.1", "luaversion": "5.1", "tiledversion": "1.0.3",
        "orientation": "orthogonal", "renderorder": "right-down",
        "width": cols, "height": rows,
        "tilewidth": TILE, "tileheight": TILE,
        "nextobjectid": _next_id[0] + 50,
        "properties": {},
        "tilesets": TILESETS,
        "layers": layers,
    }
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, name + ".lua"), "w") as f:
        f.write("return " + lua_val(doc, 0) + "\n")


# ── room construction ───────────────────────────────────────────────

def solid_border_walls(cols, rows, openings):
    """Walls around the room edge, leaving gaps where `openings` says.

    openings: set of "left"/"right"/"top"/"bottom". A gap is 3 tiles tall
    (or wide) positioned so the player can walk/fall through it.
    """
    W, H = cols * TILE, rows * TILE
    T = 2 * TILE                        # border thickness, 2 tiles
    walls = []

    def w(x, y, ww, hh, **faces):
        props = {k: True for k, v in faces.items() if v}
        walls.append(obj(x, y, ww, hh, props=props))

    gap_h = 3 * TILE
    gap_y = (rows // 2) * TILE - TILE

    # left / right
    if "left" in openings:
        w(0, 0, T, gap_y, right=True, down=True)
        w(0, gap_y + gap_h, T, H - gap_y - gap_h, right=True, up=True)
    else:
        w(0, 0, T, H, right=True)
    if "right" in openings:
        w(W - T, 0, T, gap_y, left=True, down=True)
        w(W - T, gap_y + gap_h, T, H - gap_y - gap_h, left=True, up=True)
    else:
        w(W - T, 0, T, H, left=True)

    # top / bottom (full width; corners overlap the side walls, which is fine)
    gap_x = (cols // 2) * TILE - TILE
    gap_w = 3 * TILE
    if "top" in openings:
        w(0, 0, gap_x, T, down=True)
        w(gap_x + gap_w, 0, W - gap_x - gap_w, T, down=True)
    else:
        w(0, 0, W, T, down=True)
    if "bottom" in openings:
        w(0, H - T, gap_x, T, up=True)
        w(gap_x + gap_w, H - T, W - gap_x - gap_w, T, up=True)
    else:
        w(0, H - T, W, T, up=True)

    return walls


def platform(x_t, y_t, w_t, h_t=1):
    """A floating ledge, in TILE units, with the rocky lip on top."""
    return obj(x_t * TILE, y_t * TILE, w_t * TILE, h_t * TILE,
               props={"up": True})


def tiles_from_walls(cols, rows, walls):
    """Paint tile 1 wherever a wall rectangle covers a cell.

    The renderer draws Main_Tiles; the physics uses the Wall objects. If the
    two disagree the player collides with invisible geometry (or walks
    through visible rock), so both are derived from the same rectangles.
    """
    data = [0] * (cols * rows)
    for w in walls:
        x0 = max(0, w["x"] // TILE)
        y0 = max(0, w["y"] // TILE)
        x1 = min(cols, -(-(w["x"] + w["width"]) // TILE))
        y1 = min(rows, -(-(w["y"] + w["height"]) // TILE))
        for ty in range(int(y0), int(y1)):
            for tx in range(int(x0), int(x1)):
                data[ty * cols + tx] = 1
    return data


# The camera clamps look-at into the Room rect inset by a full
# gameWidth/gameHeight on each side (see source/ui/cam.lua -- the camera is
# zoomed out, so it insets by the FULL dimension, not half). gameWidth is
# 1152 = 9 tiles and gameHeight is 768 = 6 tiles, so a Room smaller than
# 18x12 tiles makes upperBound exceed lowerBound and the view sticks to an
# edge. Every room below is therefore at least 20x14 tiles.
MIN_COLS, MIN_ROWS = 20, 14


def make_room(name, cols, rows, openings, transitions,
              platforms=(), enemies=(), pickups=(), water=None,
              breakables=(), vines=(), saves=()):
    assert cols >= MIN_COLS and rows >= MIN_ROWS, \
        "%s is %dx%d; camera needs >= %dx%d tiles" % (name, cols, rows,
                                                      MIN_COLS, MIN_ROWS)
    W, H = cols * TILE, rows * TILE
    walls = solid_border_walls(cols, rows, openings)
    for p in platforms:
        walls.append(p)

    layers = [
        tilelayer("Main_Tiles", cols, rows, tiles_from_walls(cols, rows, walls)),
        objgroup("Window_Size", []),
        objgroup("Room", [obj(TILE, TILE, W - 2 * TILE, H - 2 * TILE, name="main")]),
        objgroup("Walls", walls),
        objgroup("Transitions", transitions),
        objgroup("Pickups", list(pickups)),
        objgroup("Enemies", list(enemies)),
        objgroup("Breakables", list(breakables)),
        objgroup("Vines", list(vines)),
    ]
    if water:
        layers.append(objgroup("Water", water))
    if saves:
        layers.append(objgroup("Saves", list(saves)))

    write_map(name, cols, rows, layers)


def exit_trigger(cols, rows, side, to, **spawn):
    """A transition filling the border gap on `side`, so it cannot be missed.

    solid_border_walls() cuts a 3-tile gap centred at rows//2-1 (or
    cols//2-1 for top/bottom). The trigger must cover that exact span --
    a trigger placed at a different height is simply never touched.
    """
    gap_y = (rows // 2) * TILE - TILE
    gap_x = (cols // 2) * TILE - TILE
    span = 3 * TILE
    if side == "right":
        return trans(to, (cols - 2) * TILE, gap_y, 2 * TILE, span, **spawn)
    if side == "left":
        return trans(to, 0, gap_y, 2 * TILE, span, **spawn)
    if side == "top":
        return trans(to, gap_x, 0, span, 2 * TILE, **spawn)
    return trans(to, gap_x, (rows - 2) * TILE, span, 2 * TILE, **spawn)


def trans(to, x, y, w, h, spawnX=0, spawnY=0, relX=0, relY=0):
    return obj(x, y, w, h, name=to, props={
        "spawnX": spawnX, "spawnY": spawnY,
        "relativeX": relX, "relativeY": relY,
    })


def enemy(kind, x_t, y_t, arg=0):
    return obj(x_t * TILE, y_t * TILE, 64, 64, typ=kind, props={"arg": arg})


def pickup(kind, x_t, y_t):
    return obj(x_t * TILE, y_t * TILE, 64, 64, name=kind)


# ── the world ───────────────────────────────────────────────────────
#
# A compact connected cave. Room sizes are in tiles; the player enters
# rm1 from the intro. Side exits are wired both ways so the graph is
# walkable rather than a dead end.

def build():
    # rm1: opening cavern.
    #
    # The intro hard-codes the player to (512, 260) -- 4 tiles in, 2 tiles
    # down -- so this room MUST have solid ground under that point or the
    # player falls the whole height on arrival and the camera clamps to the
    # floor showing nothing. A ledge at tile y=4 spanning the spawn column
    # catches them; the rest of the room opens up to the right.
    make_room(
        "rm1", 38, 14, {"right"},
        [exit_trigger(38, 14, "right", "rm2", spawnX=TILE * 3)],
        # Ledges form a climbable staircase from the floor (y=12) up to the
        # right-hand exit gap (y=6..9), so the exit is actually reachable.
        platforms=[platform(1, 4, 8),                    # spawn ledge
                   platform(12, 10, 4), platform(18, 9, 4),
                   platform(24, 8, 4), platform(30, 8, 5)],
        enemies=[],                              # tutorial room: no threats
        pickups=[pickup("blaster", 19, 8)],
    )

    # rm2: vertical shaft, exits left/right/bottom
    make_room(
        "rm2", 20, 22, {"left", "right", "bottom"},
        [exit_trigger(20, 22, "left", "rm1", spawnX=-TILE * 5),
         exit_trigger(20, 22, "right", "rm3", spawnX=TILE * 3),
         exit_trigger(20, 22, "bottom", "rm4", spawnY=TILE * 3)],
        platforms=[platform(4, 17, 5), platform(12, 14, 5), platform(5, 11, 4),
                   platform(13, 8, 5), platform(4, 5, 4)],
        # one chaser, tucked in the top-left pocket behind platforms so it
        # only engages if the player goes up there
        enemies=[enemy("bat", 6, 3)],
        pickups=[pickup("health1", 15, 17)],   # +5 max HP, one-time
    )

    # rm3: water room
    make_room(
        "rm3", 30, 16, {"left"},
        [exit_trigger(30, 16, "left", "rm2", spawnX=-TILE * 5)],
        platforms=[platform(6, 6, 4), platform(14, 5, 5), platform(22, 7, 4)],
        # fish live in the water (y>=10); one only, at the far end, so the
        # player can surface and retreat rather than being cornered
        enemies=[enemy("fish", 22, 12)],
        pickups=[pickup("aquaPack", 26, 5), pickup("health2", 8, 5)],
        water=[obj(2 * TILE, 10 * TILE, 26 * TILE, 5 * TILE, typ="top")],
    )

    # rm4: lower gallery, leads to the boss
    make_room(
        "rm4", 34, 14, {"top", "right"},
        [exit_trigger(34, 14, "top", "rm2", spawnY=-TILE * 5),
         exit_trigger(34, 14, "right", "rmBoss", spawnX=TILE * 4)],
        platforms=[platform(6, 10, 5), platform(14, 8, 6), platform(24, 9, 5)],
        # Last room before the boss. A CHASER here drains the player dry no
        # matter where it starts (bats home in within 2300px), so this room
        # gets a stationary spike instead -- it only hurts on contact, and
        # it sits well below the mid-height lane to the boss door.
        enemies=[enemy("spike", 5, 12)],
        pickups=[pickup("rocket", 16, 7)],
        breakables=[obj(29 * TILE, 9 * TILE, 256, 256)],
    )

    # rmBoss: arena
    make_room(
        "rmBoss", 26, 16, {"left"},
        [exit_trigger(26, 16, "left", "rm4", spawnX=-TILE * 5)],
        platforms=[platform(4, 12, 4), platform(18, 12, 4)],
        enemies=[enemy("boss", 13, 7)],
    )

    # rmBossAfter: quiet room the boss fight exits into
    make_room(
        "rmBossAfter", 20, 14, {"left"},
        [exit_trigger(20, 14, "left", "rmBoss", spawnX=-TILE * 5)],
        platforms=[platform(8, 8, 5)],
        pickups=[pickup("harpoon", 10, 7)],
    )

    # Remaining named rooms the loader requires. They must EXIST (loadMaps
    # loads all 34 by name at boot) but the generated world does not route
    # through them, so they are small, safe, and exit back to rm1.
    for i in range(5, 29):
        make_room(
            "rm%d" % i, 20, 14, {"left"},
            [exit_trigger(20, 14, "left", "rm1", spawnX=TILE * 3)],
            platforms=[platform(5, 7, 5)],
        )

    # menu / intro / credits are BACKDROPS -- the UI draws on top of them,
    # so they get a Room rect (the camera requires one) but NO walls, or the
    # title screen renders a boxed-in cave behind the menu.
    for special in ("rmMainMenu", "rmIntro", "rmCredits", "blank", "blank2"):
        cols, rows = 20, 14
        write_map(special, cols, rows, [
            tilelayer("Main_Tiles", cols, rows, [0] * (cols * rows)),
            objgroup("Window_Size", []),
            objgroup("Room", [obj(TILE, TILE, (cols - 2) * TILE,
                                  (rows - 2) * TILE, name="main")]),
            objgroup("Walls", []),
            objgroup("Transitions", []),
            objgroup("Pickups", []),
            objgroup("Enemies", []),
            objgroup("Breakables", []),
            objgroup("Vines", []),
        ])

    print("wrote %d maps to %s" % (len(os.listdir(OUT)), OUT))


if __name__ == "__main__":
    build()
