#!/usr/bin/env python3
"""
make-audio.py - synthesize the freely-licensed audio for the Cavern port.

Same reasoning as make-assets.py: the original sounds and music are
CC BY-NC-ND and cannot ship. Everything here is synthesized from scratch
(oscillators, noise, envelopes) so the output is our own work under MIT.

Format note: the original music is .ogg. Writing a real Vorbis encoder by
hand is out of scope, so everything here is emitted as WAV. The game's
source still asks for "music/menu.ogg" -- and stays UNMODIFIED, because the
engine's sound loader falls back to the sibling extension when the exact
path is missing. Codec choice is a packaging decision, not a gameplay one.

Sizes are deliberately modest: the original 20MB of audio becomes ~2MB,
which also makes the cart far more practical to ship.
"""
import math
import os
import struct
import wave

OUT = os.path.join(os.path.dirname(__file__), "..", "app")
RATE = 22050          # plenty for SFX; keeps the cart small


def write_wav(path, samples, rate=RATE):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    frames = bytearray()
    for s in samples:
        v = max(-1.0, min(1.0, s))
        frames += struct.pack("<h", int(v * 32000))
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(frames))


# ── deterministic noise ─────────────────────────────────────────────
_state = 12345


def rnd():
    """xorshift32 in [-1,1) - deterministic, no RNG import."""
    global _state
    x = _state
    x ^= (x << 13) & 0xFFFFFFFF
    x ^= x >> 17
    x ^= (x << 5) & 0xFFFFFFFF
    _state = x & 0xFFFFFFFF
    return (_state / 0x7FFFFFFF) - 1.0


def reseed(n):
    global _state
    _state = (n * 2654435761) & 0xFFFFFFFF or 1


# ── envelopes + oscillators ─────────────────────────────────────────

def env_ad(i, n, attack=0.01, decay=None, curve=2.0):
    a = int(n * attack)
    if i < a:
        return i / max(1, a)
    t = (i - a) / max(1, n - a)
    return max(0.0, (1.0 - t) ** curve)


def sine(f, i, rate=RATE):
    return math.sin(2 * math.pi * f * i / rate)


def square(f, i, rate=RATE):
    return 1.0 if (f * i / rate) % 1.0 < 0.5 else -1.0


def saw(f, i, rate=RATE):
    return 2.0 * ((f * i / rate) % 1.0) - 1.0


# ── SFX ─────────────────────────────────────────────────────────────

def sfx_laser(path, f0=1400, f1=300, dur=0.22, seed=1):
    reseed(seed)
    n = int(RATE * dur)
    out = []
    for i in range(n):
        t = i / n
        f = f0 + (f1 - f0) * t
        v = (square(f, i) * 0.5 + saw(f * 1.5, i) * 0.3 + rnd() * 0.06)
        out.append(v * env_ad(i, n, 0.005, curve=2.4) * 0.55)
    write_wav(path, out)


def sfx_hurt(path, f0=420, dur=0.30, seed=2):
    reseed(seed)
    n = int(RATE * dur)
    out = []
    for i in range(n):
        t = i / n
        f = f0 * (1.0 - 0.45 * t)
        v = saw(f, i) * 0.5 + rnd() * 0.35
        out.append(v * env_ad(i, n, 0.004, curve=2.0) * 0.6)
    write_wav(path, out)


def sfx_explosion(path, dur=0.85, seed=3, boom=70):
    reseed(seed)
    n = int(RATE * dur)
    out = []
    lp = 0.0
    for i in range(n):
        t = i / n
        noise = rnd()
        lp += (noise - lp) * (0.30 - 0.22 * t)        # darkens as it decays
        rumble = sine(boom * (1 - 0.5 * t), i) * 0.55
        v = lp * 0.9 + rumble
        out.append(v * env_ad(i, n, 0.002, curve=1.7) * 0.75)
    write_wav(path, out)


def sfx_blip(path, f=880, dur=0.06, seed=4):
    reseed(seed)
    n = int(RATE * dur)
    out = [square(f, i) * env_ad(i, n, 0.01, curve=2.5) * 0.35 for i in range(n)]
    write_wav(path, out)


def sfx_pickup(path, dur=0.55, seed=5):
    """Rising arpeggio - the 'you got it' sound."""
    reseed(seed)
    n = int(RATE * dur)
    notes = [523.25, 659.25, 783.99, 1046.50]
    out = []
    for i in range(n):
        t = i / n
        idx = min(len(notes) - 1, int(t * len(notes)))
        f = notes[idx]
        seg = n // len(notes)
        local = i % seg
        v = (sine(f, i) * 0.6 + sine(f * 2, i) * 0.2 + square(f, i) * 0.1)
        out.append(v * env_ad(local, seg, 0.02, curve=2.0) * 0.45)
    write_wav(path, out)


