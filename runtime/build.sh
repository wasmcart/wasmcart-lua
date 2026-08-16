#!/bin/bash
# Build the wasmcart Lua engine -> ../build/engine.wasm
# Needs: emcc, curl, and the `wasmcart` npm package (npm install) for
# include/wc_cart.h + wc_pcm_mixer.h and the packer. $WASMCART_REPO overrides
# with a checkout; the sibling-checkout default remains the last fallback.
set -e
cd "$(dirname "$0")"
if [ -z "${WASMCART_REPO:-}" ]; then
  WASMCART_REPO="$(node -e "console.log(require('path').dirname(require.resolve('wasmcart')))" 2>/dev/null || true)"
fi
WASMCART_REPO="${WASMCART_REPO:-../../wasmcart}"

# WARN when the resolved headers differ from a sibling checkout.
#
# The npm package is the right default -- building this engine should not
# require a second repo -- but when someone IS developing the two together,
# an npm copy that predates a header change fails in the least helpful way
# available: the engine compiles against the old declarations and a GL enum
# the new code needs is simply absent, so the error names a symbol rather
# than the stale file that lacks it. Say which header is in use instead.
if [ -f ../../wasmcart/include/wasmcart.h ] && \
   [ -f "$WASMCART_REPO/include/wasmcart.h" ] && \
   ! cmp -s ../../wasmcart/include/wasmcart.h "$WASMCART_REPO/include/wasmcart.h"; then
  echo "WARNING: building against $WASMCART_REPO/include/wasmcart.h, which"
  echo "         DIFFERS from the sibling checkout ../../wasmcart/include/."
  echo "         If you are developing both, re-run with:"
  echo "           WASMCART_REPO=\$(cd ../../wasmcart && pwd) ./build.sh"
fi
LUA_VERSION=5.4.7
# Pinned by SHA, not tag. physics.c calls b2Body_SetMotionLocks, which exists
# in NO released Box2D tag (v3.1.1 is the newest); the old v3.2.0 pin here
# resolved to nothing at all upstream, so a fresh clone could not build
# physics -- it only worked where a stale vendor/libbox2d.a happened to sit.
BOX2D_SHA=56edae79f2949d86142b03450d5d60f63bcf5a6f
# Box3D (Erin Catto's 3D engine, portable C17). Same SHA box3d-wasm pins, so
# the wasm and native builds of both projects agree on one revision.
BOX3D_SHA=29bf523ce7bc4590aba9f17c9db791cdc5c4397e
RUNTIME_DIR="$(pwd)"

# ── fetch + build the Lua VM (pinned, fetched not vendored) ──────────
if [ ! -f vendor/liblua54.a ]; then
  mkdir -p vendor
  if [ ! -d vendor/lua ]; then
    curl -sL "https://www.lua.org/ftp/lua-${LUA_VERSION}.tar.gz" -o vendor/lua.tar.gz
    tar xzf vendor/lua.tar.gz -C vendor
    mv "vendor/lua-${LUA_VERSION}" vendor/lua
    rm vendor/lua.tar.gz
  fi
  # core + the libraries a game can safely have. NO liolib/loslib/loadlib:
  # the cart has no filesystem and no dynamic loading, by design.
  CORE="lapi.c lcode.c lctype.c ldebug.c ldo.c ldump.c lfunc.c lgc.c llex.c \
        lmem.c lobject.c lopcodes.c lparser.c lstate.c lstring.c ltable.c \
        ltm.c lundump.c lvm.c lzio.c lauxlib.c lbaselib.c lcorolib.c \
        ldblib.c lmathlib.c lstrlib.c ltablib.c lutf8lib.c"
  # LUA_CART_NOFILES compiles out luaL_loadfilex/luaL_dofile (see the patch
  # below). Those are the ONLY stdio users left once io/os/package are gone,
  # and they are dead code in a cart: keeping them would drag WASI imports
  # (fd_write/fd_seek/fd_read/clock_time_get) into the engine and break hosts
  # that provide only the wasmcart `env` module.
  # patch-lua.py is idempotent per file (it checks for its own marker)
  python3 ../patch-lua.py vendor/lua/src/lauxlib.c
  python3 ../patch-lua.py vendor/lua/src/lmathlib.c
  # -include cartconf.h lands before lauxlib.h's `#if !defined(lua_writestring)`
  # guards, so Lua's stdout/stderr hooks resolve to wc_log instead of stdio.
  ( cd vendor/lua/src && \
    emcc -O2 -c $CORE -DLUA_CART_NOFILES -sSUPPORT_LONGJMP=wasm \
      -I "$RUNTIME_DIR" -include "$RUNTIME_DIR/cartconf.h" && \
    emar rcs ../../liblua54.a *.o && rm -f *.o )
