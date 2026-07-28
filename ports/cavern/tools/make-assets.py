#!/usr/bin/env python3
"""
make-assets.py - generate the freely-licensed art for the Cavern port.

WHY THIS EXISTS: Cavern's original art, maps and audio are CC BY-NC-ND
(attribution, non-commercial, NO DERIVATIVES). That is fine for a private
demo but cannot ship in a repo we may publish. Rather than hunt for
third-party replacements with their own attribution chains, every sprite
here is generated procedurally by this script, so the output is our own
work and ships under the project's MIT license with no strings.

The constraint that matters: the game's own source is UNMODIFIED, so each
replacement must match the original's PIXEL DIMENSIONS exactly. Cavern
positions sprites by their width/height (arm pivots, tile seams, spritesheet
frame math), so a differently-sized image would misplace or tear the art
even though it "looks fine" in isolation. Every size below was read off the
original PNGs.

Output is deterministic: same script, same bytes. No PIL/numpy dependency --
PNGs are written by hand so the script runs anywhere Python does.
"""
import os
import struct
import zlib
import math

OUT = os.path.join(os.path.dirname(__file__), "..", "app")

# ── minimal PNG writer ──────────────────────────────────────────────

def write_png(path, w, h, px):
    """px: list of (r,g,b,a) rows, row-major, top row first."""
    raw = bytearray()
    for y in range(h):
        raw.append(0)                     # filter type 0 (none)
        row = px[y]
        for x in range(w):
            r, g, b, a = row[x]
            raw += bytes((r, g, b, a))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
           + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
           + chunk(b"IEND", b""))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(png)


def blank(w, h, rgba=(0, 0, 0, 0)):
    return [[rgba for _ in range(w)] for _ in range(h)]


# ── deterministic value noise (no RNG state, no numpy) ──────────────

def hash2(x, y, seed=0):
    n = (x * 374761393 + y * 668265263 + seed * 1442695040888963407) & 0xFFFFFFFF
    n = ((n ^ (n >> 13)) * 1274126177) & 0xFFFFFFFF
    return ((n ^ (n >> 16)) & 0xFFFF) / 65535.0


def smooth_noise(x, y, scale, seed=0):
    fx, fy = x / scale, y / scale
    x0, y0 = int(math.floor(fx)), int(math.floor(fy))
    tx, ty = fx - x0, fy - y0
    tx = tx * tx * (3 - 2 * tx)
    ty = ty * ty * (3 - 2 * ty)
    a = hash2(x0, y0, seed);       b = hash2(x0 + 1, y0, seed)
    c = hash2(x0, y0 + 1, seed);   d = hash2(x0 + 1, y0 + 1, seed)
    return (a + (b - a) * tx) * (1 - ty) + (c + (d - c) * tx) * ty


def tile_fbm(x, y, w, h, scale, seed=0, octaves=3):
    """fbm that WRAPS exactly at (w,h).

    Blends four shifted copies weighted by position, so the value at x=0
    equals the value at x=w. Without this, every 128px tile carries the
    same internal gradient and the wall reads as a grid no matter how
    detailed the noise is -- which is exactly what happened on the first
    two attempts here.
    """
    fx, fy = x / w, y / h
    a = fbm(x, y, scale, seed, octaves)
    b = fbm(x - w, y, scale, seed, octaves)
    c = fbm(x, y - h, scale, seed, octaves)
    d = fbm(x - w, y - h, scale, seed, octaves)
    top = a * (1 - fx) + b * fx
    bot = c * (1 - fx) + d * fx
    return top * (1 - fy) + bot * fy


def fbm(x, y, scale, seed=0, octaves=3):
    total, amp, norm = 0.0, 1.0, 0.0
    for o in range(octaves):
        total += smooth_noise(x, y, scale / (2 ** o), seed + o * 17) * amp
        norm += amp
        amp *= 0.5
    return total / norm


def lerp(a, b, t):
    return a + (b - a) * t


def mix(c1, c2, t):
    return tuple(int(round(lerp(c1[i], c2[i], t))) for i in range(3))


# ── shape helpers ───────────────────────────────────────────────────

