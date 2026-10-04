/*
** ljit.c - baseline method JIT: Lua 5.4 bytecode -> WebAssembly at runtime.
** See ljit.h for the model. Layout of this file:
**   1. host imports and configuration
**   2. helpers the generated code calls (exported from the engine)
**   3. the wasm binary emitter
**   4. the bytecode translator
**   5. compile / enter / trap glue called from lvm.c
**   6. the Lua control library (wc.jit)
*/

#define ljit_c
#define LUA_CORE

#include "lprefix.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"
#include "lvm.h"
#include "ljit.h"


/* {==================================================================
** 1. Host imports and configuration
** =================================================================== */

#if defined(__wasm__)
#define JIT_EXPORT	__attribute__((used, visibility("default")))
__attribute__((import_module("env"), import_name("wc_jit_link")))
extern int wc_jit_link (const void *bytes, unsigned int len, int *result);
__attribute__((import_module("env"), import_name("wc_jit_config")))
extern int wc_jit_config (int key);
#else
#define JIT_EXPORT
static int wc_jit_link (const void *b, unsigned int n, int *r) {
  (void)b; (void)n; *r = -1; return -1;
}
static int wc_jit_config (int key) { (void)key; return -1; }
#endif

#if !defined(LUAJ_DEFAULT_THRESHOLD)
#define LUAJ_DEFAULT_THRESHOLD	500
#endif

/* functions larger than this are left to the interpreter */
#define MAXJITCODE	12000
#define MAXJITBLOCKS	4000

int luaJ_threshold = LUAJ_DEFAULT_THRESHOLD;

static int jit_state = 0;  /* 0 = not initialized, 1 = on, -1 = off */
static int jit_userthreshold = -1;  /* threshold set from Lua, if any */

static struct {
  lu_mem compiled, failed, modules, bytes, rejected, bails, pending;
  const char *lasterr;
} jst;


/* asynchronous compiles report through these cells (see wc_jit_link) */
#define NCELLS	1024
static int jit_cells[NCELLS];
static Proto *jit_cellowner[NCELLS];
static int jit_cellused[NCELLS];

static int cell_alloc (Proto *p) {
  int i;
  for (i = 0; i < NCELLS; i++) {
    /* reclaim cells whose owner died before the host answered */
    if (jit_cellused[i] && jit_cellowner[i] == NULL && jit_cells[i] != -2)
      jit_cellused[i] = 0;
    if (!jit_cellused[i]) {
      jit_cellused[i] = 1;
      jit_cellowner[i] = p;
      jit_cells[i] = -2;
      return i;
    }
  }
  return -1;
}


static void jit_init (void) {
  int v;
  jit_state = 1;
  if (wc_jit_config(1) == 1) jit_state = -1;
  v = wc_jit_config(0);
  if (v >= 0 && jit_userthreshold < 0) luaJ_threshold = v;
}

/* }================================================================== */


/* {==================================================================
** 2. Helpers called from generated code
**
** Generated code stores ci->u.l.savedpc before calling any of these, so
** errors, hooks and yields see the right instruction. Each helper does
** what the interpreter does for the same instruction, using the same
** functions, so the semantics are the interpreter's by construction.
** Pointers into the stack are recomputed after anything that can move it.
** =================================================================== */

#define hbase(ci)	((ci)->func.p + 1)
#define savestate(L,ci)	(L->top.p = (ci)->top.p)

JIT_EXPORT void luaJ_h_get (lua_State *L, CallInfo *ci, TValue *t,
                            TValue *key, StkId ra) {
  const TValue *slot;
  if (ttisinteger(key)) {
    lua_Unsigned n = ivalue(key);
    if (luaV_fastgeti(L, t, n, slot)) { setobj2s(L, ra, slot); return; }
  }
  else if (luaV_fastget(L, t, key, slot, luaH_get)) {
    setobj2s(L, ra, slot);
    return;
  }
  savestate(L, ci);
  luaV_finishget(L, t, key, ra, slot);
}

JIT_EXPORT void luaJ_h_geti (lua_State *L, CallInfo *ci, TValue *t, int c,
                             StkId ra) {
  const TValue *slot;
  if (luaV_fastgeti(L, t, c, slot)) {
    setobj2s(L, ra, slot);
  }
  else {
    TValue key;
    setivalue(&key, c);
    savestate(L, ci);
    luaV_finishget(L, t, &key, ra, slot);
  }
}

JIT_EXPORT void luaJ_h_set (lua_State *L, CallInfo *ci, TValue *t,
                            TValue *key, TValue *val) {
  const TValue *slot;
  if (ttisinteger(key)) {
    lua_Unsigned n = ivalue(key);
    if (luaV_fastgeti(L, t, n, slot)) {
      luaV_finishfastset(L, t, slot, val);
      return;
    }
  }
  else if (luaV_fastget(L, t, key, slot, luaH_get)) {
    luaV_finishfastset(L, t, slot, val);
    return;
  }
  savestate(L, ci);
  luaV_finishset(L, t, key, val, slot);
}

JIT_EXPORT void luaJ_h_seti (lua_State *L, CallInfo *ci, TValue *t, int c,
                             TValue *val) {
  const TValue *slot;
  if (luaV_fastgeti(L, t, c, slot)) {
    luaV_finishfastset(L, t, slot, val);
  }
  else {
    TValue key;
    setivalue(&key, c);
    savestate(L, ci);
    luaV_finishset(L, t, &key, val, slot);
  }
}

JIT_EXPORT const TValue *luaJ_h_getshortstr (Table *t, TString *key) {
  return luaH_getshortstr(t, key);
}

JIT_EXPORT void luaJ_h_barrierback (lua_State *L, Table *t, TValue *v) {
  luaC_barrierback(L, obj2gco(t), v);
}

JIT_EXPORT void luaJ_h_barrier (lua_State *L, UpVal *uv, TValue *v) {
  luaC_barrier(L, uv, v);
}

JIT_EXPORT void luaJ_h_mmbin (lua_State *L, CallInfo *ci, TValue *a,
                              TValue *b, StkId res, int tm) {
  savestate(L, ci);
  luaT_trybinTM(L, a, b, res, cast(TMS, tm));
}

JIT_EXPORT void luaJ_h_mmbini (lua_State *L, CallInfo *ci, TValue *a,
                               int imm, int flip, StkId res, int tm) {
  savestate(L, ci);
  luaT_trybiniTM(L, a, imm, flip, res, cast(TMS, tm));
}

JIT_EXPORT void luaJ_h_mmbink (lua_State *L, CallInfo *ci, TValue *a,
                               TValue *imm, int flip, StkId res, int tm) {
  savestate(L, ci);
  luaT_trybinassocTM(L, a, imm, flip, res, cast(TMS, tm));
}

JIT_EXPORT lua_Integer luaJ_h_idiv (lua_State *L, CallInfo *ci,
                                    lua_Integer a, lua_Integer b) {
  savestate(L, ci);
  return luaV_idiv(L, a, b);
}

JIT_EXPORT lua_Integer luaJ_h_imod (lua_State *L, CallInfo *ci,
                                    lua_Integer a, lua_Integer b) {
  savestate(L, ci);
  return luaV_mod(L, a, b);
}

JIT_EXPORT lua_Number luaJ_h_fmod (lua_Number a, lua_Number b) {
  return luaV_modf(NULL, a, b);
}

JIT_EXPORT lua_Number luaJ_h_pow (lua_Number a, lua_Number b) {
  return luai_numpow(NULL, a, b);
}

JIT_EXPORT lua_Integer luaJ_h_shiftl (lua_Integer x, lua_Integer y) {
  return luaV_shiftl(x, y);
}

/*
** Bitwise operation with operands that are not both integers (floats with
** an exact integer value convert). Returns 1 when done, 0 when the next
** instruction (the metamethod one) must run.
*/
static lua_Integer bitop (int op, lua_Integer a, lua_Integer b) {
  switch (op) {
    case OP_BAND: case OP_BANDK: return intop(&, a, b);
    case OP_BOR: case OP_BORK: return intop(|, a, b);
    case OP_BXOR: case OP_BXORK: return intop(^, a, b);
    case OP_SHL: return luaV_shiftl(a, b);
    case OP_SHR: return luaV_shiftl(a, intop(-, 0, b));
    case OP_SHRI: return luaV_shiftl(a, -b);
    case OP_SHLI: return luaV_shiftl(b, a);
    default: lua_assert(0); return 0;
  }
}

JIT_EXPORT int luaJ_h_bitop (int op, TValue *v1, TValue *v2, StkId ra) {
  lua_Integer i1, i2;
  if (tointegerns(v1, &i1) && tointegerns(v2, &i2)) {
    setivalue(s2v(ra), bitop(op, i1, i2));
    return 1;
  }
  return 0;
}

JIT_EXPORT int luaJ_h_bitopi (int op, TValue *v1, int imm, StkId ra) {
  lua_Integer i1;
  if (tointegerns(v1, &i1)) {
    setivalue(s2v(ra), bitop(op, i1, imm));
    return 1;
  }
  return 0;
}

JIT_EXPORT void luaJ_h_unm (lua_State *L, CallInfo *ci, TValue *rb,
                            StkId ra) {
  savestate(L, ci);
  luaT_trybinTM(L, rb, rb, ra, TM_UNM);
}

JIT_EXPORT void luaJ_h_bnot (lua_State *L, CallInfo *ci, TValue *rb,
                             StkId ra) {
  lua_Integer ib;
  if (tointegerns(rb, &ib)) {
    setivalue(s2v(ra), intop(^, ~l_castS2U(0), ib));
  }
  else {
    savestate(L, ci);
    luaT_trybinTM(L, rb, rb, ra, TM_BNOT);
  }
}

JIT_EXPORT void luaJ_h_len (lua_State *L, CallInfo *ci, int a, int b) {
  StkId base = hbase(ci);
  savestate(L, ci);
  luaV_objlen(L, base + a, s2v(base + b));
}

JIT_EXPORT void luaJ_h_concat (lua_State *L, CallInfo *ci, int a, int n) {
  StkId ra = hbase(ci) + a;
  L->top.p = ra + n;  /* mark the end of concat operands */
  luaV_concat(L, n);
  luaC_condGC(L, (void)0, (void)0);  /* 'luaV_concat' ensures correct top */
}

JIT_EXPORT int luaJ_h_eq (lua_State *L, CallInfo *ci, TValue *a, TValue *b) {
  savestate(L, ci);
  return luaV_equalobj(L, a, b);
}

JIT_EXPORT int luaJ_h_lt (lua_State *L, CallInfo *ci, TValue *a, TValue *b) {
  savestate(L, ci);
  return luaV_lessthan(L, a, b);
}

JIT_EXPORT int luaJ_h_le (lua_State *L, CallInfo *ci, TValue *a, TValue *b) {
  savestate(L, ci);
  return luaV_lessequal(L, a, b);
}

JIT_EXPORT int luaJ_h_orderi (lua_State *L, CallInfo *ci, TValue *a, int im,
                              int inv, int isf, int tm) {
  savestate(L, ci);
  return luaT_callorderiTM(L, a, im, inv, isf, cast(TMS, tm));
}

JIT_EXPORT int luaJ_h_rawequal (TValue *a, TValue *b) {
  return luaV_rawequalobj(a, b);
}

/* OP_CALL: returns 1 when a Lua call was set up (L->ci is the callee) */
JIT_EXPORT int luaJ_h_call (lua_State *L, CallInfo *ci, int a, int b,
                            int nresults) {
  StkId ra = hbase(ci) + a;
  (void)ci;
  if (b != 0)  /* fixed number of arguments? */
    L->top.p = ra + b;  /* top signals number of arguments */
  /* else previous instruction set top */
  return luaD_precall(L, ra, nresults) != NULL;
}