def sfx_splash(path, dur=0.5, seed=6):
    reseed(seed)
    n = int(RATE * dur)
    out = []
    lp = 0.0
    for i in range(n):
        t = i / n
        lp += (rnd() - lp) * 0.5
        v = lp * (1.0 - t * 0.4)
        # a little pitch-swept tone gives it "water" character
        v += sine(300 + 500 * (1 - t), i) * 0.18 * (1 - t)
        out.append(v * env_ad(i, n, 0.01, curve=1.6) * 0.5)
    write_wav(path, out)


def sfx_click(path, dur=0.05, seed=7):
    reseed(seed)
    n = int(RATE * dur)
    out = [(rnd() * 0.5 + square(1200, i) * 0.5) * env_ad(i, n, 0.002, curve=4.0) * 0.3
           for i in range(n)]
    write_wav(path, out)


def sfx_text(path, dur=0.03, seed=8):
    reseed(seed)
    n = int(RATE * dur)
    out = [square(1500, i) * env_ad(i, n, 0.005, curve=3.0) * 0.16 for i in range(n)]
    write_wav(path, out)


def sfx_spikes(path, dur=0.35, seed=9):
    reseed(seed)
    n = int(RATE * dur)
    out = []
    for i in range(n):
        t = i / n
        f = 200 + 900 * (1 - t) ** 2
        out.append((saw(f, i) * 0.45 + rnd() * 0.25) * env_ad(i, n, 0.003, curve=2.2) * 0.5)
    write_wav(path, out)


def sfx_boss_roar(path, dur=1.6, seed=10):
    reseed(seed)
    n = int(RATE * dur)
    out = []
    lp = 0.0
    for i in range(n):
        t = i / n
        growl = sine(58 + 14 * math.sin(t * 18), i) * 0.6
        lp += (rnd() - lp) * 0.15
        v = growl + lp * 0.35 + sine(120 + 30 * math.sin(t * 9), i) * 0.2
        env = min(1.0, t * 6) * (1.0 - t) ** 1.2
        out.append(v * env * 0.8)
    write_wav(path, out)


def sfx_boss_die(path, dur=2.4, seed=11):
    reseed(seed)
    n = int(RATE * dur)
    out = []
    lp = 0.0
    for i in range(n):
        t = i / n
        lp += (rnd() - lp) * (0.35 - 0.28 * t)
        rumble = sine(90 * (1 - 0.7 * t), i) * 0.5
        wail = sine(300 * (1 - 0.8 * t), i) * 0.25 * (1 - t)
        out.append((lp + rumble + wail) * (1.0 - t) ** 1.3 * 0.8)
    write_wav(path, out)


def sfx_starfish(path, dur=0.25, seed=12):
    reseed(seed)
    n = int(RATE * dur)
    out = []
    for i in range(n):
        t = i / n
        f = 700 + 400 * math.sin(t * 22)
        out.append(sine(f, i) * env_ad(i, n, 0.01, curve=2.5) * 0.4)
    write_wav(path, out)


# ── music ───────────────────────────────────────────────────────────

SCALE_MINOR = [0, 2, 3, 5, 7, 8, 10]      # natural minor
SCALE_DORIAN = [0, 2, 3, 5, 7, 9, 10]


def note_hz(semitone, base=220.0):
    return base * (2 ** (semitone / 12.0))


MUSIC_RATE = 11025    # pads + slow leads: half rate is inaudible here and
                      # halves the cart's largest asset class