def ellipse_a(px, w, h, cx, cy, rx, ry, color, soft=1.0):
    """Anti-aliased filled ellipse."""
    for y in range(max(0, int(cy - ry - 2)), min(h, int(cy + ry + 3))):
        for x in range(max(0, int(cx - rx - 2)), min(w, int(cx + rx + 3))):
            dx, dy = (x + 0.5 - cx) / max(rx, 0.001), (y + 0.5 - cy) / max(ry, 0.001)
            d = math.sqrt(dx * dx + dy * dy)
            if d <= 1.0 + soft / max(rx, 1):
                a = 1.0 if d <= 1.0 else max(0.0, 1.0 - (d - 1.0) * max(rx, 1) / soft)
                if a > 0:
                    r, g, b = color
                    old = px[y][x]
                    na = a * 255
                    if old[3] == 0:
                        px[y][x] = (r, g, b, int(na))
                    else:
                        t = a
                        px[y][x] = (int(lerp(old[0], r, t)), int(lerp(old[1], g, t)),
                                    int(lerp(old[2], b, t)), max(old[3], int(na)))


def rrect(px, w, h, x0, y0, x1, y1, rad, color):
    """Rounded rectangle, inclusive bounds."""
    for y in range(max(0, y0), min(h, y1 + 1)):
        for x in range(max(0, x0), min(w, x1 + 1)):
            dx = max(x0 + rad - x, 0, x - (x1 - rad))
            dy = max(y0 + rad - y, 0, y - (y1 - rad))
            if dx * dx + dy * dy <= rad * rad + rad:
                px[y][x] = (color[0], color[1], color[2], 255)


# ── palette (a cohesive cave/sci-fi look, our own choice) ───────────
ROCK_DARK   = (38, 30, 26)
ROCK_MID    = (74, 58, 46)
ROCK_LIGHT  = (104, 84, 66)
SUIT        = (176, 150, 168)
SUIT_DARK   = (120, 98, 116)
VISOR       = (232, 240, 248)
METAL       = (150, 158, 172)
METAL_DARK  = (96, 104, 120)
ENERGY      = (110, 214, 255)
ENERGY_DIM  = (48, 130, 190)
DANGER      = (226, 92, 84)
DANGER_DARK = (150, 52, 52)
FLESH       = (196, 108, 148)
FLESH_DARK  = (128, 62, 100)
GOLD        = (240, 196, 96)
GREEN       = (126, 200, 130)
WATER       = (72, 148, 200)


# ── generators ──────────────────────────────────────────────────────

def rocky_texture(w, h, seed=1, base=ROCK_MID, dark=ROCK_DARK, light=ROCK_LIGHT):
    """Layered rock.

    A single noise octave tiles visibly. Broad stretched strata, mid-scale
    blotches and fine pitting break up the repeat.

    NOTE: an earlier version darkened tile EDGES to suggest separate
    stones. That backfired badly -- because the game tiles this image on a
    128px grid, per-tile edge shading renders as a hard brick lattice
    across the whole wall, which reads far more repetitive than the noise
    it was meant to hide. The texture must be edge-neutral so neighbouring
    tiles blend.
    """
    px = blank(w, h)
    for y in range(h):
        for x in range(w):
            strata = tile_fbm(x, y * 2.2, w, h * 2.2, 52, seed, 3)
            blotch = tile_fbm(x, y, w, h, 17, seed + 41, 4)
            pit = hash2(x, y, seed + 99)
            v = strata * 0.45 + blotch * 0.45 + (pit * 0.18 - 0.09)

            if v < 0.42:
                c = mix(dark, base, max(0.0, v / 0.42))
            else:
                c = mix(base, light, min(1.0, (v - 0.42) / 0.5))

            # dark speckle: small clustered pits
            if pit > 0.955 and blotch < 0.6:
                c = mix(c, dark, 0.55)

            px[y][x] = (c[0], c[1], c[2], 255)
    return px


def gen_wall(path, w=128, h=128, seed=3):
    write_png(path, w, h, rocky_texture(w, h, seed))