/* OP_TAILCALL: returns LUAJ_CALL or LUAJ_RETURN */
JIT_EXPORT int luaJ_h_tailcall (lua_State *L, CallInfo *ci, int a, int b,
                                int nparams1, int k) {
  StkId base = hbase(ci);
  StkId ra = base + a;
  int n;
  int delta = (nparams1) ? ci->u.l.nextraargs + nparams1 : 0;
  if (b != 0)
    L->top.p = ra + b;
  else  /* previous instruction set top */
    b = cast_int(L->top.p - ra);
  if (k)
    luaF_closeupval(L, base);  /* close upvalues from current call */
  if ((n = luaD_pretailcall(L, ci, ra, b, delta)) < 0)  /* Lua function? */
    return LUAJ_CALL;
  ci->func.p -= delta;  /* restore 'func' (if vararg) */
  luaD_poscall(L, ci, n);  /* finish caller */
  return LUAJ_RETURN;
}

/* OP_RETURN */
JIT_EXPORT void luaJ_h_return (lua_State *L, CallInfo *ci, int a, int n,
                               int nparams1, int k) {
  StkId ra = hbase(ci) + a;
  if (n < 0)  /* not fixed? */
    n = cast_int(L->top.p - ra);  /* get what is available */
  if (k) {  /* may there be open upvalues? */
    ci->u2.nres = n;  /* save number of returns */
    if (L->top.p < ci->top.p)
      L->top.p = ci->top.p;
    luaF_close(L, hbase(ci), CLOSEKTOP, 1);
    ra = hbase(ci) + a;
  }
  if (nparams1)  /* vararg function? */
    ci->func.p -= ci->u.l.nextraargs + nparams1;
  L->top.p = ra + n;  /* set call for 'luaD_poscall' */
  luaD_poscall(L, ci, n);
}

/* OP_RETURN0 / OP_RETURN1 without hooks (code bails when hooks are on) */
JIT_EXPORT void luaJ_h_return0 (lua_State *L, CallInfo *ci) {
  int nres;
  StkId base = hbase(ci);
  L->ci = ci->previous;  /* back to caller */
  L->top.p = base - 1;
  for (nres = ci->nresults; l_unlikely(nres > 0); nres--)
    setnilvalue(s2v(L->top.p++));  /* all results are nil */
}

JIT_EXPORT void luaJ_h_return1 (lua_State *L, CallInfo *ci, int a) {
  StkId base = hbase(ci);
  int nres = ci->nresults;
  L->ci = ci->previous;  /* back to caller */
  if (nres == 0)
    L->top.p = base - 1;  /* asked for no results */
  else {
    setobjs2s(L, base - 1, base + a);  /* at least this result */
    L->top.p = base;
    for (; l_unlikely(nres > 1); nres--)
      setnilvalue(s2v(L->top.p++));  /* complete missing results */
  }
}

JIT_EXPORT int luaJ_h_forprep (lua_State *L, CallInfo *ci, StkId ra) {
  savestate(L, ci);
  return luaV_forprep(L, ra);
}

JIT_EXPORT int luaJ_h_floatforloop (StkId ra) {
  return luaV_floatforloop(ra);
}

JIT_EXPORT void luaJ_h_tforprep (lua_State *L, CallInfo *ci, StkId ra) {
  savestate(L, ci);
  luaF_newtbcupval(L, ra + 3);
}

JIT_EXPORT void luaJ_h_tforcall (lua_State *L, CallInfo *ci, int a, int c) {
  StkId ra = hbase(ci) + a;
  (void)ci;
  memcpy(ra + 4, ra, 3 * sizeof(*ra));
  L->top.p = ra + 4 + 3;
  luaD_call(L, ra + 4, c);  /* do the call */
}

JIT_EXPORT void luaJ_h_newtable (lua_State *L, CallInfo *ci, int a, int b,
                                 int c) {
  StkId ra = hbase(ci) + a;
  Table *t;
  L->top.p = ra + 1;  /* correct top in case of emergency GC */
  t = luaH_new(L);  /* memory allocation */
  sethvalue2s(L, ra, t);
  if (b != 0 || c != 0)
    luaH_resize(L, t, c, b);  /* idem */
  luaC_condGC(L, L->top.p = ra + 1, (void)0);
}

JIT_EXPORT void luaJ_h_setlist (lua_State *L, CallInfo *ci, int a, int n,
                                unsigned int last) {
  StkId ra = hbase(ci) + a;
  Table *h = hvalue(s2v(ra));
  if (n == 0)
    n = cast_int(L->top.p - ra) - 1;  /* get up to the top */
  else
    L->top.p = ci->top.p;  /* correct top in case of emergency GC */
  last += n;
  if (last > luaH_realasize(h))  /* needs more space? */
    luaH_resizearray(L, h, last);  /* preallocate it at once */
  for (; n > 0; n--) {
    TValue *val = s2v(ra + n);
    setobj2t(L, &h->array[last - 1], val);
    last--;
    luaC_barrierback(L, obj2gco(h), val);
  }
}

JIT_EXPORT void luaJ_h_closure (lua_State *L, CallInfo *ci, int a,
                                Proto *p) {
  StkId base = hbase(ci);
  savestate(L, ci);
  luaV_pushclosure(L, p, ci_func(ci)->upvals, base, base + a);
  luaC_condGC(L, L->top.p = hbase(ci) + a + 1, (void)0);
}

JIT_EXPORT void luaJ_h_vararg (lua_State *L, CallInfo *ci, int a, int n) {
  savestate(L, ci);
  luaT_getvarargs(L, ci, hbase(ci) + a, n);
}

JIT_EXPORT void luaJ_h_varargprep (lua_State *L, CallInfo *ci, int a) {
  luaT_adjustvarargs(L, a, ci, ci_func(ci)->p);
}

JIT_EXPORT void luaJ_h_close (lua_State *L, CallInfo *ci, int a) {
  savestate(L, ci);
  luaF_close(L, hbase(ci) + a, LUA_OK, 1);
}

JIT_EXPORT void luaJ_h_tbc (lua_State *L, CallInfo *ci, int a) {
  savestate(L, ci);
  luaF_newtbcupval(L, hbase(ci) + a);
}

/* }================================================================== */


/* {==================================================================
** 3. WebAssembly binary emitter
** =================================================================== */

typedef struct Buf {
  unsigned char *b;
  size_t n, cap;
  int oom;
} Buf;

static void bgrow (Buf *B, size_t need) {
  if (B->n + need > B->cap) {
    size_t nc = B->cap ? B->cap * 2 : 1024;
    unsigned char *nb;
    while (nc < B->n + need) nc *= 2;
    nb = (unsigned char *)realloc(B->b, nc);
    if (nb == NULL) { B->oom = 1; return; }
    B->b = nb;
    B->cap = nc;
  }
}

static void b1 (Buf *B, int x) {
  bgrow(B, 1);
  if (!B->oom) B->b[B->n++] = (unsigned char)x;
}

static void bmem (Buf *B, const void *p, size_t n) {
  bgrow(B, n);
  if (!B->oom) { memcpy(B->b + B->n, p, n); B->n += n; }
}

static void bu (Buf *B, uint64_t v) {
  do {
    int c = (int)(v & 0x7f);
    v >>= 7;
    if (v) c |= 0x80;
    b1(B, c);
  } while (v);
}

static void bs (Buf *B, int64_t v) {
  for (;;) {
    int c = (int)(v & 0x7f);
    v >>= 7;  /* arithmetic shift */
    if ((v == 0 && !(c & 0x40)) || (v == -1 && (c & 0x40))) {
      b1(B, c);
      return;
    }
    b1(B, c | 0x80);
  }
}

static void bname (Buf *B, const char *s) {
  size_t n = strlen(s);
  bu(B, n);
  bmem(B, s, n);
}

/* append a section with id 'id' and contents 'S' */
static void bsection (Buf *M, int id, Buf *S) {
  b1(M, id);
  bu(M, S->n);
  bmem(M, S->b, S->n);
}

/* value types */
#define T_I32	0x7f
#define T_I64	0x7e
#define T_F64	0x7c
#define T_VOID	0x40

/* opcodes */
#define W_UNREACHABLE	0x00
#define W_BLOCK		0x02
#define W_LOOP		0x03
#define W_IF		0x04
#define W_ELSE		0x05
#define W_END		0x0b
#define W_BR		0x0c
#define W_BRIF		0x0d
#define W_BRTABLE	0x0e
#define W_RETURN	0x0f
#define W_CALL		0x10
#define W_DROP		0x1a
#define W_SELECT	0x1b
#define W_LGET		0x20
#define W_LSET		0x21
#define W_LTEE		0x22
#define W_I32LOAD	0x28
#define W_I64LOAD	0x29
#define W_F64LOAD	0x2b
#define W_I32LOAD8U	0x2d
#define W_I32STORE	0x36
#define W_I64STORE	0x37
#define W_F64STORE	0x39
#define W_I32STORE8	0x3a
#define W_I32CONST	0x41
#define W_I64CONST	0x42
#define W_F64CONST	0x44
#define W_I32EQZ	0x45
#define W_I32EQ		0x46
#define W_I32NE		0x47
#define W_I32LTU	0x49
#define W_I32GEU	0x4f
#define W_I64EQZ	0x50
#define W_I64EQ		0x51
#define W_I64NE		0x52
#define W_I64LTS	0x53
#define W_I64LTU	0x54
#define W_I64GTS	0x55
#define W_I64LES	0x57
#define W_I64GES	0x59
#define W_F64EQ		0x61
#define W_F64NE		0x62
#define W_F64LT		0x63
#define W_F64GT		0x64
#define W_F64LE		0x65
#define W_F64GE		0x66
#define W_I32ADD	0x6a
#define W_I32SUB	0x6b
#define W_I32MUL	0x6c
#define W_I32AND	0x71
#define W_I32OR		0x72
#define W_I32XOR	0x73
#define W_I32SHL	0x74
#define W_I64ADD	0x7c
#define W_I64SUB	0x7d
#define W_I64MUL	0x7e
#define W_I64DIVS	0x7f
#define W_I64REMS	0x81
#define W_I64AND	0x83
#define W_I64OR		0x84
#define W_I64XOR	0x85
#define W_F64NEG	0x9a
#define W_F64FLOOR	0x9c
#define W_F64ADD	0xa0
#define W_F64SUB	0xa1
#define W_F64MUL	0xa2
#define W_F64DIV	0xa3
#define W_I32WRAPI64	0xa7
#define W_I64EXTI32U	0xad
#define W_F64CVTI64S	0xb9

/* }================================================================== */


/* {==================================================================
** 4. Bytecode translator
** =================================================================== */

/* helper imports: name and signature ("params:results", i/l/d) */
typedef enum {
  H_GET, H_GETI, H_SET, H_SETI, H_GETSHORTSTR, H_BARRIERBACK, H_BARRIER,
  H_MMBIN, H_MMBINI, H_MMBINK, H_IDIV, H_IMOD, H_FMOD, H_POW, H_SHIFTL,
  H_BITOP, H_BITOPI, H_UNM, H_BNOT, H_LEN, H_CONCAT, H_EQ, H_LT, H_LE,
  H_ORDERI, H_RAWEQUAL, H_CALL, H_TAILCALL, H_RETURN, H_RETURN0, H_RETURN1,
  H_FORPREP, H_FLOATFORLOOP, H_TFORPREP, H_TFORCALL, H_NEWTABLE, H_SETLIST,
  H_CLOSURE, H_VARARG, H_VARARGPREP, H_CLOSE, H_TBC,
  H__N
} HelperId;