def music(path, dur, root=220.0, scale=SCALE_MINOR, bpm=88, seed=100,
          pad=True, lead=True, drums=False, bright=0.0):
    """A short loopable ambient/melodic bed.

    Deliberately simple: a slow chord pad, an optional sparse lead picked
    from the scale by deterministic hash, and optional soft percussion.
    """
    reseed(seed)
    rate = MUSIC_RATE
    n = int(rate * dur)
    beat = rate * 60.0 / bpm
    out = [0.0] * n

    # chord pad: root - VI - III - VII, two bars each
    chords = [0, -4, 3, -2]
    bar = beat * 4
    for i in range(n):
        ci = int((i / (bar * 2)) % len(chords))
        r = note_hz(chords[ci], root)
        if pad:
            v = 0.0
            for k, mul in enumerate((1.0, 1.5, 2.0)):
                v += sine(r * mul, i, rate) * (0.16 / (k + 1))
            # slow tremolo keeps it from feeling static
            v *= 0.85 + 0.15 * math.sin(i / rate * 0.7)
            out[i] += v

    if lead:
        step = int(beat / 2)
        for s in range(n // step):
            deg = scale[int(abs(hash((seed, s))) % len(scale))]
            oct_ = 12 if (hash((seed, s, 1)) % 3 == 0) else 0
            f = note_hz(deg + oct_ + 12, root)
            amp = 0.16 + 0.06 * bright
            for j in range(step):
                i = s * step + j
                if i >= n:
                    break
                e = env_ad(j, step, 0.05, curve=2.2)
                out[i] += (sine(f, i, rate) * 0.7 + square(f, i, rate) * 0.15) * e * amp

    if drums:
        step = int(beat)
        for s in range(n // step):
            for j in range(min(step, int(rate * 0.12))):
                i = s * step + j
                if i >= n:
                    break
                e = (1.0 - j / (rate * 0.12)) ** 2
                out[i] += sine(70 * (1 - 0.4 * j / (rate * 0.12)), j, rate) * e * 0.35

    # gentle fade at both ends so the loop does not click
    fade = int(rate * 0.25)
    for i in range(fade):
        out[i] *= i / fade
        out[n - 1 - i] *= i / fade

    write_wav(path, [v * 0.8 for v in out], rate)


def p(*parts):
    return os.path.join(OUT, *parts)


def main():
    print("generating sfx...")
    sfx_pickup(p("sounds/itemGet.wav"))
    sfx_laser(p("sounds/laser.wav"))
    sfx_blip(p("sounds/blip.wav"))

    sfx_click(p("sounds/ui/click.wav"))
    sfx_text(p("sounds/ui/text.wav"))

    sfx_laser(p("sounds/player/laser1.wav"), 1500, 320, 0.20, seed=21)
    sfx_laser(p("sounds/player/laser2.wav"), 1300, 280, 0.22, seed=22)
    sfx_laser(p("sounds/player/laser3.wav"), 1700, 400, 0.18, seed=23)
    sfx_laser(p("sounds/player/spear.wav"), 900, 250, 0.26, seed=24)
    sfx_explosion(p("sounds/player/bombShot.wav"), 0.35, seed=25, boom=140)
    sfx_explosion(p("sounds/player/explosion.wav"), 0.9, seed=26)
    sfx_explosion(p("sounds/player/explosion2.wav"), 0.9, seed=27, boom=58)
    sfx_hurt(p("sounds/player/playerHurt.wav"), 380, 0.32, seed=28)
    sfx_splash(p("sounds/player/splash.wav"))

    sfx_hurt(p("sounds/enemies/enemyHurt.wav"), 520, 0.24, seed=31)
    sfx_hurt(p("sounds/enemies/enemyHurt_2.wav"), 470, 0.26, seed=32)
    sfx_spikes(p("sounds/enemies/spikes.wav"))
    sfx_starfish(p("sounds/enemies/starfish.wav"))
    sfx_starfish(p("sounds/enemies/starfish-old1.wav"), 0.3, seed=33)
    sfx_boss_roar(p("sounds/enemies/helloBoss.wav"))
    sfx_laser(p("sounds/enemies/bossLaser.wav"), 700, 160, 0.40, seed=34)
    sfx_boss_die(p("sounds/enemies/bossDie.wav"))
    sfx_boss_die(p("sounds/enemies/bossDie_old.wav"), 2.0, seed=35)
    sfx_explosion(p("sounds/enemies/bossExplode.wav"), 1.3, seed=36, boom=52)

    print("generating music...")
    music(p("music/menu.wav"),   24.0, 220.0, SCALE_MINOR,  76, seed=201, drums=False)
    music(p("music/cavern.wav"), 32.0, 196.0, SCALE_MINOR,  84, seed=202, drums=True)
    music(p("music/danger.wav"), 24.0, 174.6, SCALE_MINOR, 116, seed=203, drums=True, bright=1.0)
    music(p("music/boss.wav"),   28.0, 164.8, SCALE_MINOR, 132, seed=204, drums=True, bright=1.0)
    music(p("music/intro.wav"),  12.0, 246.9, SCALE_DORIAN, 66, seed=205, lead=False)
    music(p("music/ending.wav"), 24.0, 261.6, SCALE_DORIAN, 72, seed=206, bright=0.5)

    print("done.")


if __name__ == "__main__":
    main()