def gen_bg(path, w=512, h=512):
    """Seamless-ish dark cave backdrop; the game tiles it 9x7."""
    px = blank(w, h)
    for y in range(h):
        for x in range(w):
            # wrap coordinates so the tiling seam is not obvious
            n = (fbm(x, y, 90, 7, 4) * 0.6
                 + fbm((x + w // 2) % w, (y + h // 2) % h, 45, 11, 3) * 0.4)
            c = mix((18, 14, 13), (54, 42, 34), n)
            px[y][x] = (c[0], c[1], c[2], 255)
    write_png(path, w, h, px)


def gen_rocky_surface(path, w=128, h=7, tone=1.0):
    """The lip drawn along the top edge of walls."""
    px = blank(w, h)
    for x in range(w):
        bump = int(fbm(x, 0, 14, 21, 3) * (h - 2))
        for y in range(h):
            if y >= bump:
                t = (y - bump) / max(1, h - bump)
                c = mix(ROCK_LIGHT, ROCK_MID, t)
                c = tuple(min(255, int(v * tone)) for v in c)
                px[y][x] = (c[0], c[1], c[2], 255)
    write_png(path, w, h, px)


def gen_player_body(path, w=80, h=171):
    """Suited explorer torso: scaled from the original 80x171 proportions."""
    px = blank(w, h)
    sx, sy = w / 80.0, h / 171.0

    def R(x0, y0, x1, y1, rad, col):
        rrect(px, w, h, int(x0 * sx), int(y0 * sy), int(x1 * sx), int(y1 * sy),
              max(2, int(rad * min(sx, sy))), col)

    R(14, 6, 66, 96, 16, SUIT)                    # torso
    R(10, 22, 20, 74, 6, SUIT_DARK)               # shoulder pads
    R(60, 22, 70, 74, 6, SUIT_DARK)
    R(18, 88, 62, 104, 6, METAL_DARK)             # belt
    R(20, 100, 60, 128, 10, SUIT_DARK)            # hips
    R(22, 120, 38, 164, 8, SUIT)                  # legs
    R(42, 120, 58, 164, 8, SUIT)
    R(20, 160, 40, 168, 4, METAL_DARK)            # boots
    R(40, 160, 60, 168, 4, METAL_DARK)

    # chest lamp
    ellipse_a(px, w, h, w / 2, 44 * sy, 11 * sx, 11 * sy, ENERGY_DIM)
    ellipse_a(px, w, h, w / 2, 44 * sy, 8 * sx, 8 * sy, ENERGY)
    ellipse_a(px, w, h, w / 2, 42 * sy, 4 * sx, 4 * sy, VISOR)

    # rim light on the right, shadow on the left
    for y in range(h):
        for x in range(w):
            if px[y][x][3]:
                r, g, b, a = px[y][x]
                if x < w * 0.30:
                    px[y][x] = (int(r * 0.74), int(g * 0.74), int(b * 0.74), a)
                elif x > w * 0.84:
                    px[y][x] = (min(255, int(r * 1.20)), min(255, int(g * 1.20)),
                                min(255, int(b * 1.22)), a)
    write_png(path, w, h, px)


def gen_helmet(path, w=70, h=85):
    px = blank(w, h)
    ellipse_a(px, w, h, w / 2, h * 0.52, w * 0.47, h * 0.47, SUIT_DARK)
    ellipse_a(px, w, h, w / 2, h * 0.50, w * 0.43, h * 0.43, SUIT)
    # visor: dark glass with a bright sweep
    ellipse_a(px, w, h, w * 0.57, h * 0.46, w * 0.33, h * 0.31, (26, 34, 46))
    ellipse_a(px, w, h, w * 0.62, h * 0.39, w * 0.19, h * 0.13, (96, 150, 190))
    ellipse_a(px, w, h, w * 0.66, h * 0.35, w * 0.11, h * 0.07, VISOR)
    # antenna nub
    ellipse_a(px, w, h, w * 0.22, h * 0.22, w * 0.07, h * 0.06, METAL)
    write_png(path, w, h, px)


def gen_arm(path, w, h, kind="plain"):
    px = blank(w, h)
    if w > h:  # horizontal arm / weapon
        rrect(px, w, h, 2, int(h * 0.32), int(w * 0.46), int(h * 0.72), 6, SUIT)
        if kind == "blaster":
            rrect(px, w, h, int(w * 0.40), int(h * 0.24), w - 3, int(h * 0.78), 7, METAL)
            ellipse_a(px, w, h, w - 10, h * 0.5, 6, 6, ENERGY)
        elif kind == "rocket":
            rrect(px, w, h, int(w * 0.36), int(h * 0.18), w - 3, int(h * 0.84), 9, METAL_DARK)
            rrect(px, w, h, int(w * 0.60), int(h * 0.30), w - 6, int(h * 0.70), 5, DANGER)
        elif kind == "spear":
            rrect(px, w, h, int(w * 0.40), int(h * 0.42), w - 3, int(h * 0.60), 3, METAL)
            ellipse_a(px, w, h, w - 6, h * 0.5, 5, 4, VISOR)
        elif kind == "spear_loaded":
            rrect(px, w, h, int(w * 0.34), int(h * 0.40), w - 3, int(h * 0.62), 3, METAL)
            ellipse_a(px, w, h, w - 7, h * 0.5, 6, 5, ENERGY)
    else:      # vertical arm
        rrect(px, w, h, int(w * 0.20), 2, int(w * 0.80), h - 3, max(3, w // 3), SUIT)
    write_png(path, w, h, px)


def gen_flyer_body(path, w=92, h=92):
    px = blank(w, h)
    ellipse_a(px, w, h, w / 2, h / 2, w * 0.46, h * 0.42, FLESH)
    ellipse_a(px, w, h, w / 2, h * 0.42, w * 0.30, h * 0.26, FLESH_DARK)
    write_png(path, w, h, px)


def gen_eye(path, w, h, iris=DANGER):
    px = blank(w, h)
    ellipse_a(px, w, h, w / 2, h / 2, w * 0.48, h * 0.48, VISOR)
    ellipse_a(px, w, h, w / 2, h / 2, w * 0.28, h * 0.28, iris)
    ellipse_a(px, w, h, w * 0.42, h * 0.40, w * 0.10, h * 0.10, (255, 255, 255))
    write_png(path, w, h, px)


def gen_wing(path, w, h):
    px = blank(w, h)
    for x in range(w):
        t = x / max(1, w - 1)
        span = int((h / 2) * math.sin(math.pi * min(1.0, t * 1.15)) * 0.95)
        cy = h // 2
        for y in range(cy - span, cy + span + 1):
            if 0 <= y < h:
                a = int(200 * (1 - t * 0.45))
                px[y][x] = (FLESH[0], FLESH[1], FLESH[2], max(60, a))
    write_png(path, w, h, px)


def gen_spike_body(path, w=146, h=118):
    px = blank(w, h)
    ellipse_a(px, w, h, w / 2, h * 0.58, w * 0.40, h * 0.38, (92, 84, 112))
    for i in range(11):
        ang = math.pi * (0.08 + 0.84 * i / 10)
        ex = w / 2 - math.cos(ang) * w * 0.44
        ey = h * 0.58 - math.sin(ang) * h * 0.46
        ellipse_a(px, w, h, ex, ey, 9, 9, (150, 142, 178))
    ellipse_a(px, w, h, w / 2, h * 0.55, w * 0.16, h * 0.16, DANGER)
    write_png(path, w, h, px)


def gen_starfish(path, w=142, h=138):
    px = blank(w, h)
    for i in range(5):
        ang = -math.pi / 2 + i * (2 * math.pi / 5)
        for t in range(0, 100):
            f = t / 100.0
            ex = w / 2 + math.cos(ang) * (w * 0.44) * f
            ey = h / 2 + math.sin(ang) * (h * 0.44) * f
            ellipse_a(px, w, h, ex, ey, 16 * (1 - f * 0.75), 16 * (1 - f * 0.75), GOLD)
    ellipse_a(px, w, h, w / 2, h / 2, w * 0.20, h * 0.20, (255, 224, 150))
    write_png(path, w, h, px)


def gen_boss_body(path, w=1792, h=448):
    """Boss spritesheet: 4 frames of 448x448 laid out horizontally."""
    px = blank(w, h)
    fw = w // 4
    for f in range(4):
        ox = f * fw
        cx, cy = ox + fw / 2, h / 2
        pulse = 1.0 + 0.05 * math.sin(f / 4 * 2 * math.pi)
        ellipse_a(px, w, h, cx, cy, fw * 0.42 * pulse, h * 0.42 * pulse, FLESH_DARK)
        ellipse_a(px, w, h, cx, cy, fw * 0.32 * pulse, h * 0.32 * pulse, FLESH)
        for i in range(8):
            ang = i * math.pi / 4 + f * 0.15
            ex = cx + math.cos(ang) * fw * 0.36
            ey = cy + math.sin(ang) * h * 0.36
            ellipse_a(px, w, h, ex, ey, 22, 22, FLESH_DARK)
    write_png(path, w, h, px)


def gen_egg(path, w=106, h=132):
    px = blank(w, h)
    ellipse_a(px, w, h, w / 2, h * 0.56, w * 0.44, h * 0.42, (214, 206, 186))
    ellipse_a(px, w, h, w * 0.42, h * 0.40, w * 0.18, h * 0.14, (245, 240, 228))
    write_png(path, w, h, px)


def gen_bubble(path, w=38, h=38):
    px = blank(w, h)
    ellipse_a(px, w, h, w / 2, h / 2, w * 0.46, h * 0.46, (120, 90, 140))
    ellipse_a(px, w, h, w * 0.38, h * 0.36, w * 0.14, h * 0.14, (220, 200, 240))
    write_png(path, w, h, px)


def gen_pickup_item(path, w, h, color, glyph="orb"):
    px = blank(w, h)
    cx, cy = w / 2, h / 2
    r = min(w, h) * 0.42
    ellipse_a(px, w, h, cx, cy, r, r, color)
    ellipse_a(px, w, h, cx, cy, r * 0.72, r * 0.72,
              tuple(min(255, int(c * 1.25)) for c in color))
    if glyph == "cross":
        rrect(px, w, h, int(cx - r * 0.14), int(cy - r * 0.5),
              int(cx + r * 0.14), int(cy + r * 0.5), 3, (255, 255, 255))
        rrect(px, w, h, int(cx - r * 0.5), int(cy - r * 0.14),
              int(cx + r * 0.5), int(cy + r * 0.14), 3, (255, 255, 255))
    else:
        ellipse_a(px, w, h, cx - r * 0.2, cy - r * 0.24, r * 0.18, r * 0.14, (255, 255, 255))
    write_png(path, w, h, px)


def gen_weapon_pickup(path, w, h, body, accent):
    px = blank(w, h)
    rrect(px, w, h, int(w * 0.08), int(h * 0.34), int(w * 0.92), int(h * 0.66),
          int(min(w, h) * 0.14), body)
    rrect(px, w, h, int(w * 0.62), int(h * 0.22), int(w * 0.94), int(h * 0.78),
          int(min(w, h) * 0.12), accent)
    ellipse_a(px, w, h, w * 0.24, h * 0.5, min(w, h) * 0.12, min(w, h) * 0.12, ENERGY)
    write_png(path, w, h, px)


def gen_fire(path, w, h, phase):
    px = blank(w, h)
    for y in range(h):
        for x in range(w):
            nx, ny = (x - w / 2) / (w / 2), (y - h * 0.55) / (h * 0.55)
            d = math.sqrt(nx * nx + ny * ny * 1.5)
            wob = fbm(x + phase * 40, y + phase * 60, 9, 31 + phase, 3) * 0.42
            v = 1.0 - d + wob - 0.18
            if v > 0:
                if v > 0.55:
                    c = (255, 236, 170)
                elif v > 0.30:
                    c = mix((255, 150, 60), (255, 226, 150), (v - 0.30) / 0.25)
                else:
                    c = mix((190, 60, 40), (255, 150, 60), v / 0.30)
                px[y][x] = (c[0], c[1], c[2], min(255, int(v * 420)))
    write_png(path, w, h, px)


def gen_crack(path, w, h, branches=7, seed=5):
    px = blank(w, h)
    cx, cy = w / 2, h / 2
    for b in range(branches):
        ang = hash2(b, 0, seed) * 2 * math.pi
        x, y = cx, cy
        length = int(min(w, h) * (0.30 + hash2(b, 1, seed) * 0.20))
        for step in range(length):
            ang += (hash2(b, step, seed + 3) - 0.5) * 0.42
            x += math.cos(ang) * 1.6
            y += math.sin(ang) * 1.6
            rad = max(1, int(3.0 * (1 - step / length)))
            ellipse_a(px, w, h, x, y, rad, rad, (16, 12, 10))
    write_png(path, w, h, px)


def gen_water_sheet(path, w=1280, h=64):
    """Animated water surface: 10 frames of 128x64."""
    px = blank(w, h)
    fw = 128
    for f in range(10):
        ph = f / 10 * 2 * math.pi
        for x in range(fw):
            surf = 10 + math.sin(x / 128 * 4 * math.pi + ph) * 5 \
                      + math.sin(x / 128 * 9 * math.pi - ph * 1.7) * 3
            for y in range(h):
                if y >= surf:
                    t = min(1.0, (y - surf) / (h - surf + 1))
                    c = mix((120, 200, 235), WATER, t)
                    a = int(lerp(210, 130, t))
                    px[y][f * fw + x] = (c[0], c[1], c[2], a)
    write_png(path, w, h, px)


def gen_vine(path, w=64, h=646):
    px = blank(w, h)
    for y in range(h):
        sway = math.sin(y / 46) * (w * 0.20)
        cx = w / 2 + sway
        ellipse_a(px, w, h, cx, y + 0.5, 5, 1.2, (74, 130, 74))
        if y % 44 == 0:
            side = 1 if (y // 44) % 2 == 0 else -1
            ellipse_a(px, w, h, cx + side * 11, y, 11, 6, GREEN)
    write_png(path, w, h, px)


def gen_sinking_wall(path, w=128, h=640):
    write_png(path, w, h, rocky_texture(w, h, 13, (66, 60, 78), (32, 28, 40), (98, 92, 118)))


def gen_break_particle(path, w=142, h=142):
    px = blank(w, h)
    for i in range(9):
        ex = w * (0.18 + hash2(i, 0, 41) * 0.64)
        ey = h * (0.18 + hash2(i, 1, 41) * 0.64)
        r = 8 + hash2(i, 2, 41) * 18
        ellipse_a(px, w, h, ex, ey, r, r, mix(ROCK_MID, ROCK_LIGHT, hash2(i, 3, 41)))
    write_png(path, w, h, px)


def gen_sound_icon(path, w=40, h=54):
    px = blank(w, h)
    rrect(px, w, h, 2, int(h * 0.34), int(w * 0.38), int(h * 0.66), 2, (255, 255, 255))
    for y in range(h):
        t = abs(y - h / 2) / (h / 2)
        xx = int(w * 0.38 + (1 - t) * w * 0.30)
        for x in range(int(w * 0.38), min(w, xx)):
            px[y][x] = (255, 255, 255, 255)
    write_png(path, w, h, px)


def gen_link_icon(path, w=51, h=56):
    """A generic 'link' glyph -- deliberately NOT any company's logo."""
    px = blank(w, h)
    ellipse_a(px, w, h, w / 2, h / 2, w * 0.44, h * 0.44, (255, 255, 255))
    ellipse_a(px, w, h, w / 2, h / 2, w * 0.30, h * 0.30, (0, 0, 0, ))
    rrect(px, w, h, int(w * 0.30), int(h * 0.46), int(w * 0.70), int(h * 0.54), 2,
          (255, 255, 255))
    write_png(path, w, h, px)


def gen_projectile(path, w=48, h=48, color=DANGER):
    px = blank(w, h)
    ellipse_a(px, w, h, w / 2, h / 2, w * 0.34, h * 0.34, color)
    ellipse_a(px, w, h, w / 2, h / 2, w * 0.18, h * 0.18, (255, 240, 220))
    write_png(path, w, h, px)


def gen_bomb(path, w=42, h=42):
    px = blank(w, h)
    ellipse_a(px, w, h, w / 2, h * 0.58, w * 0.40, h * 0.40, (52, 52, 60))
    ellipse_a(px, w, h, w * 0.40, h * 0.44, w * 0.10, h * 0.10, (120, 120, 132))
    rrect(px, w, h, int(w * 0.46), 2, int(w * 0.56), int(h * 0.24), 2, (150, 110, 60))
    write_png(path, w, h, px)


def gen_pack(path, w, h, color):
    px = blank(w, h)
    rrect(px, w, h, 2, 2, w - 3, h - 3, max(3, w // 5), color)
    rrect(px, w, h, int(w * 0.22), int(h * 0.14), int(w * 0.78), int(h * 0.42),
          max(2, w // 8), tuple(min(255, int(c * 1.3)) for c in color))
    write_png(path, w, h, px)


def gen_spear(path, w=116, h=12):
    px = blank(w, h)
    rrect(px, w, h, 0, int(h * 0.34), int(w * 0.82), int(h * 0.66), 2, (140, 110, 80))
    for x in range(int(w * 0.80), w):
        t = (x - w * 0.80) / (w * 0.20 + 1)
        span = int((h / 2) * (1 - t))
        for y in range(h // 2 - span, h // 2 + span + 1):
            if 0 <= y < h:
                px[y][x] = (METAL[0], METAL[1], METAL[2], 255)
    write_png(path, w, h, px)


def gen_full_player(path, w=102, h=222):
    px = blank(w, h)
    rrect(px, w, h, 22, 54, w - 22, 150, 16, SUIT)
    rrect(px, w, h, 30, 146, 48, h - 6, 8, SUIT_DARK)
    rrect(px, w, h, w - 48, 146, w - 30, h - 6, 8, SUIT_DARK)
    ellipse_a(px, w, h, w / 2, 36, 30, 32, SUIT)
    ellipse_a(px, w, h, w * 0.56, 32, 20, 15, (30, 40, 52))
    ellipse_a(px, w, h, w / 2, 92, 10, 10, ENERGY)
    write_png(path, w, h, px)


# ── tilesheets (geometry MUST match the .lua map metadata) ──────────

def gen_sheet1(path):
    """main_sheet: one 128x128 tile, no margin."""
    write_png(path, 128, 128, rocky_texture(128, 128, 3))


def gen_sheet2(path):
    write_png(path, 128, 128, rocky_texture(128, 128, 8, (60, 56, 74), (28, 26, 38), (92, 88, 112)))


def gen_3by3(path):
    """3x3 grid of 128px tiles with a 4px margin -> 392x392.

    Tiles are edge/corner variants: the map picks which one to place, so
    each cell needs the correct side to be 'open' (transparent) for seams
    to line up. Index order is row-major from the top-left.
    """
    W = H = 392
    M = 4
    px = blank(W, H)
    rock = rocky_texture(128, 128, 3)
    for ty in range(3):
        for tx in range(3):
            ox, oy = M + tx * 128, M + ty * 128
            for y in range(128):
                for x in range(128):
                    # round off the outward corner of corner tiles
                    keep = True
                    if tx == 0 and x < 6:
                        keep = x >= 6 - int(6 * (1 - abs(y - 64) / 64))
                    if tx == 2 and x > 121:
                        keep = keep and x <= 121 + int(6 * (1 - abs(y - 64) / 64))
                    if keep and oy + y < H and ox + x < W:
                        px[oy + y][ox + x] = rock[y][x]
            # shade the exposed top edge of the top row
            if ty == 0:
                for x in range(128):
                    for y in range(7):
                        if oy + y < H and ox + x < W:
                            c = mix(ROCK_LIGHT, ROCK_MID, y / 7)
                            px[oy + y][ox + x] = (c[0], c[1], c[2], 255)
    write_png(path, W, H, px)


def gen_dark_corners(path):
    """darkCorners: 2x2 grid of 128px tiles with a 4px margin -> 264x264.

    Soft dark vignette wedges the map lays over cave corners.
    """
    W = H = 264
    M = 4
    px = blank(W, H)
    corners = [(0, 0), (1, 0), (0, 1), (1, 1)]
    for idx, (tx, ty) in enumerate(corners):
        ox, oy = M + tx * 128, M + ty * 128
        # which corner is dark
        cx = 0 if tx == 0 else 127
        cy = 0 if ty == 0 else 127
        for y in range(128):
            for x in range(128):
                d = math.sqrt((x - cx) ** 2 + (y - cy) ** 2) / 128.0
                a = max(0.0, 1.0 - d) ** 1.6
                if a > 0.01:
                    px[oy + y][ox + x] = (10, 8, 8, int(a * 235))
    write_png(path, W, H, px)


# ── build everything ────────────────────────────────────────────────

def p(*parts):
    return os.path.join(OUT, *parts)


def main():
    print("generating sprites...")

    # environment
    gen_bg(p("sprites/environment/bg.png"))
    gen_wall(p("sprites/environment/wall.png"), seed=3)
    gen_wall(p("sprites/environment/wall2.png"), seed=8)
    gen_wall(p("sprites/environment/wall_old.png"), seed=3)
    gen_rocky_surface(p("sprites/environment/rockySurface.png"))
    gen_rocky_surface(p("sprites/environment/rockySurface2.png"), tone=0.85)
    gen_rocky_surface(p("sprites/environment/rockySurface_white.png"), tone=1.6)
    gen_sinking_wall(p("sprites/environment/sinkingWall.png"))
    gen_vine(p("sprites/environment/vine.png"))
    gen_water_sheet(p("sprites/environment/waterSheet.png"))
    gen_break_particle(p("sprites/environment/breakParticle.png"))
    gen_crack(p("sprites/environment/crack.png"), 180, 180)
    for i in (1, 2, 3):
        gen_crack(p(f"sprites/environment/cracks/crack{i}.png"), 210, 210,
                  branches=6 + i, seed=5 + i * 7)

    # player (three historical sets; all are loaded somewhere)
    for d in ("newPlayer2", "newPlayer"):
        gen_player_body(p(f"sprites/{d}/body.png"))
        gen_helmet(p(f"sprites/{d}/helmet.png"))
        gen_arm(p(f"sprites/{d}/arm.png"), 37, 84)
        gen_arm(p(f"sprites/{d}/backArm.png"), 84, 35)
        gen_arm(p(f"sprites/{d}/armBlaster.png"), 128, 59, "blaster")
        gen_arm(p(f"sprites/{d}/armRocket.png"), 152, 64, "rocket")
        gen_arm(p(f"sprites/{d}/armSpear.png"), 126, 54, "spear")
        gen_arm(p(f"sprites/{d}/armSpearLoaded.png"), 164, 54, "spear_loaded")
    gen_arm(p("sprites/newPlayer/backArm_old.png"), 71, 29)
    gen_full_player(p("sprites/newPlayer/newPlayer.png"))
    gen_spear(p("sprites/newPlayer/spear.png"))
    gen_pack(p("sprites/newPlayer/jetpack.png"), 50, 72, METAL_DARK)
    gen_pack(p("sprites/newPlayer/aquapack.png"), 50, 72, WATER)

    gen_player_body(p("sprites/player/body.png"), 76, 122)
    gen_helmet(p("sprites/player/helmet.png"), 100, 100)
    gen_arm(p("sprites/player/arm_empty.png"), 38, 38)
    gen_arm(p("sprites/player/arm_blaster.png"), 84, 54, "blaster")
    gen_arm(p("sprites/player/arm_blaster2.png"), 89, 54, "blaster")
    gen_arm(p("sprites/player/armSpear.png"), 218, 30, "spear")
    gen_arm(p("sprites/player/spear.png"), 218, 30, "spear")
    gen_arm(p("sprites/player/rocketLauncher.png"), 174, 46, "rocket")
    gen_arm(p("sprites/player/rocketLauncherAlt.png"), 174, 66, "rocket")
    gen_pack(p("sprites/player/jetpack.png"), 31, 84, METAL_DARK)
    gen_pack(p("sprites/player/aquapack.png"), 31, 84, WATER)
    gen_bomb(p("sprites/player/bomb.png"))

    # enemies
    gen_flyer_body(p("sprites/enemies/flyerBody.png"))
    gen_eye(p("sprites/enemies/flyerEye.png"), 48, 48)
    gen_wing(p("sprites/enemies/flyerWing1.png"), 140, 34)
    gen_wing(p("sprites/enemies/flyerWing2.png"), 151, 28)
    gen_spike_body(p("sprites/enemies/spikeBody.png"))
    gen_projectile(p("sprites/enemies/spikeProj.png"), 48, 48, (170, 160, 200))
    gen_starfish(p("sprites/enemies/starfish.png"))
    gen_bubble(p("sprites/enemies/evilBubble.png"))
    gen_boss_body(p("sprites/enemies/bossBody.png"))
    gen_eye(p("sprites/enemies/bigBossEye.png"), 242, 242, DANGER_DARK)
    gen_egg(p("sprites/enemies/egg.png"))

    # items
    gen_pickup_item(p("sprites/items/healthPickup.png"), 510, 510, DANGER, "cross")
    gen_pickup_item(p("sprites/items/itemPickup.png"), 82, 82, GOLD)
    gen_pickup_item(p("sprites/items/pickup_back.png"), 324, 324, ENERGY_DIM)
    gen_weapon_pickup(p("sprites/items/blaster.png"), 508, 500, METAL, ENERGY)
    gen_weapon_pickup(p("sprites/items/rocketLauncher.png"), 972, 410, METAL_DARK, DANGER)
    gen_weapon_pickup(p("sprites/items/spearGun.png"), 1098, 380, METAL, GREEN)
    gen_pack(p("sprites/items/aquaPack.png"), 486, 530, WATER)

    # fire
    for i in range(1, 6):
        gen_fire(p(f"sprites/fire/fire_{i}.png"), 32, 32, i)

    # ui
    gen_sound_icon(p("sprites/ui/sound.png"))
    gen_link_icon(p("sprites/ui/github.png"))

    print("generating tilesheets...")
    gen_sheet1(p("maps/tilesheets/sheet1.png"))
    gen_sheet2(p("maps/tilesheets/sheet2.png"))
    gen_3by3(p("maps/tilesheets/3by3_1.png"))
    gen_dark_corners(p("maps/tilesheets/darkCorners_sheet.png"))

    print("done.")


if __name__ == "__main__":
    main()