static const struct { const char *name; const char *sig; } helpers[H__N] = {
  {"luaJ_h_get", "iiiii:"},
  {"luaJ_h_geti", "iiiii:"},
  {"luaJ_h_set", "iiiii:"},
  {"luaJ_h_seti", "iiiii:"},
  {"luaJ_h_getshortstr", "ii:i"},
  {"luaJ_h_barrierback", "iii:"},
  {"luaJ_h_barrier", "iii:"},
  {"luaJ_h_mmbin", "iiiiii:"},
  {"luaJ_h_mmbini", "iiiiiii:"},
  {"luaJ_h_mmbink", "iiiiiii:"},
  {"luaJ_h_idiv", "iill:l"},
  {"luaJ_h_imod", "iill:l"},
  {"luaJ_h_fmod", "dd:d"},
  {"luaJ_h_pow", "dd:d"},
  {"luaJ_h_shiftl", "ll:l"},
  {"luaJ_h_bitop", "iiii:i"},
  {"luaJ_h_bitopi", "iiii:i"},
  {"luaJ_h_unm", "iiii:"},
  {"luaJ_h_bnot", "iiii:"},
  {"luaJ_h_len", "iiii:"},
  {"luaJ_h_concat", "iiii:"},
  {"luaJ_h_eq", "iiii:i"},
  {"luaJ_h_lt", "iiii:i"},
  {"luaJ_h_le", "iiii:i"},
  {"luaJ_h_orderi", "iiiiiii:i"},
  {"luaJ_h_rawequal", "ii:i"},
  {"luaJ_h_call", "iiiii:i"},
  {"luaJ_h_tailcall", "iiiiii:i"},
  {"luaJ_h_return", "iiiiii:"},
  {"luaJ_h_return0", "ii:"},
  {"luaJ_h_return1", "iii:"},
  {"luaJ_h_forprep", "iii:i"},
  {"luaJ_h_floatforloop", "i:i"},
  {"luaJ_h_tforprep", "iii:"},
  {"luaJ_h_tforcall", "iiii:"},
  {"luaJ_h_newtable", "iiiii:"},
  {"luaJ_h_setlist", "iiiii:"},
  {"luaJ_h_closure", "iiii:"},
  {"luaJ_h_vararg", "iiii:"},
  {"luaJ_h_varargprep", "iii:"},
  {"luaJ_h_close", "iii:"},
  {"luaJ_h_tbc", "iii:"},
};

/* locals of the generated function */
#define LV_L	0
#define LV_CI	1
#define LV_PC	2
#define LV_BASE	3
#define LV_CL	4
#define LV_T0	5	/* i32 temps T0..T5 */
#define NI32TMP	6
#define LV_X0	(LV_T0 + NI32TMP)	/* i64 temps X0..X3 */
#define NI64TMP	4
#define LV_D0	(LV_X0 + NI64TMP)	/* f64 temps D0..D3 */
#define NF64TMP	4
#define LV_X1	(LV_X0 + 1)
#define LV_D1	(LV_D0 + 1)

/* layout constants */
#define SV	((uint32_t)sizeof(StackValue))
#define TTOFF	((uint32_t)offsetof(TValue, tt_))
#define TAG_INT	LUA_VNUMINT
#define TAG_FLT	LUA_VNUMFLT
#define TAG_TAB	ctb(LUA_VTABLE)
#define TAG_SSTR	ctb(LUA_VSHRSTR)

typedef struct J {
  Buf c;  /* function body */
  Proto *p;
  int n;  /* number of instructions */
  int *blk;  /* block index of a leader pc, -1 elsewhere */
  int nblk;
  int cur;  /* block being emitted */
  int extra;  /* structured constructs open inside the current block */
  int dead;  /* code is unreachable until the next leader */
  int skipdepth;  /* 'extra' just inside an arith op's skip block, or -1 */
  const char *err;
} J;

/* an addressable TValue: register, absolute address, or local + offset */
typedef struct Ad {
  int kind;  /* 0 = base + off, 1 = absolute off, 2 = local 'loc' + off */
  uint32_t off;
  int loc;
} Ad;

static Ad R (int r) { Ad a; a.kind = 0; a.off = (uint32_t)r * SV; a.loc = 0; return a; }
static Ad KA (const TValue *k) { Ad a; a.kind = 1; a.off = (uint32_t)(uintptr_t)k; a.loc = 0; return a; }
static Ad LA (int loc) { Ad a; a.kind = 2; a.off = 0; a.loc = loc; return a; }

#define E(j,x)	b1(&(j)->c, (x))

static void e_lget (J *j, int i) { E(j, W_LGET); bu(&j->c, i); }
static void e_lset (J *j, int i) { E(j, W_LSET); bu(&j->c, i); }
static void e_ltee (J *j, int i) { E(j, W_LTEE); bu(&j->c, i); }
static void e_i32 (J *j, int32_t v) { E(j, W_I32CONST); bs(&j->c, v); }
static void e_i64 (J *j, int64_t v) { E(j, W_I64CONST); bs(&j->c, v); }
static void e_f64 (J *j, double d) { E(j, W_F64CONST); bmem(&j->c, &d, 8); }
static void e_mem (J *j, int op, int align, uint32_t off) {
  E(j, op); bu(&j->c, align); bu(&j->c, off);
}
static void e_call (J *j, HelperId h) { E(j, W_CALL); bu(&j->c, h); }

static void e_block (J *j) { E(j, W_BLOCK); E(j, T_VOID); j->extra++; }
static void e_blockt (J *j, int t) { E(j, W_BLOCK); E(j, t); j->extra++; }
static void e_if (J *j) { E(j, W_IF); E(j, T_VOID); j->extra++; }
static void e_ift (J *j, int t) { E(j, W_IF); E(j, t); j->extra++; }
static void e_else (J *j) { E(j, W_ELSE); }
static void e_end (J *j) { E(j, W_END); j->extra--; }
static void e_br (J *j, int d) { E(j, W_BR); bu(&j->c, d); }
static void e_brif (J *j, int d) { E(j, W_BRIF); bu(&j->c, d); }

/* push the base address of 'a' (the memory offset carries a.off) */
static void e_adbase (J *j, Ad a) {
  if (a.kind == 0) e_lget(j, LV_BASE);
  else if (a.kind == 1) e_i32(j, 0);
  else e_lget(j, a.loc);
}

/* push the address of 'a' as a value */
static void e_ptr (J *j, Ad a) {
  if (a.kind == 1) { e_i32(j, (int32_t)a.off); return; }
  e_adbase(j, a);
  if (a.off) { e_i32(j, (int32_t)a.off); E(j, W_I32ADD); }
}

static void e_ldtag (J *j, Ad a) { e_adbase(j, a); e_mem(j, W_I32LOAD8U, 0, a.off + TTOFF); }
static void e_ldi64 (J *j, Ad a) { e_adbase(j, a); e_mem(j, W_I64LOAD, 3, a.off); }
static void e_ldf64 (J *j, Ad a) { e_adbase(j, a); e_mem(j, W_F64LOAD, 3, a.off); }
static void e_ldptr (J *j, Ad a) { e_adbase(j, a); e_mem(j, W_I32LOAD, 2, a.off); }

static void e_settag (J *j, Ad a, int tag) {
  e_adbase(j, a); e_i32(j, tag); e_mem(j, W_I32STORE8, 0, a.off + TTOFF);
}

/* the stack holds an i64 / f64; store it into 'a' (needs the value in a local) */
static void e_sti64_from (J *j, Ad a, int loc) {
  e_adbase(j, a); e_lget(j, loc); e_mem(j, W_I64STORE, 3, a.off);
}
static void e_stf64_from (J *j, Ad a, int loc) {
  e_adbase(j, a); e_lget(j, loc); e_mem(j, W_F64STORE, 3, a.off);
}

/* dst := src (value and tag, like setobj) */
static void e_copy (J *j, Ad dst, Ad src) {
  e_adbase(j, dst); e_ldi64(j, src); e_mem(j, W_I64STORE, 3, dst.off);
  e_adbase(j, dst); e_ldtag(j, src); e_mem(j, W_I32STORE8, 0, dst.off + TTOFF);
}

static void e_reloadbase (J *j) {
  e_lget(j, LV_CI);
  e_mem(j, W_I32LOAD, 2, (uint32_t)offsetof(CallInfo, func));
  e_i32(j, (int32_t)SV);
  E(j, W_I32ADD);
  e_lset(j, LV_BASE);
}

/* ci->u.l.savedpc = &code[pc] */
static void e_savepc (J *j, int pc) {
  e_lget(j, LV_CI);
  e_i32(j, (int32_t)(uintptr_t)(j->p->code + pc));
  e_mem(j, W_I32STORE, 2, (uint32_t)offsetof(CallInfo, u.l.savedpc));
}

static void e_return (J *j, int code) { e_i32(j, code); E(j, W_RETURN); }

/* interpret from 'pc' (the interpreter re-enters at the next leader) */
static void e_bail (J *j, int pc) { e_savepc(j, pc); e_return(j, LUAJ_BAIL); }

/* if hooks got switched on, continue in the interpreter at 'pc' */
static void e_hookcheck (J *j, int pc) {
  e_lget(j, LV_L);
  e_mem(j, W_I32LOAD, 2, (uint32_t)offsetof(lua_State, hookmask));
  e_if(j);
  e_bail(j, pc);
  e_end(j);
}

/* unconditional jump to the leader at 'target' */
static void e_goto (J *j, int target) {
  int t = j->blk[target];
  lua_assert(t >= 0);
  if (t > j->cur)
    e_br(j, t - j->cur - 1 + j->extra);
  else {  /* backward: through the dispatch loop */
    e_i32(j, target);
    e_lset(j, LV_PC);
    e_br(j, j->nblk - 1 - j->cur + j->extra);
  }
}

/* jump to 'target' if the i32 on the stack is nonzero */
static void e_goto_if (J *j, int target) {
  int t = j->blk[target];
  lua_assert(t >= 0);
  if (t > j->cur)
    e_brif(j, t - j->cur - 1 + j->extra);
  else {
    e_if(j);
    e_goto(j, target);
    e_end(j);
  }
}

/* push l_isfalse(a) as i32 */
static void e_isfalse (J *j, Ad a) {
  e_ldtag(j, a);
  e_ltee(j, LV_T0 + 5);
  e_i32(j, LUA_VFALSE);
  E(j, W_I32EQ);
  e_lget(j, LV_T0 + 5);
  e_i32(j, 0x0f);
  E(j, W_I32AND);
  E(j, W_I32EQZ);
  E(j, W_I32OR);
}

/* the operand of an arithmetic instruction: a register or a known number */
typedef struct NOp {
  int isk;  /* 0 = register at 'ad'; 1 = constant */
  Ad ad;  /* register, or the constant's address (for helpers) */
  int kint;  /* constant is an integer */
  lua_Integer ki;
  lua_Number kf;
} NOp;

static NOp nreg (int r) { NOp o; memset(&o, 0, sizeof(o)); o.ad = R(r); return o; }
static NOp nk (const TValue *k) {
  NOp o; memset(&o, 0, sizeof(o));
  o.isk = 1; o.ad = KA(k);
  if (ttisinteger(k)) { o.kint = 1; o.ki = ivalue(k); }
  else { o.kint = 0; o.kf = fltvalue(k); }
  return o;
}
static NOp nimm (int v) {
  NOp o; memset(&o, 0, sizeof(o));
  o.isk = 1; o.kint = 1; o.ki = v; o.ad.kind = -1;
  return o;
}

static void e_nop_i64 (J *j, NOp o) {
  if (o.isk) e_i64(j, o.ki); else e_ldi64(j, o.ad);
}

/* convert 'o' to a float into local 'dloc', or branch 'fail' levels out
   (counted from the level where this is called) */