fi


# ── stub the physics timers for wasm ────────────────────────────────
# Box2D and Box3D time their own solver stages with clock_gettime, which
# emscripten lowers to the WASI import clock_time_get. A wasmcart cart must
# import ONLY the `env` module -- a host that provides just that (and the
# engine's own test harness, which fails a cart that touches WASI) breaks
# otherwise. The numbers are profiling telemetry no cart reads, so under
# wasm the clock becomes a counter: the profile fields still exist, they
# just always read zero. Idempotent via the marker.
patch_physics_timer() {   # $1 = path to a box2d/box3d timer.c
  python3 - "$1" <<'PY'
import sys, pathlib
p = pathlib.Path(sys.argv[1])
src = p.read_text()
MARK = "/* wasmcart: no-WASI clock */"
if MARK in src:
    sys.exit(0)
old = """	struct timespec ts;
	clock_gettime( CLOCK_MONOTONIC, &ts );
	return ts.tv_sec * 1000000000LL + ts.tv_nsec;"""
new = MARK + """
#if defined( __EMSCRIPTEN__ )
	/* No host clock: a wasmcart cart imports only `env`. Profiling reads 0. */
	return 0;
#else
	struct timespec ts;
	clock_gettime( CLOCK_MONOTONIC, &ts );
	return ts.tv_sec * 1000000000LL + ts.tv_nsec;
#endif"""
if old not in src:
    sys.stderr.write("patch_physics_timer: pattern not found in %s\n" % p)
    sys.exit(1)
p.write_text(src.replace(old, new, 1))
print("patched timer:", p)
PY
}

# ── fetch + build Box2D v3 (SIMD) ────────────────────────────────────
# Box2D 3.x is pure C with an opaque-handle API and first-class wasm SIMD
# (B2_CPU_WASM selects its SSE2 path; -msse2 lets clang lower those
# intrinsics to wasm128). That makes it a far better embedding target than
# the C++ v2 that LOVE wraps.
if [ ! -f vendor/libbox2d.a ]; then
  if [ ! -d vendor/box2d ]; then
    git clone --filter=blob:none https://github.com/erincatto/box2d.git vendor/box2d
    ( cd vendor/box2d && git checkout -q "$BOX2D_SHA" )
    rm -rf vendor/box2d/.git
  fi
  patch_physics_timer vendor/box2d/src/timer.c
  ( cd vendor/box2d/src && \
    emcc -O2 -msimd128 -msse2 -c *.c -I../include -I. && \
    emar rcs ../../libbox2d.a *.o && rm -f *.o )
fi

# ── fetch + build Box3D (SIMD) ───────────────────────────────────────
# Portable C17, needs only libc + libm. Bound as the Lua global `b3` by
# physics3d.c. -msimd128 gives it wasm SIMD the same way Box2D gets it.
if [ ! -f vendor/libbox3d.a ]; then
  if [ ! -d vendor/box3d ]; then
    git clone --filter=blob:none https://github.com/erincatto/box3d.git vendor/box3d
    ( cd vendor/box3d && git checkout -q "$BOX3D_SHA" )
    rm -rf vendor/box3d/.git
  fi
  patch_physics_timer vendor/box3d/src/timer.c
  ( cd vendor/box3d/src && \
    emcc -O2 -msimd128 -msse2 -c *.c -I../include -I. && \
    emar rcs ../../libbox3d.a *.o && rm -f *.o )
