/*
 * cartconf.h - route Lua's stdout/stderr hooks to the wasmcart host log.
 *
 * Force-included into every Lua translation unit (-include cartconf.h) so it
 * lands BEFORE lauxlib.h's `#if !defined(lua_writestring)` guards. These are
 * upstream's designated override points, so no Lua source is modified.
 *
 * WHY: a cart has no stdout. Left alone, lua_writestring/lua_writestringerror
 * expand to fwrite(stdout)/fprintf(stderr), which drag WASI imports
 * (fd_write/fd_seek/fd_read/fd_close/clock_time_get) into the engine wasm.
 * Hosts that provide only the wasmcart `env` module then refuse to
 * instantiate the cart at all. Sending Lua's own diagnostics to wc_log is
 * both the correct destination and what keeps the import list clean.
 */
#ifndef WASMCART_LUA_CARTCONF_H
#define WASMCART_LUA_CARTCONF_H

#include <stddef.h>

/* the wasmcart host log import (same one runtime.c uses) */
#ifdef __wasm__
__attribute__((import_module("env"), import_name("wc_log")))
extern void wc_log(const char *ptr, unsigned int len);
#else
static inline void wc_log(const char *ptr, unsigned int len) { (void)ptr; (void)len; }
#endif

void wcl_write(const char *s, size_t len);
void wcl_writef(const char *fmt, const char *arg);

#define lua_writestring(s, l)       wcl_write((s), (size_t)(l))
#define lua_writeline()             wcl_write("\n", 1)
#define lua_writestringerror(s, p)  wcl_writef((s), (const char *)(p))

/*
 * Fixed hash seed (another upstream `#if !defined` override point, in
 * lstate.c). Stock Lua seeds from time(NULL) + ASLR addresses, which
 * (a) imports WASI clock_time_get and (b) makes string-hash order vary
 * between runs -- so `pairs()` iteration order would differ host to host.
 *
 * A cart promises identical frames from identical inputs, so a varying
 * seed is a bug here, not a security feature: there is no untrusted input
 * to defend against inside a sandboxed single-tenant cart. Fixing it makes
 * table iteration reproducible, which is what replay and frame-hash
 * regression goldens depend on.
 */
#define luai_makeseed(L)            ((unsigned int)0x9E3779B9u)

#endif /* WASMCART_LUA_CARTCONF_H */