static void e_tonum (J *j, NOp o, int dloc, int faildepth) {
  if (o.isk) {
    e_f64(j, o.kint ? cast_num(o.ki) : o.kf);
    e_lset(j, dloc);
    return;
  }
  e_ldtag(j, o.ad);
  e_ltee(j, LV_T0 + 4);
  e_i32(j, TAG_FLT);
  E(j, W_I32EQ);
  e_ift(j, T_F64);
  e_ldf64(j, o.ad);
  e_else(j);
  e_lget(j, LV_T0 + 4);
  e_i32(j, TAG_INT);
  E(j, W_I32NE);
  e_brif(j, faildepth + 1);
  e_ldi64(j, o.ad);
  E(j, W_F64CVTI64S);
  e_end(j);
  e_lset(j, dloc);
}

/* push (o is an integer) as i32; constants fold */
static int e_isint (J *j, NOp o) {
  if (o.isk) return o.kint ? 1 : 0;  /* caller handles the folded case */
  e_ldtag(j, o.ad);
  e_i32(j, TAG_INT);
  E(j, W_I32EQ);
  return -1;
}

/*
** Arithmetic instruction followed by its MMBIN. Opens the "skip" block that
** the MMBIN instruction closes: a successful fast path branches out of it,
** past the metamethod call. 'iop' is the i64 opcode (0 = no integer
** path: float-only ops); 'fop' the f64 opcode or a negative special.
*/
#define FOP_POW		(-1)
#define FOP_MOD		(-2)
#define FOP_IDIV	(-3)
#define IOP_MOD		(-1)
#define IOP_IDIV	(-2)

static void e_arith (J *j, int pc, int a, NOp b, NOp c, int iop, int fop) {
  int bothint;
  e_block(j);  /* skip block, closed by the MMBIN */
  j->skipdepth = j->extra;
  /* integer path */
  if (iop != 0) {
    int ib = b.isk ? (b.kint ? 1 : 0) : -1;
    int ic = c.isk ? (c.kint ? 1 : 0) : -1;
    bothint = (ib != 0 && ic != 0);
    if (bothint) {
      int conds = 0;
      if (ib < 0) { e_isint(j, b); conds++; }
      if (ic < 0) { e_isint(j, c); conds++; }
      if (conds == 2) E(j, W_I32AND);
      if (conds) e_if(j);
      if (iop == IOP_MOD || iop == IOP_IDIV) {
        if (c.isk && c.ki != 0 && c.ki != -1) {
          /* luaV_mod / luaV_idiv with a constant divisor: no error case */
          e_nop_i64(j, b); e_lset(j, LV_X0);
          if (iop == IOP_MOD) {
            e_lget(j, LV_X0); e_i64(j, c.ki); E(j, W_I64REMS); e_lset(j, LV_X1);
            /* if (r != 0 && (r ^ n) < 0) r += n */
            e_lget(j, LV_X1); E(j, W_I64EQZ); E(j, W_I32EQZ);
            e_lget(j, LV_X1); e_i64(j, c.ki); E(j, W_I64XOR); e_i64(j, 0); E(j, W_I64LTS);
            E(j, W_I32AND);
            e_if(j);
            e_lget(j, LV_X1); e_i64(j, c.ki); E(j, W_I64ADD); e_lset(j, LV_X1);
            e_end(j);
          }
          else {
            e_lget(j, LV_X0); e_i64(j, c.ki); E(j, W_I64DIVS); e_lset(j, LV_X1);
            /* if ((m ^ n) < 0 && m % n != 0) q -= 1 */
            e_lget(j, LV_X0); e_i64(j, c.ki); E(j, W_I64XOR); e_i64(j, 0); E(j, W_I64LTS);
            e_lget(j, LV_X0); e_i64(j, c.ki); E(j, W_I64REMS); E(j, W_I64EQZ); E(j, W_I32EQZ);
            E(j, W_I32AND);
            e_if(j);
            e_lget(j, LV_X1); e_i64(j, 1); E(j, W_I64SUB); e_lset(j, LV_X1);
            e_end(j);
          }
        }
        else {
          e_savepc(j, pc + 1);
          e_lget(j, LV_L); e_lget(j, LV_CI);
          e_nop_i64(j, b); e_nop_i64(j, c);
          e_call(j, iop == IOP_MOD ? H_IMOD : H_IDIV);
          e_lset(j, LV_X1);
          e_reloadbase(j);
        }
      }
      else {
        e_nop_i64(j, b); e_nop_i64(j, c); E(j, iop); e_lset(j, LV_X1);
      }
      e_sti64_from(j, R(a), LV_X1);
      e_settag(j, R(a), TAG_INT);
      e_br(j, j->extra - j->skipdepth);
      if (conds) e_end(j);
    }
  }
  /* float path: both operands numbers */
  e_block(j);  /* mm: falls to the metamethod */
  e_tonum(j, b, LV_D0, 0);
  e_tonum(j, c, LV_D1, 0);
  e_adbase(j, R(a));
  if (fop == FOP_POW) {
    e_lget(j, LV_D0); e_lget(j, LV_D1); e_call(j, H_POW);
  }
  else if (fop == FOP_MOD) {
    e_lget(j, LV_D0); e_lget(j, LV_D1); e_call(j, H_FMOD);
  }
  else if (fop == FOP_IDIV) {
    e_lget(j, LV_D0); e_lget(j, LV_D1); E(j, W_F64DIV); E(j, W_F64FLOOR);
  }
  else {
    e_lget(j, LV_D0); e_lget(j, LV_D1); E(j, fop);
  }
  e_mem(j, W_F64STORE, 3, R(a).off);
  e_settag(j, R(a), TAG_FLT);
  e_br(j, j->extra - j->skipdepth);
  e_end(j);
  /* falls into the MMBIN instruction */
}

/* bitwise op followed by MMBIN: integers inline, the rest via helper */
static void e_bitwise (J *j, int op, int a, NOp b, NOp c) {
  int conds = 0;
  e_block(j);
  j->skipdepth = j->extra;
  if (!(b.isk && !b.kint) && !(c.isk && !c.kint)) {
    if (!b.isk) { e_isint(j, b); conds++; }
    if (!c.isk) { e_isint(j, c); conds++; }
    if (conds == 2) E(j, W_I32AND);
    if (conds) e_if(j);
    switch (op) {
      case OP_BAND: case OP_BANDK:
        e_nop_i64(j, b); e_nop_i64(j, c); E(j, W_I64AND); break;
      case OP_BOR: case OP_BORK:
        e_nop_i64(j, b); e_nop_i64(j, c); E(j, W_I64OR); break;
      case OP_BXOR: case OP_BXORK:
        e_nop_i64(j, b); e_nop_i64(j, c); E(j, W_I64XOR); break;
      case OP_SHL:
        e_nop_i64(j, b); e_nop_i64(j, c); e_call(j, H_SHIFTL); break;
      case OP_SHR:
        e_nop_i64(j, b); e_i64(j, 0); e_nop_i64(j, c); E(j, W_I64SUB);
        e_call(j, H_SHIFTL); break;
      case OP_SHRI:  /* luaV_shiftl(ib, -ic) */
        e_nop_i64(j, b); e_i64(j, -c.ki); e_call(j, H_SHIFTL); break;
      case OP_SHLI:  /* luaV_shiftl(ic, ib) */
        e_i64(j, c.ki); e_nop_i64(j, b); e_call(j, H_SHIFTL); break;
      default: lua_assert(0);
    }
    e_lset(j, LV_X1);
    e_sti64_from(j, R(a), LV_X1);
    e_settag(j, R(a), TAG_INT);
    e_br(j, j->extra - j->skipdepth);
    if (conds) e_end(j);
  }
  /* general case: helper decides (floats with integral values convert) */
  if (op == OP_SHRI || op == OP_SHLI) {
    e_i32(j, op); e_ptr(j, b.ad); e_i32(j, (int32_t)c.ki); e_ptr(j, R(a));
    e_call(j, H_BITOPI);
  }
  else {
    e_i32(j, op); e_ptr(j, b.ad); e_ptr(j, c.ad); e_ptr(j, R(a));
    e_call(j, H_BITOP);
  }
  e_brif(j, j->extra - j->skipdepth);
  /* falls into the MMBIN instruction */
}

/*
** Table read with a constant short-string key: hash lookup through the
** helper, copy on a hit, the full interpreter path otherwise.
*/
static void e_getstr (J *j, int pc, Ad t, const TValue *key, int a) {
  e_block(j);  /* done */
  e_block(j);  /* slow */
  e_ldtag(j, t); e_i32(j, TAG_TAB); E(j, W_I32NE); e_brif(j, 0);
  e_ldptr(j, t);
  e_i32(j, (int32_t)(uintptr_t)tsvalue(key));
  e_call(j, H_GETSHORTSTR);
  e_ltee(j, LV_T0 + 1);
  e_mem(j, W_I32LOAD8U, 0, TTOFF);
  e_i32(j, 0x0f); E(j, W_I32AND); E(j, W_I32EQZ); e_brif(j, 0);
  e_copy(j, R(a), LA(LV_T0 + 1));
  e_br(j, 1);
  e_end(j);
  e_savepc(j, pc + 1);
  e_lget(j, LV_L); e_lget(j, LV_CI); e_ptr(j, t); e_ptr(j, KA(key)); e_ptr(j, R(a));
  e_call(j, H_GET);
  e_reloadbase(j);
  e_end(j);
}

/* table write with a constant short-string key */
static void e_setstr (J *j, int pc, Ad t, const TValue *key, Ad v) {
  e_block(j);  /* done */
  e_block(j);  /* slow */
  e_ldtag(j, t); e_i32(j, TAG_TAB); E(j, W_I32NE); e_brif(j, 0);
  e_ldptr(j, t);
  e_ltee(j, LV_T0 + 2);
  e_i32(j, (int32_t)(uintptr_t)tsvalue(key));
  e_call(j, H_GETSHORTSTR);
  e_ltee(j, LV_T0 + 1);
  e_mem(j, W_I32LOAD8U, 0, TTOFF);
  e_i32(j, 0x0f); E(j, W_I32AND); E(j, W_I32EQZ); e_brif(j, 0);
  e_copy(j, LA(LV_T0 + 1), v);
  /* luaC_barrierback when storing a collectable value */
  e_ldtag(j, v); e_i32(j, BIT_ISCOLLECTABLE); E(j, W_I32AND);
  e_if(j);
  e_lget(j, LV_L); e_lget(j, LV_T0 + 2); e_ptr(j, v); e_call(j, H_BARRIERBACK);
  e_end(j);
  e_br(j, 1);
  e_end(j);
  e_savepc(j, pc + 1);
  e_lget(j, LV_L); e_lget(j, LV_CI); e_ptr(j, t); e_ptr(j, KA(key)); e_ptr(j, v);
  e_call(j, H_SET);
  e_reloadbase(j);
  e_end(j);
}

/*
** Integer-keyed read/write on the array part. 'key' is a register holding
** the key, or (kreg < 0) the constant 'kc'.
*/
static void e_arraykey (J *j, Ad t, int kreg, int kc) {
  /* leaves: T1 = Table*, T2 = slot address; branches 0 levels to 'slow' */
  e_ldtag(j, t); e_i32(j, TAG_TAB); E(j, W_I32NE); e_brif(j, 0);
  if (kreg >= 0) {
    e_ldtag(j, R(kreg)); e_i32(j, TAG_INT); E(j, W_I32NE); e_brif(j, 0);
    e_ldi64(j, R(kreg));
  }
  else
    e_i64(j, kc);
  e_i64(j, 1); E(j, W_I64SUB); e_lset(j, LV_X0);
  e_ldptr(j, t); e_lset(j, LV_T0 + 1);
  /* (lua_Unsigned)(k - 1) < alimit */
  e_lget(j, LV_X0);
  e_lget(j, LV_T0 + 1); e_mem(j, W_I32LOAD, 2, (uint32_t)offsetof(Table, alimit));
  E(j, W_I64EXTI32U);
  E(j, W_I64LTU); E(j, W_I32EQZ); e_brif(j, 0);
  e_lget(j, LV_T0 + 1); e_mem(j, W_I32LOAD, 2, (uint32_t)offsetof(Table, array));
  e_lget(j, LV_X0); E(j, W_I32WRAPI64); e_i32(j, 4); E(j, W_I32SHL);
  E(j, W_I32ADD);
  e_ltee(j, LV_T0 + 2);
  e_mem(j, W_I32LOAD8U, 0, TTOFF);
  e_i32(j, 0x0f); E(j, W_I32AND); E(j, W_I32EQZ); e_brif(j, 0);
}