fi

# ── fetch + build Opus (libopus + libogg + opusfile) ────────────────
# MP3 and FLAC are single-header (dr_mp3.h / dr_flac.h, committed next to
# stb_vorbis.c) and need no fetch. Opus is the one codec with real
# dependencies: the decoder itself, the Ogg container it always ships in, and
# opusfile to tie them together.
#
# OPTIONAL, and off unless the sources are present or WCL_OPUS=1 asks for the
# fetch: it is ~140 C files against two single headers, and no LOVE game needs
# Opus (LOVE has no Opus decoder at all -- this is a wasmcart-lua extra). The
# engine builds and every other codec works without it; sound_load() then logs
# an explicit "built without Opus" line rather than failing mysteriously.
OPUS_TAG=v1.5.2
OGG_TAG=v1.3.5
OPUSFILE_TAG=v0.12
OPUS_FLAGS=""
if [ "${WCL_OPUS:-0}" = "1" ] && [ ! -d vendor/opus ]; then
  git clone --filter=blob:none --depth 1 -b "$OPUS_TAG" \
    https://github.com/xiph/opus.git vendor/opus && rm -rf vendor/opus/.git
  git clone --filter=blob:none --depth 1 -b "$OGG_TAG" \
    https://github.com/xiph/ogg.git vendor/ogg && rm -rf vendor/ogg/.git
  git clone --filter=blob:none --depth 1 -b "$OPUSFILE_TAG" \
    https://github.com/xiph/opusfile.git vendor/opusfile && rm -rf vendor/opusfile/.git
  # libogg needs its config_types.h generated; the sizes are fixed under wasm32.
  sed -e 's/@INCLUDE_INTTYPES_H@/1/; s/@INCLUDE_STDINT_H@/1/; s/@INCLUDE_SYS_TYPES_H@/1/' \
      -e 's/@SIZE16@/int16_t/; s/@USIZE16@/uint16_t/' \
      -e 's/@SIZE32@/int32_t/; s/@USIZE32@/uint32_t/' \
      -e 's/@SIZE64@/int64_t/; s/@USIZE64@/uint64_t/' \
      vendor/ogg/include/ogg/config_types.h.in > vendor/ogg/include/ogg/config_types.h
