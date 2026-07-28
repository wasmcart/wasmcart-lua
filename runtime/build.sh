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
emcc runtime.c vorbis.c cartconf.c physics.c \
  vendor/liblua54.a vendor/libbox2d.a \
  -O2 -msimd128 -msse2 \
  -I vendor/lua/src -I vendor/box2d/include -I "$WASMCART_REPO/include" -I . \
  -s STANDALONE_WASM=1 --no-entry -sSUPPORT_LONGJMP=wasm \
  -s EXPORTED_FUNCTIONS='["_wc_init","_wc_render","_wc_get_info","_wc_debug_state","_wc_set_seed"]' \
  -s ERROR_ON_UNDEFINED_SYMBOLS=0 \
  -s ALLOW_MEMORY_GROWTH=1 -s INITIAL_MEMORY=67108864 -s STACK_SIZE=4194304 \
  -o ../build/engine.wasm
echo "built ../build/engine.wasm ($(wc -c < ../build/engine.wasm) bytes)"

# the template ships the engine so `run.sh` works with no build step
cp ../build/engine.wasm ../template/main.wasm

# pack the flagship example as a ready-to-run demo cart
if [ -d ../examples/pong/app ]; then
  node "$WASMCART_REPO/bin/wasmcart-pack.js" \
    --wasm ../build/engine.wasm --assets ../examples/pong/app \
    --name pong -o ../build/pong.wasc > /dev/null
  echo "packed ../build/pong.wasc"
fi