static void e_getint (J *j, int pc, Ad t, int kreg, int kc, int a) {
  e_block(j);
  e_block(j);
  e_arraykey(j, t, kreg, kc);
  e_copy(j, R(a), LA(LV_T0 + 2));
  e_br(j, 1);
  e_end(j);
  e_savepc(j, pc + 1);
  e_lget(j, LV_L); e_lget(j, LV_CI); e_ptr(j, t);
  if (kreg >= 0) { e_ptr(j, R(kreg)); e_ptr(j, R(a)); e_call(j, H_GET); }
  else { e_i32(j, kc); e_ptr(j, R(a)); e_call(j, H_GETI); }
  e_reloadbase(j);
  e_end(j);
}

static void e_setint (J *j, int pc, Ad t, int kreg, int kc, Ad v) {
  e_block(j);
  e_block(j);
  e_arraykey(j, t, kreg, kc);
  e_copy(j, LA(LV_T0 + 2), v);
  e_ldtag(j, v); e_i32(j, BIT_ISCOLLECTABLE); E(j, W_I32AND);
  e_if(j);
  e_lget(j, LV_L); e_lget(j, LV_T0 + 1); e_ptr(j, v); e_call(j, H_BARRIERBACK);
  e_end(j);
  e_br(j, 1);
  e_end(j);
  e_savepc(j, pc + 1);
  e_lget(j, LV_L); e_lget(j, LV_CI); e_ptr(j, t);
  if (kreg >= 0) { e_ptr(j, R(kreg)); e_ptr(j, v); e_call(j, H_SET); }
  else { e_i32(j, kc); e_ptr(j, v); e_call(j, H_SETI); }
  e_reloadbase(j);
  e_end(j);
}

/* push the TValue* of upvalue 'b' into local 'loc' (and the UpVal* in uvloc) */
static void e_upval (J *j, int b, int loc, int uvloc) {
  e_lget(j, LV_CL);
  e_mem(j, W_I32LOAD, 2, (uint32_t)(offsetof(LClosure, upvals) + b * sizeof(UpVal *)));
  if (uvloc >= 0) e_ltee(j, uvloc);
  e_mem(j, W_I32LOAD, 2, (uint32_t)offsetof(UpVal, v));
  e_lset(j, loc);
}

/* conditional jump of a test instruction at 'pc': on the stack is
   (cond == k), i.e. "take the jump that follows" */
static void e_condjump (J *j, int pc) {
  Instruction ni = j->p->code[pc + 1];
  lua_assert(GET_OPCODE(ni) == OP_JMP);
  e_goto_if(j, pc + 2 + GETARG_sJ(ni));
  e_goto(j, pc + 2);
  j->dead = 1;
}

/* push (R[a] < / <= R[b]) with int/float fast paths, helper otherwise */
static void e_order (J *j, int pc, int a, int b, int lt) {
  e_blockt(j, T_I32);
  /* both integers */
  e_ldtag(j, R(a)); e_i32(j, TAG_INT); E(j, W_I32EQ);
  e_ldtag(j, R(b)); e_i32(j, TAG_INT); E(j, W_I32EQ);
  E(j, W_I32AND);
  e_if(j);
  e_ldi64(j, R(a)); e_ldi64(j, R(b)); E(j, lt ? W_I64LTS : W_I64LES);
  e_br(j, 1);
  e_end(j);
  /* both floats */
  e_ldtag(j, R(a)); e_i32(j, TAG_FLT); E(j, W_I32EQ);
  e_ldtag(j, R(b)); e_i32(j, TAG_FLT); E(j, W_I32EQ);
  E(j, W_I32AND);
  e_if(j);
  e_ldf64(j, R(a)); e_ldf64(j, R(b)); E(j, lt ? W_F64LT : W_F64LE);
  e_br(j, 1);
  e_end(j);
  e_savepc(j, pc + 1);
  e_lget(j, LV_L); e_lget(j, LV_CI); e_ptr(j, R(a)); e_ptr(j, R(b));
  e_call(j, lt ? H_LT : H_LE);
  e_reloadbase(j);
  e_end(j);
}

/* push (R[a] == R[b]) */
static void e_eq (J *j, int pc, int a, int b) {
  e_blockt(j, T_I32);
  e_ldtag(j, R(a)); e_ltee(j, LV_T0);
  e_ldtag(j, R(b));
  E(j, W_I32EQ);
  e_if(j);
  /* same variant tag */
  e_lget(j, LV_T0); e_i32(j, TAG_INT); E(j, W_I32EQ);
  e_if(j); e_ldi64(j, R(a)); e_ldi64(j, R(b)); E(j, W_I64EQ); e_br(j, 2); e_end(j);
  e_lget(j, LV_T0); e_i32(j, TAG_FLT); E(j, W_I32EQ);
  e_if(j); e_ldf64(j, R(a)); e_ldf64(j, R(b)); E(j, W_F64EQ); e_br(j, 2); e_end(j);
  e_lget(j, LV_T0); e_i32(j, TAG_SSTR); E(j, W_I32EQ);
  e_if(j); e_ldptr(j, R(a)); e_ldptr(j, R(b)); E(j, W_I32EQ); e_br(j, 2); e_end(j);
  /* nil, false, true: equal tags are equal values */
  e_lget(j, LV_T0); e_i32(j, LUA_VNIL); E(j, W_I32EQ);
  e_lget(j, LV_T0); e_i32(j, LUA_VFALSE); E(j, W_I32EQ); E(j, W_I32OR);
  e_lget(j, LV_T0); e_i32(j, LUA_VTRUE); E(j, W_I32EQ); E(j, W_I32OR);
  e_if(j); e_i32(j, 1); e_br(j, 2); e_end(j);
  e_end(j);
  e_savepc(j, pc + 1);
  e_lget(j, LV_L); e_lget(j, LV_CI); e_ptr(j, R(a)); e_ptr(j, R(b));
  e_call(j, H_EQ);
  e_reloadbase(j);
  e_end(j);
}

/* push (R[a] == K) for OP_EQK: raw equality */
static void e_eqk (J *j, int a, const TValue *k) {
  int kt = ttypetag(k);
  e_blockt(j, T_I32);
  e_ldtag(j, R(a)); e_ltee(j, LV_T0);
  e_i32(j, rawtt(k)); E(j, W_I32EQ);  /* full tag, collectable bit included */
  e_if(j);
  switch (kt) {
    case LUA_VNUMINT: e_ldi64(j, R(a)); e_i64(j, ivalue(k)); E(j, W_I64EQ); break;
    case LUA_VNUMFLT: e_ldf64(j, R(a)); e_f64(j, fltvalue(k)); E(j, W_F64EQ); break;
    case LUA_VSHRSTR:
      e_ldptr(j, R(a)); e_i32(j, (int32_t)(uintptr_t)tsvalue(k)); E(j, W_I32EQ); break;
    case LUA_VNIL: case LUA_VFALSE: case LUA_VTRUE: e_i32(j, 1); break;
    default:  /* long strings and anything else */
      e_ptr(j, R(a)); e_ptr(j, KA(k)); e_call(j, H_RAWEQUAL); break;
  }
  e_br(j, 1);
  e_end(j);
  /* different tags: equal only as int/float numbers */
  if (kt == LUA_VNUMINT || kt == LUA_VNUMFLT) {
    e_lget(j, LV_T0); e_i32(j, kt == LUA_VNUMINT ? TAG_FLT : TAG_INT); E(j, W_I32EQ);
    e_if(j);
    e_ptr(j, R(a)); e_ptr(j, KA(k)); e_call(j, H_RAWEQUAL);
    e_br(j, 1);
    e_end(j);
  }
  e_i32(j, 0);
  e_end(j);
}

/* push the result of OP_EQI/LTI/LEI/GTI/GEI */
static void e_cmpi (J *j, int pc, Instruction i) {
  int op = GET_OPCODE(i);
  int a = GETARG_A(i);
  int im = GETARG_sB(i);
  int iop, fop;
  switch (op) {
    case OP_EQI: iop = W_I64EQ; fop = W_F64EQ; break;
    case OP_LTI: iop = W_I64LTS; fop = W_F64LT; break;
    case OP_LEI: iop = W_I64LES; fop = W_F64LE; break;
    case OP_GTI: iop = W_I64GTS; fop = W_F64GT; break;
    default: iop = W_I64GES; fop = W_F64GE; break;
  }
  e_blockt(j, T_I32);
  e_ldtag(j, R(a)); e_ltee(j, LV_T0);
  e_i32(j, TAG_INT); E(j, W_I32EQ);
  e_if(j); e_ldi64(j, R(a)); e_i64(j, im); E(j, iop); e_br(j, 1); e_end(j);
  e_lget(j, LV_T0); e_i32(j, TAG_FLT); E(j, W_I32EQ);
  e_if(j); e_ldf64(j, R(a)); e_f64(j, cast_num(im)); E(j, fop); e_br(j, 1); e_end(j);
  if (op == OP_EQI)
    e_i32(j, 0);  /* other types cannot be equal to a number */
  else {
    TMS tm = (op == OP_LTI || op == OP_GTI) ? TM_LT : TM_LE;
    int inv = (op == OP_GTI || op == OP_GEI);
    e_savepc(j, pc + 1);
    e_lget(j, LV_L); e_lget(j, LV_CI); e_ptr(j, R(a));
    e_i32(j, im); e_i32(j, inv); e_i32(j, GETARG_C(i)); e_i32(j, tm);
    e_call(j, H_ORDERI);
    e_reloadbase(j);
  }
  e_end(j);
}

/* RK(C) operand of SET* instructions */
static Ad rkc (J *j, Instruction i) {
  return TESTARG_k(i) ? KA(j->p->k + GETARG_C(i)) : R(GETARG_C(i));
}

