#!/usr/bin/env python3
"""Guard Lua's file-loading functions behind LUA_CART_NOFILES.

A wasmcart cart has no filesystem: it reads its Lua from the cart asset bundle
through wc_asset_read, never from a path. luaL_loadfilex/luaL_dofile are
therefore dead code -- but they are the last things in the engine that touch
stdio (fopen/getc/freopen), and linking them drags WASI imports
(fd_write/fd_read/fd_seek/fd_close/clock_time_get) into the wasm. Hosts that
provide only the wasmcart `env` module then refuse to instantiate the cart.

So we compile them out rather than tolerate imports we can never satisfy.
Upstream Lua is unmodified on disk apart from this guard, which is applied
idempotently at build time (build.sh skips it if already present).
"""
import re
import sys

MARKER = "LUA_CART_NOFILES"

# The file-loading block runs from the skipcomment helper up to (not into) the
# LoadS/getS string-reader that luaL_loadbufferx needs and we DO keep.
START = "static int skipcomment"
END_MARKER = "typedef struct LoadS"


def patch_mathlib(path):
    """Make math.random's default seed deterministic.

    lmathlib.c seeds from time(NULL) with no override hook (unlike lstate.c's
    luai_makeseed). That single call imports WASI clock_time_get and makes the
    default RNG stream differ per run. The engine's prelude replaces
    math.random with the host-seeded generator anyway, so this only affects
    the unused fallback -- but an unused fallback is not worth a WASI import
    or a hole in the determinism promise.
    """
    src = open(path, encoding="utf-8").read()
    if MARKER in src:
        print(f"patch-lua: {path} already patched")
        return 0
    old = "lua_Unsigned seed1 = (lua_Unsigned)time(NULL);"
    if old not in src:
        print(f"patch-lua: FAILED to locate the time() seed in {path}",
              file=sys.stderr)
        return 1
    new = ("/* " + MARKER + ": carts have no wall clock; fixed seed keeps the\n"
           "     engine free of WASI imports and replays reproducible. */\n"
           "  lua_Unsigned seed1 = (lua_Unsigned)0x2545F4914F6CDD1DULL;")
    open(path, "w", encoding="utf-8").write(src.replace(old, new, 1))
    print(f"patch-lua: fixed the RNG seed in {path}")
    return 0


def main(path):
    if path.endswith("lmathlib.c"):
        return patch_mathlib(path)

    src = open(path, encoding="utf-8").read()
    if MARKER in src:
        print(f"patch-lua: {path} already guarded")
        return 0

    i = src.find(START)
    j = src.find(END_MARKER)
    if i < 0 or j < 0 or j <= i:
        print(f"patch-lua: FAILED to locate the file-loading block in {path}",
              file=sys.stderr)
        return 1

    block = src[i:j]
    guarded = (
        f"#if !defined({MARKER})\n"
        f"{block}"
        f"#else\n"
        "/* wasmcart: no filesystem — these are compiled out to keep the\n"
        " * engine free of WASI stdio imports. See patch-lua.py. */\n"
        "LUALIB_API int luaL_loadfilex (lua_State *L, const char *filename,\n"
        "                               const char *mode) {\n"
        "  (void)mode;\n"
        "  lua_pushfstring(L, \"cannot open %s: carts have no filesystem\",\n"
        "                  filename ? filename : \"(stdin)\");\n"
        "  return LUA_ERRFILE;\n"
        "}\n"
        "#endif\n\n"
    )

    out = src[:i] + guarded + src[j:]
    open(path, "w", encoding="utf-8").write(out)
    print(f"patch-lua: guarded file-loading in {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