fi
if [ -d vendor/opus ] && [ ! -f vendor/libopus.a ]; then
  # OPUS_BUILD + VAR_ARRAYS + FLOATING_POINT replace opus's generated config.h,
  # so no autotools run is needed under emcc. OP_ENABLE_HTTP=0 keeps opusfile
  # to memory decoding: a cart has no sockets, and the http path would drag in
  # imports a wasmcart host does not provide.
  OPUS_INC="-Ivendor/opus/celt -Ivendor/opus/silk -Ivendor/opus/silk/float \
            -Ivendor/opus/include -Ivendor/opus -Ivendor/ogg/include \
            -Ivendor/opusfile/include -Ivendor/opusfile/src"
  OPUS_SRC="$(ls vendor/opus/src/*.c vendor/opus/celt/*.c vendor/opus/silk/*.c \
                 vendor/opus/silk/float/*.c 2>/dev/null \
              | grep -v -E 'demo|_test|opus_compare|repacketizer_demo|/tests?/')"
  OPUS_SRC="$OPUS_SRC vendor/ogg/src/bitwise.c vendor/ogg/src/framing.c"
  OPUS_SRC="$OPUS_SRC vendor/opusfile/src/opusfile.c vendor/opusfile/src/info.c \
            vendor/opusfile/src/internal.c vendor/opusfile/src/stream.c"
  mkdir -p vendor/opus-obj && ( cd vendor/opus-obj && rm -f ./*.o )
  ( cd vendor/opus-obj && emcc -O2 -msimd128 -msse2 -c $(for f in $OPUS_SRC; do echo "../../$f"; done) \
      -DOPUS_BUILD=1 -DVAR_ARRAYS=1 -DFLOATING_POINT=1 \
      -DOP_ENABLE_HTTP=0 -DOP_FIXED_POINT=0 -w \
      $(echo "$OPUS_INC" | sed 's|-I|-I../../|g') && \
    emar rcs ../libopus.a ./*.o && rm -f ./*.o )
fi
if [ -f vendor/libopus.a ]; then
  # opusfile.h includes <opus_multistream.h>, so opus's own include dir is
  # needed by decoders.c and not just by the library build.
  OPUS_FLAGS="-DWCL_ENABLE_OPUS -Ivendor/opusfile/include -Ivendor/ogg/include \
              -Ivendor/opus/include vendor/libopus.a"
  echo "Opus: ENABLED"
else
  echo "Opus: disabled (WCL_OPUS=1 ./build.sh to fetch and enable)"
fi

# ── embed the Lua API surface (games ship ONLY their own Lua) ────────
python3 - <<'PY'
# The ffi shim is appended to the prelude as a PRELOADED MODULE rather than
# embedded separately: it must be reachable through require("ffi") (which is
# how every caller asks for it) and it needs nothing the prelude does not
# already have. One embedded blob, one load, one place to look.
prelude = open('prelude.lua', 'rb').read()
ffi_src = open('ffi.lua', 'rb').read()

shim = (b"\n-- ---- embedded module: ffi (see runtime/ffi.lua) ----\n"
        b"package.loaded['ffi'] = (function()\n"
        + ffi_src +
        b"\nend)()\n")
data = prelude + shim

with open('prelude.inc', 'w') as f:
    f.write('static const unsigned char WCL_PRELUDE[] = {')
    f.write(','.join(str(b) for b in data))
    f.write('};\n')
    f.write(f'static const unsigned int WCL_PRELUDE_LEN = {len(data)};\n')
PY

mkdir -p ../build

# -sSUPPORT_LONGJMP=wasm is REQUIRED: Lua's error handling is setjmp/longjmp
# and the default JS-trampoline form breaks under import-stubbing hosts.
# No LTO: it crashes the wasm-sjlj path.
# ── the engine ──────────────────────────────────────────────────────
# GL2D is the DEFAULT. Target hardware has a GPU, and the measured wins are
# large (Cavern 49.5x, render targets 113x). The software rasterizer is still
# built into this same binary and still renders every frame that GL2D cannot
# (circles, polygons, additive, the error screen) via the sticky whole-frame
# fallback -- it is the reference implementation, not a legacy path.
build_engine() {   # $1 = output, $2... = extra flags
  local out="$1"; shift
  emcc runtime.c vorbis.c decoders.c cartconf.c physics.c physics3d.c wc_taskpool.c \
    render2d_gl.c render3d_gl.c \
    vendor/liblua54.a vendor/libbox2d.a vendor/libbox3d.a \
    -O2 -msimd128 -msse2 -DWC_USE_NET_PEER -DWC_PHYSICS_SIMD='"wasm-simd128"' \
    $OPUS_FLAGS "$@" \
    -I vendor/lua/src -I vendor/box2d/include -I vendor/box3d/include \
    -I "$WASMCART_REPO/include" -I . \
    -s STANDALONE_WASM=1 --no-entry -sSUPPORT_LONGJMP=wasm \
    -s EXPORTED_FUNCTIONS='["_wc_init","_wc_render","_wc_get_info","_wc_debug_state","_wc_set_seed","_wc_peer_on_connect","_wc_peer_on_message","_wc_peer_on_disconnect","_wc_peer_on_error"]' \
    -s ERROR_ON_UNDEFINED_SYMBOLS=0 \
    -s ALLOW_MEMORY_GROWTH=1 -s INITIAL_MEMORY=67108864 -s STACK_SIZE=4194304 \
    -o "$out"
  echo "built $out ($(wc -c < "$out") bytes)"
}

build_engine ../build/engine.wasm -DWCL_USE_GL -DWCL_ENABLE_GL2D

# ── the CPU-only comparator ─────────────────────────────────────────
# Imports nothing from the `gl` module, so it is what the GL build is
# measured and diffed against (tools/gl2d-compare.mjs, test/render-hash.js).
# A TEST ARTIFACT ONLY. Carts ship engine.wasm: wasmcart targets a GPU, and
# this build exists to give the differ an oracle, never to ship to a player
# as a no-GL fallback.
build_engine ../build/engine-cpu.wasm

# the template ships the engine so `run.sh` works with no build step
cp ../build/engine.wasm ../template/main.wasm

# ── pack the release set ────────────────────────────────────────────
# THE CARTS ATTACHED TO A GITHUB RELEASE ARE BUILT HERE, not by hand.
#
# This used to pack ping alone, and the other seven carts in build/ were
# whatever some earlier run had left there. By v0.5.0 they were three weeks
# stale -- still carrying the v0.1.0 engine -- and attaching them to a release
# would have shipped players an engine with none of the release's work in it.
# Packing the whole set here is what stops that drifting again: the carts are
# always as new as the engine sitting next to them.
#
# Each entry is "name:appdir:WxH". The resolution is the cart's OWN, declared
# in the manifest so a host sizes its window -- and a self-provisioned GL
# context -- before the cart runs. Getting it wrong is not cosmetic: a cart
# handed a smaller context than it draws into renders in a corner of it.
# cavern is 1152x768 from its conf.lua; everything else is the 1280x720
# default. A cart with a conf.lua still overrides this at boot, but the
# manifest is what the host has to go on BEFORE boot.
RELEASE_CARTS="
ping:../examples/ping/app:1280x720
breakout:../examples/breakout/app:1280x720
kitchen-sink:../examples/kitchen-sink/app:1280x720
particles:../examples/particles/app:1280x720
platformer:../examples/platformer/app:1280x720
shmup:../examples/shmup/app:1280x720
template:../template/app:1280x720
cavern:../ports/cavern/app:1152x768
"

packed=0
for entry in $RELEASE_CARTS; do
  name="${entry%%:*}"; rest="${entry#*:}"
  appdir="${rest%%:*}"; res="${rest##*:}"
  w="${res%x*}"; h="${res#*x}"
  # Validate the WxH here. wasmcart-pack takes --width/--height as numbers and
  # simply OMITS manifest.width/height when they do not parse -- it exits 0 and
  # writes a cart that looks fine. A cart with no declared resolution is handed
  # whatever context size the host guesses, which is the "renders into a corner"
  # failure the packer's own comment warns about. Catch the typo here, where it
  # is a one-line fix, rather than in a player's window.
  case "$w$h" in
    *[!0-9]*|"") echo "bad resolution '$res' for $name in RELEASE_CARTS" >&2; exit 1 ;;
  esac
  [ -d "$appdir" ] || { echo "skipped $name (no $appdir)"; continue; }
  if node "$WASMCART_REPO/bin/wasmcart-pack.js" \
       --wasm ../build/engine.wasm --assets "$appdir" \
       --name "$name" --width "$w" --height "$h" \
       -o "../build/$name.wasc" > /dev/null; then
    packed=$((packed + 1))
  else
    echo "FAILED to pack $name" >&2
    exit 1
  fi
done
echo "packed $packed release carts into ../build/ (see RELEASE_CARTS)"