/* mark the leaders; returns 0 if the function cannot be compiled */
static int findleaders (J *j) {
  const Instruction *code = j->p->code;
  int n = j->n, pc;
  char *ld = (char *)calloc(n + 2, 1);
  if (ld == NULL) { j->err = "out of memory"; return 0; }
#define MARK(x)	do { int x_ = (x); if (x_ < 0 || x_ >= n) goto bad; ld[x_] = 1; } while (0)
  MARK(0);
  for (pc = 0; pc < n; pc++) {
    Instruction i = code[pc];
    switch (GET_OPCODE(i)) {
      case OP_JMP: MARK(pc + 1 + GETARG_sJ(i)); break;
      case OP_EQ: case OP_LT: case OP_LE: case OP_EQK: case OP_EQI:
      case OP_LTI: case OP_LEI: case OP_GTI: case OP_GEI:
      case OP_TEST: case OP_TESTSET:
        if (pc + 1 >= n || GET_OPCODE(code[pc + 1]) != OP_JMP) goto bad;
        MARK(pc + 2);
        MARK(pc + 2 + GETARG_sJ(code[pc + 1]));
        break;
      case OP_LFALSESKIP: MARK(pc + 2); break;
      case OP_FORPREP: MARK(pc + GETARG_Bx(i) + 2); MARK(pc + 1); break;
      case OP_FORLOOP: MARK(pc + 1 - GETARG_Bx(i)); MARK(pc + 1); break;
      case OP_TFORPREP: MARK(pc + 1 + GETARG_Bx(i)); break;
      case OP_TFORLOOP: MARK(pc + 1 - GETARG_Bx(i)); MARK(pc + 1); break;
      case OP_CALL: MARK(pc + 1); break;  /* resume point after a Lua call */
      case OP_ADD: case OP_SUB: case OP_MUL: case OP_MOD: case OP_POW:
      case OP_DIV: case OP_IDIV: case OP_BAND: case OP_BOR: case OP_BXOR:
      case OP_SHL: case OP_SHR: case OP_ADDI: case OP_ADDK: case OP_SUBK:
      case OP_MULK: case OP_MODK: case OP_POWK: case OP_DIVK: case OP_IDIVK:
      case OP_BANDK: case OP_BORK: case OP_BXORK: case OP_SHRI: case OP_SHLI:
        if (pc + 1 >= n) goto bad;
        switch (GET_OPCODE(code[pc + 1])) {
          case OP_MMBIN: case OP_MMBINI: case OP_MMBINK: break;
          default: goto bad;
        }
        break;
      case OP_LOADKX: case OP_NEWTABLE:
        if (pc + 1 >= n || GET_OPCODE(code[pc + 1]) != OP_EXTRAARG) goto bad;
        break;
      case OP_SETLIST:
        if (TESTARG_k(i) && (pc + 1 >= n || GET_OPCODE(code[pc + 1]) != OP_EXTRAARG))
          goto bad;
        break;
      default: break;
    }
  }
#undef MARK
  /* the instruction after an arith op (its MMBIN) must not be a leader */
  for (pc = 0; pc + 1 < n; pc++) {
    switch (GET_OPCODE(code[pc + 1])) {
      case OP_MMBIN: case OP_MMBINI: case OP_MMBINK:
        if (ld[pc + 1]) goto bad;
        break;
      default: break;
    }
  }
  j->nblk = 0;
  for (pc = 0; pc < n; pc++) {
    j->blk[pc] = ld[pc] ? j->nblk++ : -1;
  }
  free(ld);
  if (j->nblk > MAXJITBLOCKS) { j->err = "too many blocks"; return 0; }
  return 1;
 bad:
  free(ld);
  j->err = "unexpected bytecode shape";
  return 0;
}

