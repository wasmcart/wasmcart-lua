#!/bin/bash
# Build the wasmcart Lua engine -> ../build/engine.wasm
# Needs: emcc, curl, a wasmcart checkout for include/wc_cart.h + wc_pcm_mixer.h
# (WASMCART_REPO, default sibling).
set -e
cd "$(dirname "$0")"
WASMCART_REPO="${WASMCART_REPO:-../../wasmcart}"
LUA_VERSION=5.4.7
BOX2D_TAG=v3.2.0
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


# ── fetch + build Box2D v3 (SIMD) ────────────────────────────────────
# Box2D 3.x is pure C with an opaque-handle API and first-class wasm SIMD
# (B2_CPU_WASM selects its SSE2 path; -msse2 lets clang lower those
# intrinsics to wasm128). That makes it a far better embedding target than
# the C++ v2 that LOVE wraps.
if [ ! -f vendor/libbox2d.a ]; then
  if [ ! -d vendor/box2d ]; then
    git clone --depth 1 --branch "$BOX2D_TAG" https://github.com/erincatto/box2d.git vendor/box2d
    rm -rf vendor/box2d/.git
  fi
  ( cd vendor/box2d/src && \
    emcc -O2 -msimd128 -msse2 -c *.c -I../include -I. && \
    emar rcs ../../libbox2d.a *.o && rm -f *.o )
fi

# ── embed the Lua API surface (games ship ONLY their own Lua) ────────
python3 - <<'PY'
data = open('prelude.lua', 'rb').read()
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
  emcc runtime.c vorbis.c cartconf.c physics.c render2d_gl.c \
    vendor/liblua54.a vendor/libbox2d.a \
    -O2 -msimd128 -msse2 -DWC_USE_NET_PEER "$@" \
    -I vendor/lua/src -I vendor/box2d/include -I "$WASMCART_REPO/include" -I . \
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
# Also the artifact to reach for on a host with no GL at all.
build_engine ../build/engine-cpu.wasm

# the template ships the engine so `run.sh` works with no build step
cp ../build/engine.wasm ../template/main.wasm

# pack the flagship example as a ready-to-run demo cart
if [ -d ../examples/ping/app ]; then
  node "$WASMCART_REPO/bin/wasmcart-pack.js" \
    --wasm ../build/engine.wasm --assets ../examples/ping/app \
    --name ping --width 1280 --height 720 \
    -o ../build/ping.wasc > /dev/null
  echo "packed ../build/ping.wasc"
fi
