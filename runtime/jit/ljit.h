/*
** ljit.h - baseline method JIT: Lua 5.4 bytecode -> WebAssembly at runtime.
**
** A hot function's bytecode is translated into one wasm function that works
** directly on the interpreter's state in linear memory (the Lua stack, the
** CallInfo, the Proto's constants). The module is handed to the host through
** the optional wasmcart import wc_jit_link, which compiles it and installs
** the function in the engine's indirect function table. Nothing is kept in
** wasm locals across an instruction that can call out, so the interpreter can
** take over at any instruction boundary: compiled code "bails" by storing
** ci->u.l.savedpc and returning.
**
** Entry: f(L, ci, pcidx) -> LUAJ_* code. pcidx must be a block leader of the
** compiled function; any other index returns LUAJ_NOENTRY at once.
*/
#ifndef ljit_h
#define ljit_h

#include "lobject.h"
#include "lstate.h"

/* results of compiled code */
#define LUAJ_NOENTRY	0	/* pc is not an entry point: interpret it */
#define LUAJ_BAIL	1	/* interpret from ci->u.l.savedpc, then re-enter */
#define LUAJ_CALL	2	/* a Lua call was set up: L->ci is the callee */
#define LUAJ_RETURN	3	/* the function returned (poscall done) */

/* Proto.jitslot values below 1 */
#define LUAJ_COLD	0	/* not compiled (yet) */
#define LUAJ_FAILED	(-1)	/* cannot or will not compile */
#define LUAJ_PENDING	(-2)	/* asynchronous compile in flight */

LUAI_DDEC(int luaJ_threshold);
/* compiled-code frames currently on the wasm stack (bounded: they are not
   counted by nCcalls, and the host's stack is finite) */
LUAI_DDEC(int luaJ_depth);
/* 0 once the host turned out not to link: the interpreter then skips every
   JIT check with a single test */
LUAI_DDEC(int luaJ_on);

/* true when compiled code may be entered at 'pc' (compiling when hot) */
#define luaJ_ready(L,p,pc) \
	(luaJ_on && !(L)->hookmask && ((p)->jitslot > 0 || \
	  ((((p)->jitslot == LUAJ_COLD && (pc) == (p)->code) || \
	    (p)->jitslot == LUAJ_PENDING) && luaJ_hot(L, p, pc))))

LUAI_FUNC int luaJ_hot (lua_State *L, Proto *p, const Instruction *pc);
LUAI_FUNC int luaJ_enter (lua_State *L, CallInfo *ci, const Instruction *pc);
LUAI_FUNC int luaJ_trap (lua_State *L, CallInfo *ci, const Instruction *pc);
LUAI_FUNC void luaJ_freeproto (lua_State *L, Proto *p);
LUAI_FUNC void luaJ_checkrestore (lua_State *L);
LUAI_FUNC int luaopen_wcljit (lua_State *L);

/* lvm.c internals exported for the helpers */
LUAI_FUNC int luaV_forprep (lua_State *L, StkId ra);
LUAI_FUNC int luaV_floatforloop (StkId ra);
LUAI_FUNC void luaV_pushclosure (lua_State *L, Proto *p, UpVal **encup,
                                 StkId base, StkId ra);

#endif