static void e_instr (J *j, int pc) {
  Proto *p = j->p;
  Instruction i = p->code[pc];
  int a = GETARG_A(i);
  switch (GET_OPCODE(i)) {
    case OP_MOVE:
      e_copy(j, R(a), R(GETARG_B(i)));
      break;
    case OP_LOADI:
      e_adbase(j, R(a)); e_i64(j, GETARG_sBx(i)); e_mem(j, W_I64STORE, 3, R(a).off);
      e_settag(j, R(a), TAG_INT);
      break;
    case OP_LOADF:
      e_adbase(j, R(a)); e_f64(j, cast_num(GETARG_sBx(i))); e_mem(j, W_F64STORE, 3, R(a).off);
      e_settag(j, R(a), TAG_FLT);
      break;
    case OP_LOADK:
      e_copy(j, R(a), KA(p->k + GETARG_Bx(i)));
      break;
    case OP_LOADKX:
      e_copy(j, R(a), KA(p->k + GETARG_Ax(p->code[pc + 1])));
      break;
    case OP_LOADFALSE:
      e_settag(j, R(a), LUA_VFALSE);
      break;
    case OP_LFALSESKIP:
      e_settag(j, R(a), LUA_VFALSE);
      e_goto(j, pc + 2);
      j->dead = 1;
      break;
    case OP_LOADTRUE:
      e_settag(j, R(a), LUA_VTRUE);
      break;
    case OP_LOADNIL: {
      int b = GETARG_B(i), r;
      for (r = a; r <= a + b; r++) e_settag(j, R(r), LUA_VNIL);
      break;
    }
    case OP_GETUPVAL:
      e_upval(j, GETARG_B(i), LV_T0 + 1, -1);
      e_copy(j, R(a), LA(LV_T0 + 1));
      break;
    case OP_SETUPVAL:
      e_upval(j, GETARG_B(i), LV_T0 + 1, LV_T0 + 2);
      e_copy(j, LA(LV_T0 + 1), R(a));
      e_ldtag(j, R(a)); e_i32(j, BIT_ISCOLLECTABLE); E(j, W_I32AND);
      e_if(j);
      e_lget(j, LV_L); e_lget(j, LV_T0 + 2); e_ptr(j, R(a)); e_call(j, H_BARRIER);
      e_end(j);
      break;
    case OP_GETTABUP:
      e_upval(j, GETARG_B(i), LV_T0 + 3, -1);
      e_getstr(j, pc, LA(LV_T0 + 3), p->k + GETARG_C(i), a);
      break;
    case OP_GETTABLE:
      e_getint(j, pc, R(GETARG_B(i)), GETARG_C(i), 0, a);
      break;
    case OP_GETI:
      e_getint(j, pc, R(GETARG_B(i)), -1, GETARG_C(i), a);
      break;
    case OP_GETFIELD:
      e_getstr(j, pc, R(GETARG_B(i)), p->k + GETARG_C(i), a);
      break;
    case OP_SETTABUP:
      e_upval(j, a, LV_T0 + 3, -1);
      e_setstr(j, pc, LA(LV_T0 + 3), p->k + GETARG_B(i), rkc(j, i));
      break;
    case OP_SETTABLE:
      e_setint(j, pc, R(a), GETARG_B(i), 0, rkc(j, i));
      break;
    case OP_SETI:
      e_setint(j, pc, R(a), -1, GETARG_B(i), rkc(j, i));
      break;
    case OP_SETFIELD:
      e_setstr(j, pc, R(a), p->k + GETARG_B(i), rkc(j, i));
      break;
    case OP_NEWTABLE: {
      int b = GETARG_B(i);
      int c = GETARG_C(i);
      if (b > 0) b = 1 << (b - 1);
      if (TESTARG_k(i)) c += GETARG_Ax(p->code[pc + 1]) * (MAXARG_C + 1);
      e_savepc(j, pc + 2);
      e_lget(j, LV_L); e_lget(j, LV_CI); e_i32(j, a); e_i32(j, b); e_i32(j, c);
      e_call(j, H_NEWTABLE);
      e_reloadbase(j);
      break;
    }
    case OP_SELF: {
      Ad key = rkc(j, i);
      e_copy(j, R(a + 1), R(GETARG_B(i)));
      if (TESTARG_k(i) && ttisshrstring(p->k + GETARG_C(i)))
        e_getstr(j, pc, R(GETARG_B(i)), p->k + GETARG_C(i), a);
      else {
        e_savepc(j, pc + 1);
        e_lget(j, LV_L); e_lget(j, LV_CI); e_ptr(j, R(GETARG_B(i)));
        e_ptr(j, key); e_ptr(j, R(a));
        e_call(j, H_GET);
        e_reloadbase(j);
      }
      break;
    }
    case OP_ADDI:
      e_arith(j, pc, a, nreg(GETARG_B(i)), nimm(GETARG_sC(i)), W_I64ADD, W_F64ADD);
      break;
    case OP_ADDK:
      e_arith(j, pc, a, nreg(GETARG_B(i)), nk(p->k + GETARG_C(i)), W_I64ADD, W_F64ADD);
      break;
    case OP_SUBK:
      e_arith(j, pc, a, nreg(GETARG_B(i)), nk(p->k + GETARG_C(i)), W_I64SUB, W_F64SUB);
      break;
    case OP_MULK:
      e_arith(j, pc, a, nreg(GETARG_B(i)), nk(p->k + GETARG_C(i)), W_I64MUL, W_F64MUL);
      break;
    case OP_MODK:
      e_arith(j, pc, a, nreg(GETARG_B(i)), nk(p->k + GETARG_C(i)), IOP_MOD, FOP_MOD);
      break;
    case OP_POWK:
      e_arith(j, pc, a, nreg(GETARG_B(i)), nk(p->k + GETARG_C(i)), 0, FOP_POW);
      break;
    case OP_DIVK:
      e_arith(j, pc, a, nreg(GETARG_B(i)), nk(p->k + GETARG_C(i)), 0, W_F64DIV);
      break;
    case OP_IDIVK:
      e_arith(j, pc, a, nreg(GETARG_B(i)), nk(p->k + GETARG_C(i)), IOP_IDIV, FOP_IDIV);
      break;
    case OP_ADD:
      e_arith(j, pc, a, nreg(GETARG_B(i)), nreg(GETARG_C(i)), W_I64ADD, W_F64ADD);
      break;
    case OP_SUB:
      e_arith(j, pc, a, nreg(GETARG_B(i)), nreg(GETARG_C(i)), W_I64SUB, W_F64SUB);
      break;
    case OP_MUL:
      e_arith(j, pc, a, nreg(GETARG_B(i)), nreg(GETARG_C(i)), W_I64MUL, W_F64MUL);
      break;
    case OP_MOD:
      e_arith(j, pc, a, nreg(GETARG_B(i)), nreg(GETARG_C(i)), IOP_MOD, FOP_MOD);
      break;
    case OP_POW:
      e_arith(j, pc, a, nreg(GETARG_B(i)), nreg(GETARG_C(i)), 0, FOP_POW);
      break;
    case OP_DIV:
      e_arith(j, pc, a, nreg(GETARG_B(i)), nreg(GETARG_C(i)), 0, W_F64DIV);
      break;
    case OP_IDIV:
      e_arith(j, pc, a, nreg(GETARG_B(i)), nreg(GETARG_C(i)), IOP_IDIV, FOP_IDIV);
      break;
    case OP_BANDK: case OP_BORK: case OP_BXORK:
      e_bitwise(j, GET_OPCODE(i), a, nreg(GETARG_B(i)), nk(p->k + GETARG_C(i)));
      break;
    case OP_SHRI: case OP_SHLI:
      e_bitwise(j, GET_OPCODE(i), a, nreg(GETARG_B(i)), nimm(GETARG_sC(i)));
      break;
    case OP_BAND: case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR:
      e_bitwise(j, GET_OPCODE(i), a, nreg(GETARG_B(i)), nreg(GETARG_C(i)));
      break;
    case OP_MMBIN: {
      Instruction pi = p->code[pc - 1];
      e_savepc(j, pc + 1);
      e_lget(j, LV_L); e_lget(j, LV_CI); e_ptr(j, R(a)); e_ptr(j, R(GETARG_B(i)));
      e_ptr(j, R(GETARG_A(pi))); e_i32(j, GETARG_C(i));
      e_call(j, H_MMBIN);
      e_reloadbase(j);
      e_end(j);  /* the arith op's skip block */
      j->skipdepth = -1;
      break;
    }
    case OP_MMBINI: {
      Instruction pi = p->code[pc - 1];
      e_savepc(j, pc + 1);
      e_lget(j, LV_L); e_lget(j, LV_CI); e_ptr(j, R(a)); e_i32(j, GETARG_sB(i));
      e_i32(j, GETARG_k(i)); e_ptr(j, R(GETARG_A(pi))); e_i32(j, GETARG_C(i));
      e_call(j, H_MMBINI);
      e_reloadbase(j);
      e_end(j);
      j->skipdepth = -1;
      break;
    }
    case OP_MMBINK: {
      Instruction pi = p->code[pc - 1];
      e_savepc(j, pc + 1);
      e_lget(j, LV_L); e_lget(j, LV_CI); e_ptr(j, R(a)); e_ptr(j, KA(p->k + GETARG_B(i)));
      e_i32(j, GETARG_k(i)); e_ptr(j, R(GETARG_A(pi))); e_i32(j, GETARG_C(i));
      e_call(j, H_MMBINK);
      e_reloadbase(j);
      e_end(j);
      j->skipdepth = -1;
      break;
    }
    case OP_UNM: {
      int b = GETARG_B(i);
      e_block(j);
      e_ldtag(j, R(b)); e_ltee(j, LV_T0); e_i32(j, TAG_INT); E(j, W_I32EQ);
      e_if(j);
      e_i64(j, 0); e_ldi64(j, R(b)); E(j, W_I64SUB); e_lset(j, LV_X0);
      e_sti64_from(j, R(a), LV_X0); e_settag(j, R(a), TAG_INT);
      e_br(j, 1);
      e_end(j);
      e_lget(j, LV_T0); e_i32(j, TAG_FLT); E(j, W_I32EQ);
      e_if(j);
      e_ldf64(j, R(b)); E(j, W_F64NEG); e_lset(j, LV_D0);
      e_stf64_from(j, R(a), LV_D0); e_settag(j, R(a), TAG_FLT);
      e_br(j, 1);
      e_end(j);
      e_savepc(j, pc + 1);
      e_lget(j, LV_L); e_lget(j, LV_CI); e_ptr(j, R(b)); e_ptr(j, R(a));
      e_call(j, H_UNM);
      e_reloadbase(j);
      e_end(j);
      break;
    }
    case OP_BNOT: {
      int b = GETARG_B(i);
      e_block(j);
      e_ldtag(j, R(b)); e_i32(j, TAG_INT); E(j, W_I32EQ);
      e_if(j);
      e_ldi64(j, R(b)); e_i64(j, -1); E(j, W_I64XOR); e_lset(j, LV_X0);
      e_sti64_from(j, R(a), LV_X0); e_settag(j, R(a), TAG_INT);
      e_br(j, 1);
      e_end(j);
      e_savepc(j, pc + 1);
      e_lget(j, LV_L); e_lget(j, LV_CI); e_ptr(j, R(b)); e_ptr(j, R(a));
      e_call(j, H_BNOT);
      e_reloadbase(j);
      e_end(j);
      break;
    }
    case OP_NOT:
      e_adbase(j, R(a));
      e_isfalse(j, R(GETARG_B(i)));
      e_ift(j, T_I32);
      e_i32(j, LUA_VTRUE);
      e_else(j);
      e_i32(j, LUA_VFALSE);
      e_end(j);
      e_mem(j, W_I32STORE8, 0, R(a).off + TTOFF);
      break;
    case OP_LEN:
      e_savepc(j, pc + 1);
      e_lget(j, LV_L); e_lget(j, LV_CI); e_i32(j, a); e_i32(j, GETARG_B(i));
      e_call(j, H_LEN);
      e_reloadbase(j);
      break;
    case OP_CONCAT:
      e_savepc(j, pc + 1);
      e_lget(j, LV_L); e_lget(j, LV_CI); e_i32(j, a); e_i32(j, GETARG_B(i));
      e_call(j, H_CONCAT);
      e_reloadbase(j);
      break;
    case OP_CLOSE:
      e_savepc(j, pc + 1);
      e_lget(j, LV_L); e_lget(j, LV_CI); e_i32(j, a);
      e_call(j, H_CLOSE);
      e_reloadbase(j);
      e_hookcheck(j, pc + 1);
      break;
    case OP_TBC:
      e_savepc(j, pc + 1);
      e_lget(j, LV_L); e_lget(j, LV_CI); e_i32(j, a);
      e_call(j, H_TBC);
      e_reloadbase(j);
      break;
    case OP_JMP:
      e_goto(j, pc + 1 + GETARG_sJ(i));
      j->dead = 1;
      break;
    case OP_EQ:
      e_eq(j, pc, a, GETARG_B(i));
      if (!GETARG_k(i)) E(j, W_I32EQZ);
      e_condjump(j, pc);
      break;
    case OP_LT: case OP_LE:
      e_order(j, pc, a, GETARG_B(i), GET_OPCODE(i) == OP_LT);
      if (!GETARG_k(i)) E(j, W_I32EQZ);
      e_condjump(j, pc);
      break;
    case OP_EQK:
      e_eqk(j, a, p->k + GETARG_B(i));
      if (!GETARG_k(i)) E(j, W_I32EQZ);
      e_condjump(j, pc);
      break;
    case OP_EQI: case OP_LTI: case OP_LEI: case OP_GTI: case OP_GEI:
      e_cmpi(j, pc, i);
      if (!GETARG_k(i)) E(j, W_I32EQZ);
      e_condjump(j, pc);
      break;
    case OP_TEST:
      /* jump when (not l_isfalse) == k */
      e_isfalse(j, R(a));
      if (GETARG_k(i)) E(j, W_I32EQZ);
      e_condjump(j, pc);
      break;
    case OP_TESTSET: {
      /* if (l_isfalse(rb) == k) pc++; else { R[A] := R[B]; donextjump } */
      Instruction ni = p->code[pc + 1];
      int b = GETARG_B(i);
      e_isfalse(j, R(b));
      if (GETARG_k(i)) E(j, W_I32EQZ);  /* isfalse != k */
      e_if(j);
      e_copy(j, R(a), R(b));
      e_goto(j, pc + 2 + GETARG_sJ(ni));
      e_end(j);
      e_goto(j, pc + 2);
      j->dead = 1;
      break;
    }
    case OP_CALL:
      e_savepc(j, pc + 1);
      e_lget(j, LV_L); e_lget(j, LV_CI); e_i32(j, a); e_i32(j, GETARG_B(i));
      e_i32(j, GETARG_C(i) - 1);
      e_call(j, H_CALL);
      e_if(j);
      e_return(j, LUAJ_CALL);
      e_end(j);
      e_reloadbase(j);
      e_hookcheck(j, pc + 1);
      break;
    case OP_TAILCALL:
      e_savepc(j, pc + 1);
      e_lget(j, LV_L); e_lget(j, LV_CI); e_i32(j, a); e_i32(j, GETARG_B(i));
      e_i32(j, GETARG_C(i)); e_i32(j, GETARG_k(i));
      e_call(j, H_TAILCALL);
      E(j, W_RETURN);
      j->dead = 1;
      break;
    case OP_RETURN:
      e_savepc(j, pc + 1);
      e_lget(j, LV_L); e_lget(j, LV_CI); e_i32(j, a); e_i32(j, GETARG_B(i) - 1);
      e_i32(j, GETARG_C(i)); e_i32(j, GETARG_k(i));
      e_call(j, H_RETURN);
      e_return(j, LUAJ_RETURN);
      j->dead = 1;
      break;
    case OP_RETURN0: case OP_RETURN1:
      e_hookcheck(j, pc);  /* hooks: let the interpreter return */
      e_lget(j, LV_L); e_lget(j, LV_CI);
      if (GET_OPCODE(i) == OP_RETURN1) { e_i32(j, a); e_call(j, H_RETURN1); }
      else e_call(j, H_RETURN0);
      e_return(j, LUAJ_RETURN);
      j->dead = 1;
      break;
    case OP_FORLOOP: {
      int target = pc + 1 - GETARG_Bx(i);
      e_ldtag(j, R(a + 2)); e_i32(j, TAG_INT); E(j, W_I32EQ);
      e_if(j);
      /* integer loop: count = R[A+1] */
      e_ldi64(j, R(a + 1)); e_ltee(j, LV_X0);
      E(j, W_I64EQZ); E(j, W_I32EQZ);
      e_if(j);
      e_adbase(j, R(a + 1)); e_lget(j, LV_X0); e_i64(j, 1); E(j, W_I64SUB);
      e_mem(j, W_I64STORE, 3, R(a + 1).off);
      e_ldi64(j, R(a)); e_ldi64(j, R(a + 2)); E(j, W_I64ADD); e_lset(j, LV_X1);
      e_sti64_from(j, R(a), LV_X1);
      e_sti64_from(j, R(a + 3), LV_X1);
      e_settag(j, R(a + 3), TAG_INT);
      e_goto(j, target);
      e_end(j);
      e_else(j);
      e_ptr(j, R(a)); e_call(j, H_FLOATFORLOOP);
      e_goto_if(j, target);
      e_end(j);
      break;
    }
    case OP_FORPREP:
      e_savepc(j, pc + 1);
      e_lget(j, LV_L); e_lget(j, LV_CI); e_ptr(j, R(a));
      e_call(j, H_FORPREP);
      e_reloadbase(j);
      e_goto_if(j, pc + GETARG_Bx(i) + 2);
      break;
    case OP_TFORPREP:
      e_savepc(j, pc + 1);
      e_lget(j, LV_L); e_lget(j, LV_CI); e_ptr(j, R(a));
      e_call(j, H_TFORPREP);
      e_reloadbase(j);
      e_goto(j, pc + 1 + GETARG_Bx(i));
      j->dead = 1;
      break;
    case OP_TFORCALL:
      e_savepc(j, pc + 1);
      e_lget(j, LV_L); e_lget(j, LV_CI); e_i32(j, a); e_i32(j, GETARG_C(i));
      e_call(j, H_TFORCALL);
      e_reloadbase(j);
      e_hookcheck(j, pc + 1);
      break;
    case OP_TFORLOOP: {
      int target = pc + 1 - GETARG_Bx(i);
      e_ldtag(j, R(a + 4)); e_i32(j, 0x0f); E(j, W_I32AND);  /* !ttisnil */
      e_if(j);
      e_copy(j, R(a + 2), R(a + 4));
      e_goto(j, target);
      e_end(j);
      break;
    }
    case OP_SETLIST: {
      unsigned int last = GETARG_C(i);
      if (TESTARG_k(i)) last += GETARG_Ax(p->code[pc + 1]) * (MAXARG_C + 1);
      e_savepc(j, pc + 1 + (TESTARG_k(i) ? 1 : 0));
      e_lget(j, LV_L); e_lget(j, LV_CI); e_i32(j, a); e_i32(j, GETARG_B(i));
      e_i32(j, (int32_t)last);
      e_call(j, H_SETLIST);
      e_reloadbase(j);
      break;
    }
    case OP_CLOSURE:
      e_savepc(j, pc + 1);
      e_lget(j, LV_L); e_lget(j, LV_CI); e_i32(j, a);
      e_i32(j, (int32_t)(uintptr_t)p->p[GETARG_Bx(i)]);
      e_call(j, H_CLOSURE);
      e_reloadbase(j);
      break;
    case OP_VARARG:
      e_savepc(j, pc + 1);
      e_lget(j, LV_L); e_lget(j, LV_CI); e_i32(j, a); e_i32(j, GETARG_C(i) - 1);
      e_call(j, H_VARARG);
      e_reloadbase(j);
      break;
    case OP_VARARGPREP:
      e_savepc(j, pc + 1);
      e_lget(j, LV_L); e_lget(j, LV_CI); e_i32(j, a);
      e_call(j, H_VARARGPREP);
      e_reloadbase(j);
      break;
    case OP_EXTRAARG:
      break;  /* consumed by its owner; never reached on its own */
    default:
      e_bail(j, pc);
      j->dead = 1;
      break;
  }
}

/* emit the whole module for 'p' into 'M'; returns 1 on success */
static int emit_module (Proto *p, Buf *M, const char **err) {
  J j;
  Buf S, F;
  int pc, h;
  memset(&j, 0, sizeof(j));
  memset(&S, 0, sizeof(S));
  memset(&F, 0, sizeof(F));
  j.p = p;
  j.n = p->sizecode;
  j.skipdepth = -1;
  if (j.n <= 0 || j.n > MAXJITCODE) { *err = "function too large"; return 0; }
  j.blk = (int *)malloc(sizeof(int) * (size_t)j.n);
  if (j.blk == NULL) { *err = "out of memory"; return 0; }
  if (!findleaders(&j)) { *err = j.err; free(j.blk); return 0; }

  /* function body */
  b1(&j.c, 3);  /* local groups */
  bu(&j.c, 2 + NI32TMP); b1(&j.c, T_I32);
  bu(&j.c, NI64TMP); b1(&j.c, T_I64);
  bu(&j.c, NF64TMP); b1(&j.c, T_F64);
  /* prologue: base, closure */
  e_reloadbase(&j);
  e_lget(&j, LV_CI);
  e_mem(&j, W_I32LOAD, 2, (uint32_t)offsetof(CallInfo, func));
  e_mem(&j, W_I32LOAD, 2, 0);  /* the LClosure* in the function's slot */
  e_lset(&j, LV_CL);
  /* dispatch: loop { block_{n-1} ... block_0 { block { br_table } } } */
  E(&j, W_LOOP); E(&j, T_VOID);
  for (h = 0; h < j.nblk; h++) { E(&j, W_BLOCK); E(&j, T_VOID); }
  E(&j, W_BLOCK); E(&j, T_VOID);
  e_lget(&j, LV_PC);
  E(&j, W_BRTABLE);
  bu(&j.c, j.n);
  for (pc = 0; pc < j.n; pc++)
    bu(&j.c, j.blk[pc] >= 0 ? (uint64_t)j.blk[pc] + 1 : 0);
  bu(&j.c, 0);
  E(&j, W_END);
  e_return(&j, LUAJ_NOENTRY);
  E(&j, W_END);  /* end of block_0: code of block 0 follows */
  j.cur = 0;
  j.extra = 0;
  for (pc = 0; pc < j.n; pc++) {
    if (pc > 0 && j.blk[pc] >= 0) {
      if (j.extra != 0) { *err = "internal: open construct at leader"; goto fail; }
      E(&j, W_END);
      j.cur = j.blk[pc];
      j.dead = 0;
    }
    if (j.dead) continue;
    e_instr(&j, pc);
  }
  if (j.extra != 0) { *err = "internal: open construct at end"; goto fail; }
  E(&j, W_END);  /* loop */
  E(&j, W_UNREACHABLE);
  E(&j, W_END);  /* function */
  if (j.c.oom) { *err = "out of memory"; goto fail; }

  /* module */
  {
    static const unsigned char hdr[8] = {0, 'a', 's', 'm', 1, 0, 0, 0};
    bmem(M, hdr, 8);
  }
  /* types: 0 = the compiled function, 1 + h = helper h */
  S.n = 0;
  bu(&S, 1 + H__N);
  b1(&S, 0x60); bu(&S, 3); b1(&S, T_I32); b1(&S, T_I32); b1(&S, T_I32);
  bu(&S, 1); b1(&S, T_I32);
  for (h = 0; h < H__N; h++) {
    const char *s = helpers[h].sig;
    const char *colon = strchr(s, ':');
    int np = (int)(colon - s), nr = (int)strlen(colon + 1), k;
    b1(&S, 0x60);
    bu(&S, np);
    for (k = 0; k < np; k++) b1(&S, s[k] == 'i' ? T_I32 : s[k] == 'l' ? T_I64 : T_F64);
    bu(&S, nr);
    for (k = 0; k < nr; k++) {
      char c = colon[1 + k];
      b1(&S, c == 'i' ? T_I32 : c == 'l' ? T_I64 : T_F64);
    }
  }
  bsection(M, 1, &S);
  /* imports: helpers, then memory */
  S.n = 0;
  bu(&S, H__N + 1);
  for (h = 0; h < H__N; h++) {
    bname(&S, "cart"); bname(&S, helpers[h].name);
    b1(&S, 0x00); bu(&S, 1 + h);
  }
  bname(&S, "cart"); bname(&S, "memory");
  b1(&S, 0x02); b1(&S, 0x00); bu(&S, 0);
  bsection(M, 2, &S);
  /* function section */
  S.n = 0;
  bu(&S, 1); bu(&S, 0);
  bsection(M, 3, &S);
  /* export "f" */
  S.n = 0;
  bu(&S, 1); bname(&S, "f"); b1(&S, 0x00); bu(&S, H__N);
  bsection(M, 7, &S);
  /* code */
  S.n = 0;
  bu(&S, 1);
  bu(&S, j.c.n);
  bmem(&S, j.c.b, j.c.n);
  bsection(M, 10, &S);
  free(S.b);
  free(j.c.b);
  free(j.blk);
  if (M->oom) { *err = "out of memory"; return 0; }
  return 1;
 fail:
  free(S.b);
  free(j.c.b);
  free(j.blk);
  return 0;
}

/* }================================================================== */


/* {==================================================================
** 5. Compile / enter / trap
** =================================================================== */

typedef int (*JitFn) (lua_State *L, CallInfo *ci, int pc);

static void compile (lua_State *L, Proto *p) {
  Buf M;
  const char *err = NULL;
  int cell, r;
  (void)L;
  memset(&M, 0, sizeof(M));
  if (!emit_module(p, &M, &err)) {
    p->jitslot = LUAJ_FAILED;
    jst.rejected++;
    jst.lasterr = err;
    free(M.b);
    return;
  }
  cell = cell_alloc(p);
  if (cell < 0) {  /* too many compiles in flight: try again later */
    p->jitcount = 0;
    free(M.b);
    return;
  }
  jst.bytes += M.n;
  r = wc_jit_link(M.b, (unsigned int)M.n, &jit_cells[cell]);
  free(M.b);
  if (r > 0) {
    p->jitslot = r;
    jit_cellused[cell] = 0;
    jst.compiled++;
    jst.modules++;
  }
  else if (r == -2) {
    p->jitslot = LUAJ_PENDING;
    p->jitcell = cell;
    jst.pending++;
  }
  else {
    jit_cellused[cell] = 0;
    p->jitslot = LUAJ_FAILED;
    jst.failed++;
    jst.lasterr = "host refused the module";
    if (jst.compiled == 0 && jst.pending == 0)
      jit_state = -1;  /* the host does not link: stop trying */
  }
}

int luaJ_hot (lua_State *L, Proto *p, const Instruction *pc) {
  if (p->jitslot == LUAJ_COLD) {
    if (pc != p->code) return 0;  /* count calls only at entry */
    if (jit_state <= 0) {
      if (jit_state == 0) jit_init();
      if (jit_state < 0) { p->jitslot = LUAJ_FAILED; return 0; }
    }
    if (++p->jitcount <= luaJ_threshold) return 0;
    compile(L, p);
  }
  else if (p->jitslot == LUAJ_PENDING) {
    int v = jit_cells[p->jitcell];
    if (v == -2) return 0;
    jit_cellused[p->jitcell] = 0;
    p->jitcell = -1;
    jst.pending--;
    if (v > 0) { p->jitslot = v; jst.compiled++; jst.modules++; }
    else { p->jitslot = LUAJ_FAILED; jst.failed++; }
  }
  return p->jitslot > 0;
}

int luaJ_enter (lua_State *L, CallInfo *ci, const Instruction *pc) {
  Proto *p = ci_func(ci)->p;
  int r = ((JitFn)(uintptr_t)p->jitslot)(L, ci, (int)(pc - p->code));
  if (r == LUAJ_BAIL) jst.bails++;
  return r;
}

/*
** Called from the interpreter's fetch when 'trap' is set and no hooks are
** active. 0: nothing to do (trap cleared); 1: enter compiled code at 'pc';
** 2: keep 'trap' (this is the instruction compiled code bailed at).
*/
int luaJ_trap (lua_State *L, CallInfo *ci, const Instruction *pc) {
  Proto *p = ci_func(ci)->p;
  if (pc == L->jitbail) return 2;
  L->jitbail = NULL;
  ci->u.l.trap = 0;
  if (p->jitslot <= 0) {
    if (p->jitslot == LUAJ_COLD) {
      if (jit_state == 0) jit_init();
      if (jit_state < 0) { p->jitslot = LUAJ_FAILED; return 0; }
      if (p->jitcount <= luaJ_threshold) return 0;
      compile(L, p);
    }
    else if (p->jitslot == LUAJ_PENDING)
      luaJ_hot(L, p, pc);
    if (p->jitslot <= 0) return 0;
  }
  return 1;
}

void luaJ_freeproto (lua_State *L, Proto *p) {
  (void)L;
  if (p->jitslot == LUAJ_PENDING && p->jitcell >= 0) {
    jit_cellowner[p->jitcell] = NULL;  /* the host may still answer */
    jst.pending--;
  }
  /* a compiled function's table slot stays allocated (see STATUS.md) */
}

/* }================================================================== */


/* {==================================================================
** 6. Lua control library (exposed as wc.jit)
** =================================================================== */

static Proto *argproto (lua_State *L, int arg) {
  const TValue *o = s2v(L->ci->func.p + arg);
  luaL_argcheck(L, lua_type(L, arg) == LUA_TFUNCTION && ttisLclosure(o), arg,
                "Lua function expected");
  return clLvalue(o)->p;
}

static int jl_threshold (lua_State *L) {
  if (!lua_isnoneornil(L, 1)) {
    lua_Integer n = luaL_checkinteger(L, 1);
    luaL_argcheck(L, n >= 0 && n < 0x7fffffff, 1, "out of range");
    luaJ_threshold = (int)n;
    jit_userthreshold = (int)n;
  }
  lua_pushinteger(L, luaJ_threshold);
  return 1;
}

static int jl_enabled (lua_State *L) {
  if (jit_state == 0) jit_init();
  lua_pushboolean(L, jit_state > 0);
  return 1;
}

static int jl_stats (lua_State *L) {
  lua_createtable(L, 0, 8);
#define F(name, v) (lua_pushinteger(L, (lua_Integer)(v)), lua_setfield(L, -2, name))
  F("compiled", jst.compiled);
  F("failed", jst.failed);
  F("rejected", jst.rejected);
  F("modules", jst.modules);
  F("bytes", jst.bytes);
  F("bails", jst.bails);
  F("pending", jst.pending);
  F("threshold", luaJ_threshold);
#undef F
  lua_pushstring(L, jst.lasterr ? jst.lasterr : "");
  lua_setfield(L, -2, "lasterr");
  return 1;
}

static int jl_status (lua_State *L) {
  Proto *p = argproto(L, 1);
  const char *s = p->jitslot > 0 ? "compiled" : p->jitslot == LUAJ_COLD ? "cold"
                : p->jitslot == LUAJ_PENDING ? "pending" : "failed";
  lua_pushstring(L, s);
  lua_pushinteger(L, p->jitcount);
  return 2;
}

/* compile 'f' now (ignoring hotness); returns its status */
static int jl_compile (lua_State *L) {
  Proto *p = argproto(L, 1);
  if (jit_state == 0) jit_init();
  if (p->jitslot == LUAJ_COLD && jit_state > 0) compile(L, p);
  return jl_status(L);
}

/* the module bytes 'f' compiles to, without installing them */
static int jl_dump (lua_State *L) {
  Proto *p = argproto(L, 1);
  Buf M;
  const char *err = NULL;
  memset(&M, 0, sizeof(M));
  if (!emit_module(p, &M, &err)) {
    free(M.b);
    luaL_pushfail(L);
    lua_pushstring(L, err ? err : "?");
    return 2;
  }
  lua_pushlstring(L, (const char *)M.b, M.n);
  free(M.b);
  return 1;
}

static const luaL_Reg jitlib[] = {
  {"threshold", jl_threshold},
  {"enabled", jl_enabled},
  {"stats", jl_stats},
  {"status", jl_status},
  {"compile", jl_compile},
  {"dump", jl_dump},
  {NULL, NULL}
};

int luaopen_wcljit (lua_State *L) {
  luaL_newlib(L, jitlib);
  return 1;
}

/* }================================================================== */
