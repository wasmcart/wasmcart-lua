/* three_c.c: three.c (the three.js r128 API in C) bound as the Lua global
 * `t3`, for three.lua.
 *
 * three.lua keeps the three.js API in Lua; this is where its scene graph,
 * matrices, culling and drawing actually run. The shape of the binding is the
 * point (see internal C-REWRITE-INVESTIGATION): Lua tables crossing the
 * boundary per value lose to plain Lua, so the data lives here and Lua only
 * pushes what a cart changed, a whole object at a time:
 *
 *   t3.trs(h, px,py,pz, qx,qy,qz,qw, sx,sy,sz)   one call per moved object
 *   t3.update_world(scene)                       the whole walk, in C
 *   t3.render(renderer, scene, camera)           cull, sort, draw, in C
 *
 * and reads back only what a cart asks for (t3.world(h, elements) fills a
 * Matrix4's elements when Lua code touches matrixWorld).
 *
 * Handles are full userdata boxes holding one three.c reference each; their
 * __gc drops it, so a three.lua object that is collected frees its mirror.
 * The functions take the box with lua_touserdata and no type check: the only
 * caller is three.lua's own backend module.
 *
 * GL: three.c renders with the same `gl` imports as the engine, into the
 * target the engine is drawing to. The engine's 2D batches are flushed
 * first, and its cached GL state is re-established afterwards
 * (wcl_r2d__after_external_gl), so a cart can draw a 2D HUD over a three.c
 * frame in the same love.draw. */
#ifdef WCL_THREE_C

#include "lua.h"
#include "lauxlib.h"
/* Lua internals, for the hot readers below: the engine links its own pinned
 * Lua (5.4.7), so a table's slots can be read in place instead of one
 * lua_rawgeti / lua_getfield API call per number (the API's per-call
 * overhead was most of a frame's sync in the profile). */
#include "lobject.h"
#include "lstate.h"
#include "ltable.h"
#include "lgc.h"
#include "lvm.h"
#include "three.h"
#include "wasmcart.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

void wcl_r2d__flush_for_external_gl(void);
void wcl_r2d__after_external_gl(void);
unsigned wcl_r2d__after_external(int fbo, int vp_known, int vx, int vy, int vw, int vh,
                                 int active_unit, int vao, int blend_func_set, int scissor,
                                 int depth_test, int cull, float clear_r, float clear_g, float clear_b,
                                 int clear_known);
/* gl_hooks.h / render2d_gl.c: whether the engine issued GL since the last
 * hand-back, the frame's deferred clear, and whether that clear changed the
 * clear colour */
extern int wcl_gl_touched, wcl_clear_pending, wcl_clear_color_touched;
void wcl__run_pending_clear(void);
void wcl__drop_pending_clear(void);

/* what the last hand-back changed in GL (T3_FORGET_* bits): forgotten by
 * three.c at the next render when the engine did nothing else meanwhile */
static unsigned forget_next = ~0u;

/* hand GL back to the engine, putting back only what the renderer changed */
static void hand_back(t3_renderer *r) {
    t3_gl_state s;
    t3_renderer_gl_state(r, &s);
    unsigned ch = wcl_r2d__after_external(s.fbo, s.vp_known, s.vp_x, s.vp_y, s.vp_w, s.vp_h, s.active_unit, s.vao,
                                          s.blend_func_set, s.scissor, s.depth_test, s.cull,
                                          s.clear[0], s.clear[1], s.clear[2], s.clear_set && s.clear[3] == 1.0f);
    forget_next = (ch & 1u ? T3_FORGET_FBO : 0) | (ch & 2u ? T3_FORGET_VIEWPORT : 0) | (ch & 4u ? T3_FORGET_VAO : 0) |
                  (ch & 8u ? T3_FORGET_UNIT : 0) | (ch & 16u ? T3_FORGET_CAPS : 0) | (ch & 32u ? T3_FORGET_SCISSOR : 0);
    /* the hand-back's own GL is accounted for above */
    wcl_gl_touched = 0;
}

/* Before three.c draws: the frame's deferred clear (dropped when three.c
 * clears the whole target anyway), then what three.c must forget: all of it
 * if the engine issued GL since, else only what the hand-back changed. */
static void take_over(t3_renderer *r) {
    if (wcl_clear_pending) {
        if (t3_renderer_clears_target(r)) wcl__drop_pending_clear();
        else wcl__run_pending_clear();
    }
    if (wcl_gl_touched) {
        t3_renderer_reset_bindings(r);
    } else {
        unsigned f = forget_next;
        if (wcl_clear_color_touched) f |= T3_FORGET_CLEAR_COLOR;
        t3_renderer_forget(r, f);
    }
    wcl_clear_color_touched = 0;
}
void wcl_r2d__current_target(unsigned *fbo, int *x, int *y, int *w, int *h);
int wcl_r2d_active(void);

#define HANDLE_MT "t3.handle"

typedef struct { void *p; } box_t;

/* ── direct table reads ──────────────────────────────────────────────── */

static Table *tab_at(lua_State *L, int idx) { return (Table *)lua_topointer(L, idx); }
static double tv_num(const TValue *v) {
    if (ttisfloat(v)) return fltvalue(v);
    if (ttisinteger(v)) return (double)ivalue(v);
    return 0;
}
/* t[i] for 1-based i: the array part directly, else the hash part */
static const TValue *tab_geti(Table *t, int i) {
    if ((unsigned)(i - 1) < t->alimit) return &t->array[i - 1];
    return luaH_getint(t, i);
}
static double tab_num(Table *t, int i) { return tv_num(tab_geti(t, i)); }
static Table *tv_tab(const TValue *v) { return ttistable(v) ? hvalue(v) : NULL; }
static void *tv_handle(const TValue *v) {
    if (!ttisfulluserdata(v)) return NULL;
    return ((box_t *)getudatamem(uvalue(v)))->p;
}
static int tv_true(const TValue *v) { return !l_isfalse(v); }

static void *P(lua_State *L, int i) {
    box_t *b = (box_t *)lua_touserdata(L, i);
    return b ? b->p : NULL;
}
static void *Popt(lua_State *L, int i) {
    return lua_isnoneornil(L, i) ? NULL : P(L, i);
}
/* Wrap a reference the caller owns (fresh from a *_new). */
static int push_owned(lua_State *L, void *p) {
    if (!p) { lua_pushnil(L); return 1; }
    box_t *b = (box_t *)lua_newuserdatauv(L, sizeof(box_t), 0);
    b->p = p;
    luaL_setmetatable(L, HANDLE_MT);
    return 1;
}
static int handle_gc(lua_State *L) {
    box_t *b = (box_t *)lua_touserdata(L, 1);
    if (b && b->p) { t3_release(b->p); b->p = NULL; }
    return 0;
}

#define F(i) ((float)lua_tonumber(L, (i)))
#define I(i) ((int)lua_tointeger(L, (i)))
#define B(i) (lua_toboolean(L, (i)) != 0)

static t3_color color_at(lua_State *L, int i) {
    t3_color c = { F(i), F(i + 1), F(i + 2) };
    return c;
}

/* ── objects ─────────────────────────────────────────────────────────── */

/* t3.object(kind): "object", "group", "scene", "bone" */
static int l_object(lua_State *L) {
    const char *k = luaL_optstring(L, 1, "object");
    void *o;
    if (!strcmp(k, "scene")) o = t3_scene_new();
    else if (!strcmp(k, "group")) o = t3_group_new();
    else if (!strcmp(k, "bone")) o = t3_bone_new();
    else o = t3_object_new();
    return push_owned(L, o);
}

/* t3.mesh(kind, geometry, material): "mesh", "line", "linesegments", "lineloop" */
static int l_mesh(lua_State *L) {
    const char *k = luaL_checkstring(L, 1);
    t3_geometry *g = (t3_geometry *)P(L, 2);
    t3_material *m = (t3_material *)P(L, 3);
    void *o;
    if (!strcmp(k, "line")) o = t3_line_new(g, m);
    else if (!strcmp(k, "linesegments")) o = t3_line_segments_new(g, m);
    else if (!strcmp(k, "lineloop")) o = t3_line_loop_new(g, m);
    else o = t3_mesh_new(g, m);
    return push_owned(L, o);
}

/* t3.instanced(geometry, material, count) */
static int l_instanced(lua_State *L) {
    return push_owned(L, t3_instanced_mesh_new((t3_geometry *)P(L, 1), (t3_material *)P(L, 2), I(3)));
}

/* t3.mesh_set(mesh, geometry, material [, material2, ...]): swap what a mesh
 * draws (mesh.geometry = g, mesh.material = m or an array). */
static int l_mesh_set(lua_State *L) {
    t3_mesh *me = (t3_mesh *)P(L, 1);
    t3_geometry *g = (t3_geometry *)P(L, 2);
    int n = lua_gettop(L) - 2;
    if (g != me->geometry) {
        t3_retain(g);
        t3_release(me->geometry);
        me->geometry = g;
    }
    if (n < 1) return 0;
    t3_material **ms = (t3_material **)malloc(sizeof(*ms) * (size_t)n);
    for (int i = 0; i < n; i++) ms[i] = (t3_material *)t3_retain(P(L, 3 + i));
    for (int i = 0; i < me->material_count; i++) t3_release(me->materials[i]);
    free(me->materials);
    me->materials = ms;
    me->material_count = n;
    return 0;
}

static int l_add(lua_State *L) { t3_object_add((t3_object *)P(L, 1), P(L, 2)); return 0; }
static int l_remove(lua_State *L) { t3_object_remove((t3_object *)P(L, 1), P(L, 2)); return 0; }

/* t3.trs(h, px,py,pz, qx,qy,qz,qw, sx,sy,sz): the local transform, as
 * Object3D keeps it (position, quaternion, scale). three.c composes it only
 * when it changed bitwise. */
#define D(i) lua_tonumber(L, (i))
static int l_trs(lua_State *L) {
    double p[3] = { D(2), D(3), D(4) }, q[4] = { D(5), D(6), D(7), D(8) }, sc[3] = { D(9), D(10), D(11) };
    t3_object_set_trs_d(P(L, 1), p, q, sc);
    return 0;
}

/* Quaternion.setFromEuler, as three.js computes it: in double precision,
 * stored as float. Order: t3_euler_order (XYZ YXZ ZXY ZYX YZX XZY). */
static void quat_from_euler_d(double x, double y, double z, int order, double *out) {
    double c1 = cos(x * 0.5), c2 = cos(y * 0.5), c3 = cos(z * 0.5);
    double s1 = sin(x * 0.5), s2 = sin(y * 0.5), s3 = sin(z * 0.5);
    double qx, qy, qz, qw;
    switch (order) {
    default:
    case T3_XYZ:
        qx = s1 * c2 * c3 + c1 * s2 * s3; qy = c1 * s2 * c3 - s1 * c2 * s3;
        qz = c1 * c2 * s3 + s1 * s2 * c3; qw = c1 * c2 * c3 - s1 * s2 * s3; break;
    case T3_YXZ:
        qx = s1 * c2 * c3 + c1 * s2 * s3; qy = c1 * s2 * c3 - s1 * c2 * s3;
        qz = c1 * c2 * s3 - s1 * s2 * c3; qw = c1 * c2 * c3 + s1 * s2 * s3; break;
    case T3_ZXY:
        qx = s1 * c2 * c3 - c1 * s2 * s3; qy = c1 * s2 * c3 + s1 * c2 * s3;
        qz = c1 * c2 * s3 + s1 * s2 * c3; qw = c1 * c2 * c3 - s1 * s2 * s3; break;
    case T3_ZYX:
        qx = s1 * c2 * c3 - c1 * s2 * s3; qy = c1 * s2 * c3 + s1 * c2 * s3;
        qz = c1 * c2 * s3 - s1 * s2 * c3; qw = c1 * c2 * c3 + s1 * s2 * s3; break;
    case T3_YZX:
        qx = s1 * c2 * c3 + c1 * s2 * s3; qy = c1 * s2 * c3 + s1 * c2 * s3;
        qz = c1 * c2 * s3 - s1 * s2 * c3; qw = c1 * c2 * c3 - s1 * s2 * s3; break;
    case T3_XZY:
        qx = s1 * c2 * c3 - c1 * s2 * s3; qy = c1 * s2 * c3 - s1 * c2 * s3;
        qz = c1 * c2 * s3 + s1 * s2 * c3; qw = c1 * c2 * c3 + s1 * s2 * s3; break;
    }
    out[0] = qx; out[1] = qy; out[2] = qz; out[3] = qw;
}

/* t3.trs_euler(h, px,py,pz, ex,ey,ez, order, sx,sy,sz): the local transform
 * with the rotation as Euler angles, converted here (three.lua leaves its
 * quaternion uncomputed until Lua reads it). */
static int l_trs_euler(lua_State *L) {
    t3_object *o = (t3_object *)P(L, 1);
    int order = I(8);
    o->rotation.x = F(5); o->rotation.y = F(6); o->rotation.z = F(7);
    o->rotation.order = (t3_euler_order)order;
    double p[3] = { D(2), D(3), D(4) }, q[4], sc[3] = { D(9), D(10), D(11) };
    quat_from_euler_d(D(5), D(6), D(7), order, q);
    t3_object_set_trs_d(o, p, q, sc);
    return 0;
}

/* Euler order name ("XYZ", ...) to t3_euler_order */
static int order_of(const char *s) {
    if (!s || s[0] == 'X') return (s && s[1] == 'Z') ? T3_XZY : T3_XYZ;
    if (s[0] == 'Y') return s[1] == 'X' ? T3_YXZ : T3_YZX;
    return s[1] == 'X' ? T3_ZXY : T3_ZYX;
}

/* Euler's field names, interned once (lua_getfield would re-intern its key
 * string on every call, which profiled as the largest cost of a flush) */
enum { K_X, K_Y, K_Z, K_ORDER, K_COUNT };
static int key_ref[K_COUNT];
static double slot_num(lua_State *L, int t, int i) {
    lua_rawgeti(L, t, i);
    double v = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return v;
}
static void push_key_field(lua_State *L, int t, int k) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, key_ref[k]);
    lua_rawget(L, t);
}
static double key_num(lua_State *L, int t, int k) {
    push_key_field(L, t, k);
    double v = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return v;
}

/* t3.flush(list, n) -> m: push the local transforms of the n dirty records
 * in list (three.lua's _t3.lua: {handle, position, scale, quaternion,
 * rotation, object, dirty, matrixAutoUpdate, flagsWritten, matrixWritten}),
 * reading their tables here instead of through n Lua calls. Records that
 * also need Lua (flags or a hand-set matrix written) are kept, compacted to
 * the front of the list, and their count returned; the rest are cleared. */
static TString *key_ts[K_COUNT];
static int l_flush(lua_State *L) {
    int n = I(2), keep = 0;
    Table *list = tab_at(L, 1);
    for (int i = 1; i <= n; i++) {
        Table *rec = tv_tab(tab_geti(list, i));
        if (!rec) continue;
        /* rec[7] = false: off the list (a boolean, no GC barrier needed) */
        if (rec->alimit >= 7) setbfvalue(&rec->array[6]);
        t3_object *o = (t3_object *)tv_handle(tab_geti(rec, 1));
        int mau = tv_true(tab_geti(rec, 8));
        if (mau && o) {
            double pd[3], qd[4], sd[3];
            Table *p = tv_tab(tab_geti(rec, 2)), *sc = tv_tab(tab_geti(rec, 3)), *q = tv_tab(tab_geti(rec, 4));
            if (p && sc && q) {
                pd[0] = tab_num(p, 1); pd[1] = tab_num(p, 2); pd[2] = tab_num(p, 3);
                sd[0] = tab_num(sc, 1); sd[1] = tab_num(sc, 2); sd[2] = tab_num(sc, 3);
                if (tv_true(tab_geti(q, 5))) {               /* quaternion stale: from the Euler */
                    Table *r = tv_tab(tab_geti(rec, 5));
                    double ex = tv_num(luaH_getshortstr(r, key_ts[K_X]));
                    double ey = tv_num(luaH_getshortstr(r, key_ts[K_Y]));
                    double ez = tv_num(luaH_getshortstr(r, key_ts[K_Z]));
                    const TValue *ov = luaH_getshortstr(r, key_ts[K_ORDER]);
                    int order = order_of(ttisstring(ov) ? getstr(tsvalue(ov)) : NULL);
                    o->rotation.x = (float)ex; o->rotation.y = (float)ey; o->rotation.z = (float)ez;
                    o->rotation.order = (t3_euler_order)order;
                    quat_from_euler_d(ex, ey, ez, order, qd);
                } else {
                    qd[0] = tab_num(q, 1); qd[1] = tab_num(q, 2); qd[2] = tab_num(q, 3); qd[3] = tab_num(q, 4);
                }
                t3_object_set_trs_d(o, pd, qd, sd);
            }
        }
        int fl = tv_true(tab_geti(rec, 9));
        int mx = tv_true(tab_geti(rec, 10)) && !mau;
        if (fl || mx) {
            keep++;
            if (keep != i) {
                lua_rawgeti(L, 1, i);
                lua_rawseti(L, 1, keep);         /* list[keep] = rec (with the GC barrier) */
            }
        }
    }
    for (int i = keep + 1; i <= n; i++) {
        lua_pushnil(L);
        lua_rawseti(L, 1, i);
    }
    lua_pushinteger(L, keep);
    return 1;
}

/* ── animation: the per-track step of AnimationAction:_update ─────────── */

enum { AK_PP, AK_SV, AK_VS, AK_CI, AK_BUF, AK_CW, AK_RB, AK_MK, AK_CWA, AK_BINDING, AK_SETV, AK_RP, AK_ONCH,
       AK_X, AK_Y, AK_Z, AK_W, AK_TO, AK_MWNU, AK_N, AK_LO, AK_HI, AK_REC, AK_PN, AK_COUNT };
static TString *anim_key[AK_COUNT];

static TValue *tab_slot(Table *t, int i) { return (TValue *)tab_geti(t, i); }
static void slot_set(Table *t, int i, double v) {
    TValue *s = tab_slot(t, i);
    if (!isabstkey(s)) setfltvalue(s, v);
}
static double tab_field_num(Table *t, int k) { return tv_num(luaH_getshortstr(t, anim_key[k])); }
static Table *tab_field_tab(Table *t, int k) { return tv_tab(luaH_getshortstr(t, anim_key[k])); }

/* Quaternion.slerpFlat (three.lua's Interpolants.slerpFlat), 0-based offsets */
static void slerp_flat(Table *dst, int dofs, Table *s0, int o0, Table *s1, int o1, double t) {
    double x0 = tab_num(s0, o0 + 1), y0 = tab_num(s0, o0 + 2), z0 = tab_num(s0, o0 + 3), w0 = tab_num(s0, o0 + 4);
    double x1 = tab_num(s1, o1 + 1), y1 = tab_num(s1, o1 + 2), z1 = tab_num(s1, o1 + 3), w1 = tab_num(s1, o1 + 4);
    if (t == 0) {
        slot_set(dst, dofs + 1, x0); slot_set(dst, dofs + 2, y0); slot_set(dst, dofs + 3, z0); slot_set(dst, dofs + 4, w0);
        return;
    }
    if (t == 1) {
        slot_set(dst, dofs + 1, x1); slot_set(dst, dofs + 2, y1); slot_set(dst, dofs + 3, z1); slot_set(dst, dofs + 4, w1);
        return;
    }
    if (w0 != w1 || x0 != x1 || y0 != y1 || z0 != z1) {
        double s = 1 - t;
        double c = x0 * x1 + y0 * y1 + z0 * z1 + w0 * w1;
        double dir = c >= 0 ? 1 : -1;
        double sqrSin = 1 - c * c;
        if (sqrSin > 2.220446049250313e-16) {
            double sn = sqrt(sqrSin);
            double len = atan2(sn, c * dir);
            s = sin(s * len) / sn;
            t = sin(t * len) / sn;
        }
        double tDir = t * dir;
        x0 = x0 * s + x1 * tDir;
        y0 = y0 * s + y1 * tDir;
        z0 = z0 * s + z1 * tDir;
        w0 = w0 * s + w1 * tDir;
        if (s == 1 - t) {
            double f = 1 / sqrt(x0 * x0 + y0 * y0 + z0 * z0 + w0 * w0);
            x0 *= f; y0 *= f; z0 *= f; w0 *= f;
        }
    }
    slot_set(dst, dofs + 1, x0); slot_set(dst, dofs + 2, y0); slot_set(dst, dofs + 3, z0); slot_set(dst, dofs + 4, w0);
}

/* pp[k] for 1-based k: present (returns 1, *v) or nil (0) */
static int pp_at(Table *pp, int n, int k, double *v) {
    if (k < 1 || k > n) return 0;
    *v = tab_num(pp, k);
    return 1;
}

/* Interpolant:evaluate(t) of three.lua (r128's seek), for linear (1),
 * discrete (2) and quaternion-linear (3) interpolants writing into rb */
static void interp_eval(Table *it, int kind, double t) {
    Table *pp = tab_field_tab(it, AK_PP), *sv = tab_field_tab(it, AK_SV), *rb = NULL;
    (void)rb;
    int stride = (int)tab_field_num(it, AK_VS);
    int n = (int)luaH_getn(pp);
    int i1 = (int)tab_field_num(it, AK_CI);
    double t0 = 0, t1 = 0;
    int has1 = pp_at(pp, n, i1 + 1, &t1), has0 = pp_at(pp, n, i1, &t0);
    int right = 0, giveUpAt, copy_index = -1;
    if (!has1 || !(t < t1)) {
        giveUpAt = i1 + 2;
        for (;;) {
            if (!has1) {
                if (has0 && t < t0) goto backward;
                i1 = n;
                copy_index = i1 - 1;       /* afterEnd_ */
                goto done;
            }
            if (i1 == giveUpAt) break;
            t0 = t1; has0 = 1;
            i1 = i1 + 1;
            has1 = pp_at(pp, n, i1 + 1, &t1);
            if (has1 && t < t1) goto found;
        }
        right = n;
        goto search;
    }
backward:
    if (!has0 || !(t >= t0)) {
        double t1g;
        if (pp_at(pp, n, 2, &t1g) && t < t1g) {
            i1 = 2;
            t0 = t1g; has0 = 1;
        }
        giveUpAt = i1 - 2;
        for (;;) {
            if (!has0) {
                i1 = 0;
                copy_index = 0;            /* beforeStart_ */
                goto done;
            }
            if (i1 == giveUpAt) break;
            t1 = t0; has1 = 1;
            i1 = i1 - 1;
            has0 = pp_at(pp, n, i1, &t0);
            if (has0 && t >= t0) goto found;
        }
        right = i1;
        i1 = 0;
        goto search;
    }
    goto interpolate;
search:
    while (i1 < right) {
        int mid = (i1 + right) / 2;
        double pm = 0;
        pp_at(pp, n, mid + 1, &pm);
        if (t < pm) right = mid; else i1 = mid + 1;
    }
    has1 = pp_at(pp, n, i1 + 1, &t1);
    has0 = pp_at(pp, n, i1, &t0);
    if (!has0) { i1 = 0; copy_index = 0; goto done; }
    if (!has1) { i1 = n; copy_index = i1 - 1; goto done; }
found:
interpolate:
    {
        /* the mixer's buffer is the interpolant's resultBuffer */
        Table *res = tv_tab(luaH_getshortstr(it, anim_key[AK_RB]));
        if (!res) return;
        if (kind == 2) { copy_index = i1 - 1; goto done_res; }
        if (kind == 1) {
            int off1 = i1 * stride, off0 = off1 - stride;
            double w1 = (t - t0) / (t1 - t0), w0 = 1 - w1;
            for (int k = 1; k <= stride; k++) slot_set(res, k, tab_num(sv, off0 + k) * w0 + tab_num(sv, off1 + k) * w1);
        } else {
            double alpha = (t - t0) / (t1 - t0);
            int off = i1 * stride, stop = off + stride, dofs = 0;
            (void)dofs;
            while (off != stop) {
                slerp_flat(res, 0, sv, off - stride, sv, off, alpha);
                off += 4;
            }
        }
        goto store;
    done_res:
        for (int k = 1; k <= stride; k++) slot_set(res, k, tab_num(sv, copy_index * stride + k));
        goto store;
    }
done:
    {
        Table *res = tv_tab(luaH_getshortstr(it, anim_key[AK_RB]));
        if (res)
            for (int k = 1; k <= stride; k++) slot_set(res, k, tab_num(sv, copy_index * stride + k));
    }
store:
    {
        TValue *ci = (TValue *)luaH_getshortstr(it, anim_key[AK_CI]);
        if (!isabstkey(ci)) setivalue(ci, i1);
    }
}

/* t3.anim_step(interpolants, mixers, kinds, n, t, accuIndex, weight):
 * for each track j, interpolants[j]:evaluate(t) then
 * mixers[j]:accumulate(accuIndex, weight), as three.lua's
 * AnimationAction:_update does. kinds[j] = interpolant kind * 4 + mix kind
 * (interpolant 1 linear, 2 discrete, 3 quaternion; mix 1 lerp, 2 slerp,
 * 3 select). The interpolant's resultBuffer must be the mixer's buffer,
 * which AnimationAction arranges; three.lua checks this before calling. */
static int l_anim_step(lua_State *L) {
    Table *its = tab_at(L, 1), *mxs = tab_at(L, 2), *kinds = tab_at(L, 3);
    int n = I(4);
    double t = lua_tonumber(L, 5);
    int accuIndex = I(6);
    double weight = lua_tonumber(L, 7);
    for (int j = 1; j <= n; j++) {
        Table *it = tv_tab(tab_geti(its, j)), *mx = tv_tab(tab_geti(mxs, j));
        if (!it || !mx) continue;
        int k = (int)tab_num(kinds, j);
        int ik = k >> 2, mk = k & 3;
        /* the interpolant writes its own resultBuffer: point it at the mixer's */
        interp_eval(it, ik, t);
        Table *buf = tab_field_tab(mx, AK_BUF);
        int stride = (int)tab_field_num(mx, AK_VS);
        int offset = accuIndex * stride + stride;
        TValue *cwv = (TValue *)luaH_getshortstr(mx, anim_key[AK_CW]);
        double cw = tv_num(cwv);
        if (cw == 0) {
            for (int i = 1; i <= stride; i++) slot_set(buf, offset + i, tab_num(buf, i));
            cw = weight;
        } else {
            cw += weight;
            double mix = weight / cw;
            if (mk == 1) {
                double s = 1 - mix;
                for (int i = 1; i <= stride; i++) slot_set(buf, offset + i, tab_num(buf, offset + i) * s + tab_num(buf, i) * mix);
            } else if (mk == 2) {
                slerp_flat(buf, offset, buf, offset, buf, 0, mix);
            } else if (mix >= 0.5) {
                for (int i = 1; i <= stride; i++) {
                    /* select: copy the value whatever its type (strings, booleans) */
                    const TValue *src = tab_geti(buf, i);
                    TValue *dst = tab_slot(buf, offset + i);
                    if (!isabstkey(dst)) setobj(L, dst, src);
                }
            }
        }
        if (!isabstkey(cwv)) setfltvalue(cwv, cw);
    }
    return 0;
}

/* t3.anim_apply(mixers, n, accuIndex, changed, other) -> nchanged, nother:
 * PropertyMixer:apply for each of the n mixers whose mix kind (_t3mk: 1
 * lerp, 2 slerp, 3 select) is known and that has no additive weight: the
 * original value mixed in for a weight short of 1, then the two accus
 * compared. changed gets the indices of the mixers whose value moved (Lua
 * calls their binding's setValue: the write must go through the target's
 * own setters); other gets the indices left to Lua's apply. */
/* t3.anim_register(setFromArray, Vector3, Quaternion, setFromArrayMWNU):
 * the plain fromArray setter, the two classes whose fromArray anim_apply may
 * do itself (field writes, then the quaternion's _onChange left to Lua), and
 * the setter that also sets targetObject.matrixWorldNeedsUpdate (an
 * Object3D's position / quaternion / scale) */
static const void *anim_fromarray, *anim_v3_mt, *anim_q_mt, *anim_fromarray_mw;
/* t3.anim_register_more(WV3, WQ, setDirect, setDirectMWNU): a mirrored
 * object's watched vector / quaternion metatables (three.lua's _t3.lua:
 * components in array slots, slot 5 a quaternion's stale flag, _rec the
 * record Lua marks), and the plain-property setters */
static const void *anim_wv3_mt, *anim_wq_mt, *anim_setdirect, *anim_setdirect_mw;
static int l_anim_register_more(lua_State *L) {
    for (int i = 1; i <= 4; i++) { lua_pushvalue(L, i); luaL_ref(L, LUA_REGISTRYINDEX); }
    anim_wv3_mt = lua_topointer(L, 1);
    anim_wq_mt = lua_topointer(L, 2);
    anim_setdirect = lua_topointer(L, 3);
    anim_setdirect_mw = lua_topointer(L, 4);
    return 0;
}
static int l_anim_register(lua_State *L) {
    for (int i = 1; i <= 4; i++) { lua_pushvalue(L, i); luaL_ref(L, LUA_REGISTRYINDEX); }
    anim_fromarray = lua_topointer(L, 1);
    anim_v3_mt = lua_topointer(L, 2);
    anim_q_mt = lua_topointer(L, 3);
    anim_fromarray_mw = lua_isfunction(L, 4) ? lua_topointer(L, 4) : NULL;
    return 0;
}

/* The binding's write done here when it is the plain fromArray setter on a
 * plain Vector3 / Quaternion (fields present, its own metatable): returns
 * the target, or NULL for Lua's setValue */
/* setDirect for a number property that is a plain number field (Lua's
 * boolean conversion, and a watched field, are left to Lua) */
static int anim_direct_number(lua_State *L, Table *b, const void *setter, Table *buf, int offset) {
    TValue *mwnu = NULL;
    if (setter == anim_setdirect_mw) {
        Table *to = tab_field_tab(b, AK_TO);
        if (!to) return 0;
        mwnu = (TValue *)luaH_getshortstr(to, anim_key[AK_MWNU]);
        if (isabstkey(mwnu)) return 0;
    }
    Table *to = tab_field_tab(b, AK_TO);
    const TValue *pn = luaH_getshortstr(b, anim_key[AK_PN]);
    if (!to || !ttisshrstring(pn)) return 0;
    TValue *slot = (TValue *)luaH_getshortstr(to, tsvalue(pn));
    const TValue *v = tab_geti(buf, offset + 1);
    if (isabstkey(slot) || !ttisnumber(slot) || !ttisnumber(v)) return 0;
    setobj(L, slot, v);
    if (mwnu) setbtvalue(mwnu);
    return 1;
}

/* *rec: set to a watched target's record (Lua marks it) */
static Table *anim_direct(lua_State *L, Table *mx, int stride, Table *buf, int offset, Table **rec, int *done,
                          int allow_watched) {
    *rec = NULL;
    *done = 0;
    if (!anim_fromarray) return NULL;
    Table *b = tab_field_tab(mx, AK_BINDING);
    if (!b) return NULL;
    const TValue *sv = luaH_getshortstr(b, anim_key[AK_SETV]);
    if (!ttisLclosure(sv)) return NULL;
    const void *setter = clLvalue(sv);
    if (stride == 1 && anim_setdirect && (setter == anim_setdirect || setter == anim_setdirect_mw)) {
        *done = anim_direct_number(L, b, setter, buf, offset);
        return NULL;
    }
    TValue *mwnu = NULL;
    if (setter == anim_fromarray_mw && anim_fromarray_mw) {
        /* the flag must be a plain field of the target (a mirrored object
         * watches its writes: Lua's setter then) */
        Table *to = tab_field_tab(b, AK_TO);
        if (!to) return NULL;
        mwnu = (TValue *)luaH_getshortstr(to, anim_key[AK_MWNU]);
        if (isabstkey(mwnu)) return NULL;
    } else if (setter != anim_fromarray) return NULL;
    Table *dst = tab_field_tab(b, AK_RP);
    if (!dst || !dst->metatable) return NULL;
    const void *mt = dst->metatable;
    if (allow_watched && anim_wv3_mt && ((mt == anim_wv3_mt && stride == 3) || (mt == anim_wq_mt && stride == 4))) {
        /* watched: the components in slots 1..stride (a quaternion's stale
         * flag, slot 5, cleared: all four are written), then Lua marks */
        /* (the slots may sit in the hash part: they were added to a table
         * that had only string keys) */
        TValue *sl[5];
        int ns = stride == 4 ? 5 : 3;
        for (int i = 0; i < ns; i++) {
            sl[i] = tab_slot(dst, i + 1);
            if (isabstkey(sl[i])) return NULL;
        }
        Table *r = tv_tab(luaH_getshortstr(dst, anim_key[AK_REC]));
        if (!r) return NULL;
        for (int i = 0; i < stride; i++) setobj(L, sl[i], tab_geti(buf, offset + i + 1));
        if (stride == 4) setbfvalue(sl[4]);
        if (mwnu) setbtvalue(mwnu);
        *rec = r;
        *done = 1;
        return dst;
    }
    if (!((mt == anim_v3_mt && stride == 3) || (mt == anim_q_mt && stride == 4))) return NULL;
    TValue *slot[4];
    for (int i = 0; i < stride; i++) {
        slot[i] = (TValue *)luaH_getshortstr(dst, anim_key[AK_X + i]);
        if (isabstkey(slot[i])) return NULL;
    }
    for (int i = 0; i < stride; i++) {
        const TValue *v = tab_geti(buf, offset + i + 1);
        setobj(L, slot[i], v);
    }
    if (mwnu) setbtvalue(mwnu);
    *done = 1;
    return dst;
}

static int l_anim_apply(lua_State *L) {
    Table *mxs = tab_at(L, 1);
    int n = I(2), accuIndex = I(3);
    int nc = 0, no = 0, ncb = 0, nm = 0;
    int have_cb = lua_istable(L, 6), have_marks = lua_istable(L, 7);
    for (int j = 1; j <= n; j++) {
        Table *mx = tv_tab(tab_geti(mxs, j));
        if (!mx) continue;
        const TValue *mkv = luaH_getshortstr(mx, anim_key[AK_MK]);
        int mk = ttisinteger(mkv) ? (int)ivalue(mkv) : 0;
        TValue *wav = (TValue *)luaH_getshortstr(mx, anim_key[AK_CWA]);
        if (!mk || tv_num(wav) > 0) {
            lua_pushinteger(L, j);
            lua_rawseti(L, 5, ++no);
            continue;
        }
        Table *buf = tab_field_tab(mx, AK_BUF);
        int stride = (int)tab_field_num(mx, AK_VS);
        int offset = accuIndex * stride + stride;
        TValue *cwv = (TValue *)luaH_getshortstr(mx, anim_key[AK_CW]);
        double weight = tv_num(cwv);
        if (!isabstkey(cwv)) setfltvalue(cwv, 0);
        if (!isabstkey(wav)) setfltvalue(wav, 0);
        if (weight < 1) {
            int orig = stride * 3;
            double t = 1 - weight;
            if (mk == 1) {
                double s = 1 - t;
                for (int i = 1; i <= stride; i++) slot_set(buf, offset + i, tab_num(buf, offset + i) * s + tab_num(buf, orig + i) * t);
            } else if (mk == 2) {
                slerp_flat(buf, offset, buf, offset, buf, orig, t);
            } else if (t >= 0.5) {
                for (int i = 1; i <= stride; i++) {
                    TValue *dst = tab_slot(buf, offset + i);
                    if (!isabstkey(dst)) setobj(L, dst, tab_geti(buf, orig + i));
                }
            }
        }
        for (int i = stride + 1; i <= stride + stride; i++)
            if (!luaV_rawequalobj(tab_geti(buf, i), tab_geti(buf, i + stride))) {
                Table *rec = NULL;
                int done = 0;
                Table *dst = have_cb ? anim_direct(L, mx, stride, buf, offset, &rec, &done, have_marks) : NULL;
                if (!done) {
                    lua_pushinteger(L, j);
                    lua_rawseti(L, 4, ++nc);
                    break;
                }
                if (rec) {
                    sethvalue2s(L, L->top.p, rec);
                    L->top.p++;
                    lua_rawseti(L, 7, ++nm);
                }
                if (dst && stride == 4 && !ttisnil(luaH_getshortstr(dst, anim_key[AK_ONCH]))) {
                    sethvalue2s(L, L->top.p, dst);
                    L->top.p++;
                    lua_rawseti(L, 6, ++ncb);
                }
                break;
            }
    }
    lua_pushinteger(L, nc);
    lua_pushinteger(L, no);
    lua_pushinteger(L, ncb);
    lua_pushinteger(L, nm);
    return 4;
}

/* ── Matrix4 kernels for three.lua ─────────────────────────────────────
 * Ports of three.lua's Matrix4 methods, expression for expression (the
 * affine fast paths included), so the doubles are the same. They work on
 * a Matrix4's elements when it is a plain 16-number array; anything else
 * (a matrixWorld stand-in, a missing slot) returns false and the Lua
 * method runs instead. */

static int m4_get(lua_State *L, int idx, double e[16], Table **out) {
    if (!lua_istable(L, idx)) return 0;
    Table *t = tab_at(L, idx);
    if (t->alimit < 16) return 0;
    for (int i = 0; i < 16; i++) {
        const TValue *v = &t->array[i];
        if (ttisfloat(v)) e[i] = fltvalue(v);
        else if (ttisinteger(v)) e[i] = (double)ivalue(v);
        else return 0;
    }
    if (out) *out = t;
    return 1;
}
#define SETF(t, i, v) setfltvalue(&(t)->array[(i) - 1], (v))
#define SETI(t, i, v) setivalue(&(t)->array[(i) - 1], (v))

/* t3.m4_copy(te, me) -> ok: Matrix4:copy */
static int l_m4_copy(lua_State *L) {
    if (!lua_istable(L, 1) || !lua_istable(L, 2)) { lua_pushboolean(L, 0); return 1; }
    Table *a = tab_at(L, 1), *b = tab_at(L, 2);
    if (a->alimit < 16 || b->alimit < 16) { lua_pushboolean(L, 0); return 1; }
    for (int i = 0; i < 16; i++) setobj(L, &a->array[i], &b->array[i]);
    lua_pushboolean(L, 1);
    return 1;
}

/* t3.m4_mul(te, ae, be) -> ok: Matrix4:multiplyMatrices (te may be ae or be) */
static int l_m4_mul(lua_State *L) {
    double ae[16], be[16];
    Table *te;
    if (!m4_get(L, 2, ae, NULL) || !m4_get(L, 3, be, NULL) || !lua_istable(L, 1) ||
        (te = tab_at(L, 1))->alimit < 16) { lua_pushboolean(L, 0); return 1; }
    if (ae[3] == 0 && ae[7] == 0 && ae[11] == 0 && ae[15] == 1 && be[3] == 0 && be[7] == 0 && be[11] == 0 && be[15] == 1) {
        double a11 = ae[0], a21 = ae[1], a31 = ae[2], a12 = ae[4], a22 = ae[5], a32 = ae[6];
        double a13 = ae[8], a23 = ae[9], a33 = ae[10], a14 = ae[12], a24 = ae[13], a34 = ae[14];
        double b11 = be[0], b21 = be[1], b31 = be[2], b12 = be[4], b22 = be[5], b32 = be[6];
        double b13 = be[8], b23 = be[9], b33 = be[10], b14 = be[12], b24 = be[13], b34 = be[14];
        SETF(te, 1, a11 * b11 + a12 * b21 + a13 * b31);
        SETF(te, 2, a21 * b11 + a22 * b21 + a23 * b31);
        SETF(te, 3, a31 * b11 + a32 * b21 + a33 * b31);
        SETI(te, 4, 0);
        SETF(te, 5, a11 * b12 + a12 * b22 + a13 * b32);
        SETF(te, 6, a21 * b12 + a22 * b22 + a23 * b32);
        SETF(te, 7, a31 * b12 + a32 * b22 + a33 * b32);
        SETI(te, 8, 0);
        SETF(te, 9, a11 * b13 + a12 * b23 + a13 * b33);
        SETF(te, 10, a21 * b13 + a22 * b23 + a23 * b33);
        SETF(te, 11, a31 * b13 + a32 * b23 + a33 * b33);
        SETI(te, 12, 0);
        SETF(te, 13, a11 * b14 + a12 * b24 + a13 * b34 + a14);
        SETF(te, 14, a21 * b14 + a22 * b24 + a23 * b34 + a24);
        SETF(te, 15, a31 * b14 + a32 * b24 + a33 * b34 + a34);
        SETI(te, 16, 1);
    } else {
        double a11 = ae[0], a21 = ae[1], a31 = ae[2], a41 = ae[3], a12 = ae[4], a22 = ae[5], a32 = ae[6], a42 = ae[7];
        double a13 = ae[8], a23 = ae[9], a33 = ae[10], a43 = ae[11], a14 = ae[12], a24 = ae[13], a34 = ae[14], a44 = ae[15];
        double b11 = be[0], b21 = be[1], b31 = be[2], b41 = be[3], b12 = be[4], b22 = be[5], b32 = be[6], b42 = be[7];
        double b13 = be[8], b23 = be[9], b33 = be[10], b43 = be[11], b14 = be[12], b24 = be[13], b34 = be[14], b44 = be[15];
        SETF(te, 1, a11 * b11 + a12 * b21 + a13 * b31 + a14 * b41);
        SETF(te, 2, a21 * b11 + a22 * b21 + a23 * b31 + a24 * b41);
        SETF(te, 3, a31 * b11 + a32 * b21 + a33 * b31 + a34 * b41);
        SETF(te, 4, a41 * b11 + a42 * b21 + a43 * b31 + a44 * b41);
        SETF(te, 5, a11 * b12 + a12 * b22 + a13 * b32 + a14 * b42);
        SETF(te, 6, a21 * b12 + a22 * b22 + a23 * b32 + a24 * b42);
        SETF(te, 7, a31 * b12 + a32 * b22 + a33 * b32 + a34 * b42);
        SETF(te, 8, a41 * b12 + a42 * b22 + a43 * b32 + a44 * b42);
        SETF(te, 9, a11 * b13 + a12 * b23 + a13 * b33 + a14 * b43);
        SETF(te, 10, a21 * b13 + a22 * b23 + a23 * b33 + a24 * b43);
        SETF(te, 11, a31 * b13 + a32 * b23 + a33 * b33 + a34 * b43);
        SETF(te, 12, a41 * b13 + a42 * b23 + a43 * b33 + a44 * b43);
        SETF(te, 13, a11 * b14 + a12 * b24 + a13 * b34 + a14 * b44);
        SETF(te, 14, a21 * b14 + a22 * b24 + a23 * b34 + a24 * b44);
        SETF(te, 15, a31 * b14 + a32 * b24 + a33 * b34 + a34 * b44);
        SETF(te, 16, a41 * b14 + a42 * b24 + a43 * b34 + a44 * b44);
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* t3.m4_invert(te) -> ok: Matrix4:invert (affine path, else the general
 * cofactor inverse, zero on a singular matrix) */
static int l_m4_invert(lua_State *L) {
    double e[16];
    Table *te;
    if (!m4_get(L, 1, e, &te)) { lua_pushboolean(L, 0); return 1; }
    if (e[3] == 0 && e[7] == 0 && e[11] == 0 && e[15] == 1) {
        double a11 = e[0], a21 = e[1], a31 = e[2], a12 = e[4], a22 = e[5], a32 = e[6];
        double a13 = e[8], a23 = e[9], a33 = e[10], tx = e[12], ty = e[13], tz = e[14];
        double c11 = a22 * a33 - a23 * a32;
        double c12 = a23 * a31 - a21 * a33;
        double c13 = a21 * a32 - a22 * a31;
        double det = a11 * c11 + a12 * c12 + a13 * c13;
        if (det != 0) {
            double d = 1 / det;
            double i11 = c11 * d;
            double i12 = (a13 * a32 - a12 * a33) * d;
            double i13 = (a12 * a23 - a13 * a22) * d;
            double i21 = c12 * d;
            double i22 = (a11 * a33 - a13 * a31) * d;
            double i23 = (a13 * a21 - a11 * a23) * d;
            double i31 = c13 * d;
            double i32 = (a12 * a31 - a11 * a32) * d;
            double i33 = (a11 * a22 - a12 * a21) * d;
            SETF(te, 1, i11); SETF(te, 2, i21); SETF(te, 3, i31); SETI(te, 4, 0);
            SETF(te, 5, i12); SETF(te, 6, i22); SETF(te, 7, i32); SETI(te, 8, 0);
            SETF(te, 9, i13); SETF(te, 10, i23); SETF(te, 11, i33); SETI(te, 12, 0);
            SETF(te, 13, -(i11 * tx + i12 * ty + i13 * tz));
            SETF(te, 14, -(i21 * tx + i22 * ty + i23 * tz));
            SETF(te, 15, -(i31 * tx + i32 * ty + i33 * tz));
            SETI(te, 16, 1);
            lua_pushboolean(L, 1);
            return 1;
        }
    }
    double n11 = e[0], n21 = e[1], n31 = e[2], n41 = e[3], n12 = e[4], n22 = e[5], n32 = e[6], n42 = e[7];
    double n13 = e[8], n23 = e[9], n33 = e[10], n43 = e[11], n14 = e[12], n24 = e[13], n34 = e[14], n44 = e[15];
    double t11 = n23 * n34 * n42 - n24 * n33 * n42 + n24 * n32 * n43 - n22 * n34 * n43 - n23 * n32 * n44 + n22 * n33 * n44;
    double t12 = n14 * n33 * n42 - n13 * n34 * n42 - n14 * n32 * n43 + n12 * n34 * n43 + n13 * n32 * n44 - n12 * n33 * n44;
    double t13 = n13 * n24 * n42 - n14 * n23 * n42 + n14 * n22 * n43 - n12 * n24 * n43 - n13 * n22 * n44 + n12 * n23 * n44;
    double t14 = n14 * n23 * n32 - n13 * n24 * n32 - n14 * n22 * n33 + n12 * n24 * n33 + n13 * n22 * n34 - n12 * n23 * n34;
    double det = n11 * t11 + n21 * t12 + n31 * t13 + n41 * t14;
    if (det == 0) {
        for (int i = 1; i <= 16; i++) SETI(te, i, 0);
        lua_pushboolean(L, 1);
        return 1;
    }
    double d = 1 / det;
    SETF(te, 1, t11 * d);
    SETF(te, 2, (n24 * n33 * n41 - n23 * n34 * n41 - n24 * n31 * n43 + n21 * n34 * n43 + n23 * n31 * n44 - n21 * n33 * n44) * d);
    SETF(te, 3, (n22 * n34 * n41 - n24 * n32 * n41 + n24 * n31 * n42 - n21 * n34 * n42 - n22 * n31 * n44 + n21 * n32 * n44) * d);
    SETF(te, 4, (n23 * n32 * n41 - n22 * n33 * n41 - n23 * n31 * n42 + n21 * n33 * n42 + n22 * n31 * n43 - n21 * n32 * n43) * d);
    SETF(te, 5, t12 * d);
    SETF(te, 6, (n13 * n34 * n41 - n14 * n33 * n41 + n14 * n31 * n43 - n11 * n34 * n43 - n13 * n31 * n44 + n11 * n33 * n44) * d);
    SETF(te, 7, (n14 * n32 * n41 - n12 * n34 * n41 - n14 * n31 * n42 + n11 * n34 * n42 + n12 * n31 * n44 - n11 * n32 * n44) * d);
    SETF(te, 8, (n12 * n33 * n41 - n13 * n32 * n41 + n13 * n31 * n42 - n11 * n33 * n42 - n12 * n31 * n43 + n11 * n32 * n43) * d);
    SETF(te, 9, t13 * d);
    SETF(te, 10, (n14 * n23 * n41 - n13 * n24 * n41 - n14 * n21 * n43 + n11 * n24 * n43 + n13 * n21 * n44 - n11 * n23 * n44) * d);
    SETF(te, 11, (n12 * n24 * n41 - n14 * n22 * n41 + n14 * n21 * n42 - n11 * n24 * n42 - n12 * n21 * n44 + n11 * n22 * n44) * d);
    SETF(te, 12, (n13 * n22 * n41 - n12 * n23 * n41 - n13 * n21 * n42 + n11 * n23 * n42 + n12 * n21 * n43 - n11 * n22 * n43) * d);
    SETF(te, 13, t14 * d);
    SETF(te, 14, (n13 * n24 * n31 - n14 * n23 * n31 + n14 * n21 * n33 - n11 * n24 * n33 - n13 * n21 * n34 + n11 * n23 * n34) * d);
    SETF(te, 15, (n14 * n22 * n31 - n12 * n24 * n31 - n14 * n21 * n32 + n11 * n24 * n32 + n12 * n21 * n34 - n11 * n22 * n34) * d);
    SETF(te, 16, (n12 * n23 * n31 - n13 * n22 * n31 + n13 * n21 * n32 - n11 * n23 * n32 - n12 * n21 * n33 + n11 * n22 * n33) * d);
    lua_pushboolean(L, 1);
    return 1;
}

/* ── Vector3 / Quaternion / compose kernels (fields x y z w) ──────────────
 * Lua keeps integer arithmetic integer (Vector3(0, 1, 0) stays integers
 * through an identity quaternion), so these carry the number subtype with
 * each value and store what Lua would have stored. Integers past 2^53 (where
 * Lua's would wrap and a double would not) give up and leave it to Lua. */

enum { FK_X, FK_Y, FK_Z, FK_W, FK_EX, FK_EY, FK_EZ, FK_EO, FK_COUNT };
static TString *fkey[FK_COUNT];

typedef struct { double d; int i; } num_t;
static int num_bail;
static inline num_t nchk(double d, int i) {
    if (i && fabs(d) >= 9007199254740992.0) num_bail = 1;
    num_t r = { d, i };
    return r;
}
static inline num_t nadd(num_t a, num_t b) { return nchk(a.d + b.d, a.i && b.i); }
static inline num_t nsub(num_t a, num_t b) { return nchk(a.d - b.d, a.i && b.i); }
static inline num_t nmul(num_t a, num_t b) { return nchk(a.d * b.d, a.i && b.i); }
static inline num_t nint(double d) { num_t r = { d, 1 }; return r; }
static inline num_t nflt(double d) { num_t r = { d, 0 }; return r; }

/* the numbers in fields (x, y, z[, w]) of the table at idx; 0 if it is not
 * such a plain table (a watched vector keeps them in slots instead) */
static int get_fields(lua_State *L, int idx, int n, num_t *out, Table **tp) {
    if (!lua_istable(L, idx)) return 0;
    Table *t = tab_at(L, idx);
    for (int i = 0; i < n; i++) {
        const TValue *v = luaH_getshortstr(t, fkey[FK_X + i]);
        if (ttisfloat(v)) out[i] = nflt(fltvalue(v));
        else if (ttisinteger(v)) out[i] = nint((double)ivalue(v));
        else return 0;
    }
    if (tp) *tp = t;
    return 1;
}
static void set_tv(TValue *s, num_t v) {
    if (v.i) { setivalue(s, (lua_Integer)v.d); }
    else { setfltvalue(s, v.d); }
}
static void set_fields(Table *t, int n, const num_t *v) {
    for (int i = 0; i < n; i++) set_tv((TValue *)luaH_getshortstr(t, fkey[FK_X + i]), v[i]);
}
static int m4_get_n(lua_State *L, int idx, num_t e[16], Table **out) {
    if (!lua_istable(L, idx)) return 0;
    Table *t = tab_at(L, idx);
    if (t->alimit < 16) return 0;
    for (int i = 0; i < 16; i++) {
        const TValue *v = &t->array[i];
        if (ttisfloat(v)) e[i] = nflt(fltvalue(v));
        else if (ttisinteger(v)) e[i] = nint((double)ivalue(v));
        else return 0;
    }
    if (out) *out = t;
    return 1;
}
#define NO_KERNEL do { lua_pushboolean(L, 0); return 1; } while (0)

/* t3.v3_apply_m4(v, elements) -> ok: Vector3:applyMatrix4 */
static int l_v3_apply_m4(lua_State *L) {
    num_t p[3], e[16];
    Table *t;
    if (!get_fields(L, 1, 3, p, &t) || !m4_get_n(L, 2, e, NULL)) NO_KERNEL;
    num_bail = 0;
    num_t x = p[0], y = p[1], z = p[2];
    num_t w = nadd(nadd(nadd(nmul(e[3], x), nmul(e[7], y)), nmul(e[11], z)), e[15]);
    w = (w.d == 0) ? nint(1) : nflt(1 / w.d);
    num_t r[3] = { nmul(nadd(nadd(nadd(nmul(e[0], x), nmul(e[4], y)), nmul(e[8], z)), e[12]), w),
                   nmul(nadd(nadd(nadd(nmul(e[1], x), nmul(e[5], y)), nmul(e[9], z)), e[13]), w),
                   nmul(nadd(nadd(nadd(nmul(e[2], x), nmul(e[6], y)), nmul(e[10], z)), e[14]), w) };
    if (num_bail) NO_KERNEL;
    set_fields(t, 3, r);
    lua_pushboolean(L, 1);
    return 1;
}

/* t3.v3_apply_q(v, q) -> ok: Vector3:applyQuaternion */
static int l_v3_apply_q(lua_State *L) {
    num_t v[3], q[4];
    Table *t;
    if (!get_fields(L, 1, 3, v, &t) || !get_fields(L, 2, 4, q, NULL)) NO_KERNEL;
    num_bail = 0;
    num_t vx = v[0], vy = v[1], vz = v[2], qx = q[0], qy = q[1], qz = q[2], qw = q[3], two = nint(2);
    num_t tx = nmul(two, nsub(nmul(qy, vz), nmul(qz, vy)));
    num_t ty = nmul(two, nsub(nmul(qz, vx), nmul(qx, vz)));
    num_t tz = nmul(two, nsub(nmul(qx, vy), nmul(qy, vx)));
    num_t r[3] = { nsub(nadd(nadd(vx, nmul(qw, tx)), nmul(qy, tz)), nmul(qz, ty)),
                   nsub(nadd(nadd(vy, nmul(qw, ty)), nmul(qz, tx)), nmul(qx, tz)),
                   nsub(nadd(nadd(vz, nmul(qw, tz)), nmul(qx, ty)), nmul(qy, tx)) };
    if (num_bail) NO_KERNEL;
    set_fields(t, 3, r);
    lua_pushboolean(L, 1);
    return 1;
}

/* t3.m4_compose(elements, position, quaternion, scale) -> ok: Matrix4:compose */
static int l_m4_compose(lua_State *L) {
    num_t p[3], q[4], s[3];
    if (!lua_istable(L, 1) || !get_fields(L, 2, 3, p, NULL) || !get_fields(L, 3, 4, q, NULL) ||
        !get_fields(L, 4, 3, s, NULL)) NO_KERNEL;
    Table *te = tab_at(L, 1);
    if (te->alimit < 16) NO_KERNEL;
    num_bail = 0;
    num_t x = q[0], y = q[1], z = q[2], w = q[3], one = nint(1);
    num_t x2 = nadd(x, x), y2 = nadd(y, y), z2 = nadd(z, z);
    num_t xx = nmul(x, x2), xy = nmul(x, y2), xz = nmul(x, z2);
    num_t yy = nmul(y, y2), yz = nmul(y, z2), zz = nmul(z, z2);
    num_t wx = nmul(w, x2), wy = nmul(w, y2), wz = nmul(w, z2);
    num_t sx = s[0], sy = s[1], sz = s[2];
    num_t r[12] = { nmul(nsub(one, nadd(yy, zz)), sx), nmul(nadd(xy, wz), sx), nmul(nsub(xz, wy), sx),
                    nmul(nsub(xy, wz), sy), nmul(nsub(one, nadd(xx, zz)), sy), nmul(nadd(yz, wx), sy),
                    nmul(nadd(xz, wy), sz), nmul(nsub(yz, wx), sz), nmul(nsub(one, nadd(xx, yy)), sz) };
    if (num_bail) NO_KERNEL;
    TValue *a = te->array;
    set_tv(&a[0], r[0]); set_tv(&a[1], r[1]); set_tv(&a[2], r[2]); setivalue(&a[3], 0);
    set_tv(&a[4], r[3]); set_tv(&a[5], r[4]); set_tv(&a[6], r[5]); setivalue(&a[7], 0);
    set_tv(&a[8], r[6]); set_tv(&a[9], r[7]); set_tv(&a[10], r[8]); setivalue(&a[11], 0);
    set_tv(&a[12], p[0]); set_tv(&a[13], p[1]); set_tv(&a[14], p[2]); setivalue(&a[15], 1);
    lua_pushboolean(L, 1);
    return 1;
}

/* t3.q_from_euler(q, euler) -> ok: Quaternion:setFromEuler's arithmetic
 * (the caller fires _onChange). The euler's _x _y _z _order fields. */
static int l_q_from_euler(lua_State *L) {
    num_t qd[4];
    Table *qt;
    if (!get_fields(L, 1, 4, qd, &qt) || !lua_istable(L, 2)) { lua_pushboolean(L, 0); return 1; }
    Table *et = tab_at(L, 2);
    const TValue *vx = luaH_getshortstr(et, fkey[FK_EX]), *vy = luaH_getshortstr(et, fkey[FK_EY]);
    const TValue *vz = luaH_getshortstr(et, fkey[FK_EZ]), *vo = luaH_getshortstr(et, fkey[FK_EO]);
    if (!ttisnumber(vx) || !ttisnumber(vy) || !ttisnumber(vz) || !ttisstring(vo)) { lua_pushboolean(L, 0); return 1; }
    double x = tv_num(vx), y = tv_num(vy), z = tv_num(vz);
    const char *order = getstr(tsvalue(vo));
    double hx = x * 0.5, hy = y * 0.5, hz = z * 0.5;
    double c1 = cos(hx), c2 = cos(hy), c3 = cos(hz);
    double s1 = sin(hx), s2 = sin(hy), s3 = sin(hz);
    double c1c2 = c1 * c2, s1s2 = s1 * s2, s1c2 = s1 * c2, c1s2 = c1 * s2;
    double c1c2c3 = c1c2 * c3, s1s2s3 = s1s2 * s3, s1c2c3 = s1c2 * c3, c1s2s3 = c1s2 * s3;
    double c1s2c3 = c1s2 * c3, s1c2s3 = s1c2 * s3, c1c2s3 = c1c2 * s3, s1s2c3 = s1s2 * c3;
    double r[4];
    num_t rn[4];
    if (!strcmp(order, "XYZ")) { r[0] = s1c2c3 + c1s2s3; r[1] = c1s2c3 - s1c2s3; r[2] = c1c2s3 + s1s2c3; r[3] = c1c2c3 - s1s2s3; }
    else if (!strcmp(order, "YXZ")) { r[0] = s1c2c3 + c1s2s3; r[1] = c1s2c3 - s1c2s3; r[2] = c1c2s3 - s1s2c3; r[3] = c1c2c3 + s1s2s3; }
    else if (!strcmp(order, "ZXY")) { r[0] = s1c2c3 - c1s2s3; r[1] = c1s2c3 + s1c2s3; r[2] = c1c2s3 + s1s2c3; r[3] = c1c2c3 - s1s2s3; }
    else if (!strcmp(order, "ZYX")) { r[0] = s1c2c3 - c1s2s3; r[1] = c1s2c3 + s1c2s3; r[2] = c1c2s3 - s1s2c3; r[3] = c1c2c3 + s1s2s3; }
    else if (!strcmp(order, "YZX")) { r[0] = s1c2c3 + c1s2s3; r[1] = c1s2c3 + s1c2s3; r[2] = c1c2s3 - s1s2c3; r[3] = c1c2c3 - s1s2s3; }
    else if (!strcmp(order, "XZY")) { r[0] = s1c2c3 - c1s2s3; r[1] = c1s2c3 - s1c2s3; r[2] = c1c2s3 + s1s2c3; r[3] = c1c2c3 + s1s2s3; }
    else { lua_pushboolean(L, 0); return 1; }
    for (int i = 0; i < 4; i++) rn[i] = nflt(r[i]);
    set_fields(qt, 4, rn);
    lua_pushboolean(L, 1);
    return 1;
}

/* t3.q_slerp(q, qb, t) -> ok: Quaternion:slerp's arithmetic for t other
 * than 0 and 1 (the caller handles those and fires _onChange) */
static int l_q_slerp(lua_State *L) {
    num_t an[4], bn[4];
    Table *qt;
    if (!get_fields(L, 1, 4, an, &qt) || !get_fields(L, 2, 4, bn, NULL) || !lua_isnumber(L, 3)) NO_KERNEL;
    double t = lua_tonumber(L, 3);
    double x = an[0].d, y = an[1].d, z = an[2].d, w = an[3].d;
    double b[4] = { bn[0].d, bn[1].d, bn[2].d, bn[3].d };
    double cosHalfTheta = w * b[3] + x * b[0] + y * b[1] + z * b[2];
    double sx, sy, sz, sw;           /* self after the copy / negation */
    if (cosHalfTheta < 0) { sw = -b[3]; sx = -b[0]; sy = -b[1]; sz = -b[2]; cosHalfTheta = -cosHalfTheta; }
    else { sx = b[0]; sy = b[1]; sz = b[2]; sw = b[3]; }
    /* >= 1: self is put back as it was, number subtypes and all */
    if (cosHalfTheta >= 1.0) { lua_pushboolean(L, 1); return 1; }
    num_t r[4];
    double sqrSinHalfTheta = 1.0 - cosHalfTheta * cosHalfTheta;
    if (sqrSinHalfTheta <= 2.220446049250313e-16) {
        double s = 1 - t;
        double rw = s * w + t * sw, rx = s * x + t * sx, ry = s * y + t * sy, rz = s * z + t * sz;
        /* Quaternion:normalize */
        double l = sqrt(rx * rx + ry * ry + rz * rz + rw * rw);
        if (l == 0) { r[0] = nint(0); r[1] = nint(0); r[2] = nint(0); r[3] = nint(1); }
        else { l = 1 / l; r[0] = nflt(rx * l); r[1] = nflt(ry * l); r[2] = nflt(rz * l); r[3] = nflt(rw * l); }
    } else {
        double sinHalfTheta = sqrt(sqrSinHalfTheta);
        double halfTheta = atan2(sinHalfTheta, cosHalfTheta);
        double ratioA = sin((1 - t) * halfTheta) / sinHalfTheta;
        double ratioB = sin(t * halfTheta) / sinHalfTheta;
        r[3] = nflt(w * ratioA + sw * ratioB); r[0] = nflt(x * ratioA + sx * ratioB);
        r[1] = nflt(y * ratioA + sy * ratioB); r[2] = nflt(z * ratioA + sz * ratioB);
    }
    set_fields(qt, 4, r);
    lua_pushboolean(L, 1);
    return 1;
}

/* ── BufferGeometry kernels ──────────────────────────────────────────── */

/* a 0-based index value as an integer (an integral float too, as Lua's
 * table keys normalize it); -1 if it is anything else */
static lua_Integer tv_index(const TValue *v) {
    if (ttisinteger(v)) return ivalue(v);
    if (ttisfloat(v)) {
        lua_Integer i;
        if (luaV_flttointeger(fltvalue(v), &i, F2Ieq)) return i;
    }
    return -1;
}
static int tv_dbl(const TValue *v, double *out) {
    if (ttisfloat(v)) { *out = fltvalue(v); return 1; }
    if (ttisinteger(v)) { *out = (double)ivalue(v); return 1; }
    return 0;
}

/* t3.deindex(array, indices, itemSize) -> out | false: toNonIndexed's copy
 * of one attribute (item i of out is item indices[i] of array), values
 * copied as they are. false (Lua does it) for a non-integer index or a
 * missing source value. */
static int l_deindex(lua_State *L) {
    if (!lua_istable(L, 1) || !lua_istable(L, 2)) NO_KERNEL;
    Table *src = tab_at(L, 1), *idx = tab_at(L, 2);
    lua_Integer is = luaL_checkinteger(L, 3);
    lua_Unsigned n = luaH_getn(idx);
    if (is < 1 || is > 64 || n * (lua_Unsigned)is > (1u << 26)) NO_KERNEL;
    lua_createtable(L, (int)(n * is), 0);
    Table *out = tab_at(L, -1);
    TValue *o = out->array;
    for (lua_Unsigned i = 1; i <= n; i++) {
        lua_Integer s0 = tv_index(tab_geti(idx, (int)i));
        if (s0 < 0) { lua_pop(L, 1); NO_KERNEL; }
        s0 *= is;
        for (lua_Integer k = 1; k <= is; k++) {
            const TValue *v = tab_geti(src, (int)(s0 + k));
            if (ttisnil(v)) { lua_pop(L, 1); NO_KERNEL; }
            setobj(L, o, v);
            o++;
        }
    }
    return 1;
}

/* t3.vertex_normals(pa, ps, n, idx|nil, normals|nil) -> normals | false:
 * computeVertexNormals' arithmetic (zero, accumulate per face, normalize),
 * the same expressions. normals is the attribute array to reuse (n * 3
 * slots) or nil for a new one. false: Lua does it. */
static int l_vertex_normals(lua_State *L) {
    if (!lua_istable(L, 1)) NO_KERNEL;
    Table *pa = tab_at(L, 1);
    lua_Integer ps = luaL_checkinteger(L, 2), n = luaL_checkinteger(L, 3);
    Table *idx = lua_istable(L, 4) ? tab_at(L, 4) : NULL;
    if (ps < 3 || n < 0 || n > (1 << 24)) NO_KERNEL;
    Table *na;
    if (lua_istable(L, 5)) {
        na = tab_at(L, 5);
        if ((lua_Integer)luaH_realasize(na) < n * 3) NO_KERNEL;
        lua_pushvalue(L, 5);
    } else {
        lua_createtable(L, (int)(n * 3), 0);
        na = tab_at(L, -1);
    }
    double *acc = (double *)calloc((size_t)(n * 3 + 1), sizeof(double));
    if (!acc) NO_KERNEL;
#define PA(k, dst) if (!tv_dbl(tab_geti(pa, (int)(k)), &(dst))) goto fail
    if (idx) {
        lua_Integer len = (lua_Integer)luaH_getn(idx);
        for (lua_Integer i = 1; i <= len - 2; i += 3) {
            lua_Integer ia = tv_index(tab_geti(idx, (int)i)), ib = tv_index(tab_geti(idx, (int)i + 1));
            lua_Integer ic = tv_index(tab_geti(idx, (int)i + 2));
            if (ia < 0 || ib < 0 || ic < 0 || ia >= n || ib >= n || ic >= n) goto fail;
            lua_Integer oa = ia * ps, ob = ib * ps, oc = ic * ps;
            double ax, ay, az, bx, by, bz, cx, cy, cz;
            PA(oa + 1, ax); PA(oa + 2, ay); PA(oa + 3, az);
            PA(ob + 1, bx); PA(ob + 2, by); PA(ob + 3, bz);
            PA(oc + 1, cx); PA(oc + 2, cy); PA(oc + 3, cz);
            double cbx = cx - bx, cby = cy - by, cbz = cz - bz;
            double abx = ax - bx, aby = ay - by, abz = az - bz;
            double nx = cby * abz - cbz * aby, ny = cbz * abx - cbx * abz, nz = cbx * aby - cby * abx;
            double *a = acc + ia * 3, *b = acc + ib * 3, *c = acc + ic * 3;
            a[0] = a[0] + nx; a[1] = a[1] + ny; a[2] = a[2] + nz;
            b[0] = b[0] + nx; b[1] = b[1] + ny; b[2] = b[2] + nz;
            c[0] = c[0] + nx; c[1] = c[1] + ny; c[2] = c[2] + nz;
        }
    } else {
        for (lua_Integer i = 0; i <= n - 3; i += 3) {
            lua_Integer oa = i * ps, ob = (i + 1) * ps, oc = (i + 2) * ps;
            double ax, ay, az, bx, by, bz, cx, cy, cz;
            PA(oa + 1, ax); PA(oa + 2, ay); PA(oa + 3, az);
            PA(ob + 1, bx); PA(ob + 2, by); PA(ob + 3, bz);
            PA(oc + 1, cx); PA(oc + 2, cy); PA(oc + 3, cz);
            double cbx = cx - bx, cby = cy - by, cbz = cz - bz;
            double abx = ax - bx, aby = ay - by, abz = az - bz;
            double nx = cby * abz - cbz * aby, ny = cbz * abx - cbx * abz, nz = cbx * aby - cby * abx;
            double *a = acc + i * 3;
            for (int k = 0; k < 9; k += 3) { a[k] = nx; a[k + 1] = ny; a[k + 2] = nz; }
        }
    }
#undef PA
    /* normalizeNormals: divide by the length, or by 1 when it is zero
     * (every result is a float, as x * inv is in Lua) */
    for (lua_Integer i = 0; i < n; i++) {
        double *v = acc + i * 3;
        double x = v[0], y = v[1], z = v[2];
        double len = sqrt(x * x + y * y + z * z);
        if (len == 0) len = 1;
        double inv = 1 / len;
        setfltvalue(&na->array[i * 3], x * inv);
        setfltvalue(&na->array[i * 3 + 1], y * inv);
        setfltvalue(&na->array[i * 3 + 2], z * inv);
    }
    free(acc);
    return 1;
fail:
    free(acc);
    lua_pop(L, 1);
    NO_KERNEL;
}

/* ── geometry generators: three.lua's, expression for expression ────────
 * Each returns indices, positions, normals, uvs as new tables (or false:
 * Lua builds it). Every vertex value three.lua computes is a float and every
 * index an integer, so that is what these store. */

static Table *new_array(lua_State *L, lua_Integer n) {
    lua_createtable(L, (int)n, 0);
    return tab_at(L, -1);
}
#define L_PI 3.141592653589793238462643383279502884

/* t3.gen_sphere(radius, widthSegments, heightSegments, phiStart, phiLength,
 * thetaStart, thetaLength): the arguments already defaulted and floored */
static int l_gen_sphere(lua_State *L) {
    for (int i = 1; i <= 7; i++) if (!lua_isnumber(L, i)) NO_KERNEL;
    if (!lua_isinteger(L, 2) || !lua_isinteger(L, 3)) NO_KERNEL;
    double radius = lua_tonumber(L, 1);
    lua_Integer ws = lua_tointeger(L, 2), hs = lua_tointeger(L, 3);
    double phiStart = lua_tonumber(L, 4), phiLength = lua_tonumber(L, 5);
    double thetaStart = lua_tonumber(L, 6), thetaLength = lua_tonumber(L, 7);
    if (ws < 1 || hs < 1 || (ws + 1) * (hs + 1) > (1 << 22)) NO_KERNEL;
    double thetaSum = thetaStart + thetaLength;
    if (thetaSum != thetaSum) NO_KERNEL;   /* NaN */
    double thetaEnd = L_PI < thetaSum ? L_PI : thetaSum;   /* math.min */
    lua_Integer nv = (ws + 1) * (hs + 1);
    int skipTop = !(thetaStart > 0), skipBottom = !(thetaEnd < L_PI);
    lua_Integer ntri = ws * hs * 2 - (skipTop ? ws : 0) - (skipBottom ? ws : 0);
    Table *ti = new_array(L, ntri * 3);
    Table *tp = new_array(L, nv * 3);
    Table *tn = new_array(L, nv * 3);
    Table *tu = new_array(L, nv * 2);
    TValue *P = tp->array, *N = tn->array, *U = tu->array;
    for (lua_Integer iy = 0; iy <= hs; iy++) {
        double v = (double)iy / (double)hs;
        double uOffset = 0;   /* (u + uOffset is a float either way) */
        if (iy == 0 && thetaStart == 0) uOffset = 0.5 / (double)ws;
        else if (iy == hs && thetaEnd == L_PI) uOffset = -0.5 / (double)ws;
        double theta = thetaStart + v * thetaLength;
        double sinTheta = sin(theta), cosTheta = cos(theta);
        double rSinTheta = radius * sinTheta, rCosTheta = radius * cosTheta;
        double oneMinusV = 1 - v;
        for (lua_Integer ix = 0; ix <= ws; ix++) {
            double u = (double)ix / (double)ws;
            double phi = phiStart + u * phiLength;
            double px = -rSinTheta * cos(phi);
            double py = rCosTheta;
            double pz = rSinTheta * sin(phi);
            setfltvalue(P, px); setfltvalue(P + 1, py); setfltvalue(P + 2, pz); P += 3;
            double len = sqrt(px * px + py * py + pz * pz);
            if (len == 0) len = 1;
            setfltvalue(N, px / len); setfltvalue(N + 1, py / len); setfltvalue(N + 2, pz / len); N += 3;
            setfltvalue(U, u + uOffset); setfltvalue(U + 1, oneMinusV); U += 2;
        }
    }
    TValue *I = ti->array;
    lua_Integer row = ws + 1;
    for (lua_Integer iy = 1; iy <= hs; iy++)
        for (lua_Integer ix = 1; ix <= ws; ix++) {
            /* grid[iy][ix] (1-based rows / columns) is (iy - 1) * row + ix - 1 */
            lua_Integer a = (iy - 1) * row + ix, b = (iy - 1) * row + ix - 1;
            lua_Integer c = iy * row + ix - 1, d = iy * row + ix;
            if (iy != 1 || thetaStart > 0) { setivalue(I, a); setivalue(I + 1, b); setivalue(I + 2, d); I += 3; }
            if (iy != hs || thetaEnd < L_PI) { setivalue(I, b); setivalue(I + 1, c); setivalue(I + 2, d); I += 3; }
        }
    if (I != ti->array + ntri * 3) return luaL_error(L, "t3.gen_sphere: index count");
    return 4;
}

/* t3.gen_torus(radius, tube, radialSegments, tubularSegments, arc) */
static int l_gen_torus(lua_State *L) {
    for (int i = 1; i <= 5; i++) if (!lua_isnumber(L, i)) NO_KERNEL;
    if (!lua_isinteger(L, 3) || !lua_isinteger(L, 4)) NO_KERNEL;
    double radius = lua_tonumber(L, 1), tube = lua_tonumber(L, 2), arc = lua_tonumber(L, 5);
    lua_Integer rs = lua_tointeger(L, 3), ts = lua_tointeger(L, 4);
    if (rs < 1 || ts < 1 || (rs + 1) * (ts + 1) > (1 << 22)) NO_KERNEL;
    lua_Integer nv = (rs + 1) * (ts + 1);
    Table *ti = new_array(L, rs * ts * 6);
    Table *tp = new_array(L, nv * 3);
    Table *tn = new_array(L, nv * 3);
    Table *tu = new_array(L, nv * 2);
    TValue *P = tp->array, *N = tn->array, *U = tu->array;
    for (lua_Integer j = 0; j <= rs; j++) {
        double v = (double)j / (double)rs * L_PI * 2;
        double tubeCosV = tube * cos(v);
        double pz = tube * sin(v);
        double ringRadius = radius + tubeCosV;
        double vv = (double)j / (double)rs;
        for (lua_Integer i = 0; i <= ts; i++) {
            double u = (double)i / (double)ts * arc;
            double cosU = cos(u), sinU = sin(u);
            double cx = radius * cosU, cy = radius * sinU;
            double px = ringRadius * cosU, py = ringRadius * sinU;
            setfltvalue(P, px); setfltvalue(P + 1, py); setfltvalue(P + 2, pz); P += 3;
            double nx = px - cx, ny = py - cy, nz = pz;
            double len = sqrt(nx * nx + ny * ny + nz * nz);
            if (len == 0) len = 1;
            setfltvalue(N, nx / len); setfltvalue(N + 1, ny / len); setfltvalue(N + 2, nz / len); N += 3;
            setfltvalue(U, (double)i / (double)ts); setfltvalue(U + 1, vv); U += 2;
        }
    }
    TValue *I = ti->array;
    for (lua_Integer j = 1; j <= rs; j++)
        for (lua_Integer i = 1; i <= ts; i++) {
            lua_Integer a = (ts + 1) * j + i - 1, b = (ts + 1) * (j - 1) + i - 1;
            lua_Integer c = (ts + 1) * (j - 1) + i, d = (ts + 1) * j + i;
            setivalue(I, a); setivalue(I + 1, b); setivalue(I + 2, d);
            setivalue(I + 3, b); setivalue(I + 4, c); setivalue(I + 5, d);
            I += 6;
        }
    return 4;
}

/* t3.gen_torus_knot(radius, tube, tubularSegments, radialSegments, p, q) */
static void knot_point(double u, double radius, double p, double q, double out[3]) {
    double cu = cos(u), su = sin(u);
    double quOverP = q / p * u;
    double cs = cos(quOverP);
    out[0] = radius * (2 + cs) * 0.5 * cu;
    out[1] = radius * (2 + cs) * su * 0.5;
    out[2] = radius * sin(quOverP) * 0.5;
}
static void v3_normalize_d(double v[3]) {   /* Vector3:normalize */
    double l = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l == 0) return;
    double inv = 1 / l;
    v[0] = v[0] * inv; v[1] = v[1] * inv; v[2] = v[2] * inv;
}
static void v3_cross_d(double out[3], const double a[3], const double b[3]) {   /* crossVectors */
    double ax = a[0], ay = a[1], az = a[2], bx = b[0], by = b[1], bz = b[2];
    out[0] = ay * bz - az * by;
    out[1] = az * bx - ax * bz;
    out[2] = ax * by - ay * bx;
}
static int l_gen_torus_knot(lua_State *L) {
    for (int i = 1; i <= 6; i++) if (!lua_isnumber(L, i)) NO_KERNEL;
    if (!lua_isinteger(L, 3) || !lua_isinteger(L, 4)) NO_KERNEL;
    double radius = lua_tonumber(L, 1), tube = lua_tonumber(L, 2), p = lua_tonumber(L, 5), q = lua_tonumber(L, 6);
    lua_Integer ts = lua_tointeger(L, 3), rs = lua_tointeger(L, 4);
    if (rs < 1 || ts < 1 || (rs + 1) * (ts + 1) > (1 << 22)) NO_KERNEL;
    lua_Integer nv = (rs + 1) * (ts + 1);
    double *ring = (double *)malloc(sizeof(double) * 3 * (size_t)(rs + 1));
    if (!ring) NO_KERNEL;
    for (lua_Integer j = 0; j <= rs; j++) {
        double v = (double)j / (double)rs * L_PI * 2;
        ring[j * 3] = -tube * cos(v);
        ring[j * 3 + 1] = tube * sin(v);
        ring[j * 3 + 2] = (double)j / (double)rs;
    }
    Table *ti = new_array(L, ts * rs * 6);
    Table *tp = new_array(L, nv * 3);
    Table *tn = new_array(L, nv * 3);
    Table *tu = new_array(L, nv * 2);
    TValue *P = tp->array, *N = tn->array, *U = tu->array;
    for (lua_Integer i = 0; i <= ts; i++) {
        double u = (double)i / (double)ts * p * L_PI * 2;
        double P1[3], P2[3], T[3], Nv[3], B[3];
        knot_point(u, radius, p, q, P1);
        knot_point(u + 0.01, radius, p, q, P2);
        T[0] = P2[0] - P1[0]; T[1] = P2[1] - P1[1]; T[2] = P2[2] - P1[2];
        Nv[0] = P2[0] + P1[0]; Nv[1] = P2[1] + P1[1]; Nv[2] = P2[2] + P1[2];
        v3_cross_d(B, T, Nv);
        v3_cross_d(Nv, B, T);
        v3_normalize_d(B);
        v3_normalize_d(Nv);
        double uCoord = (double)i / (double)ts;
        for (lua_Integer j = 0; j <= rs; j++) {
            double cx = ring[j * 3], cy = ring[j * 3 + 1];
            double x = P1[0] + (cx * Nv[0] + cy * B[0]);
            double y = P1[1] + (cx * Nv[1] + cy * B[1]);
            double z = P1[2] + (cx * Nv[2] + cy * B[2]);
            setfltvalue(P, x); setfltvalue(P + 1, y); setfltvalue(P + 2, z); P += 3;
            double nx = x - P1[0], ny = y - P1[1], nz = z - P1[2];
            double nl = sqrt(nx * nx + ny * ny + nz * nz);
            if (nl == 0) nl = 1;
            setfltvalue(N, nx / nl); setfltvalue(N + 1, ny / nl); setfltvalue(N + 2, nz / nl); N += 3;
            setfltvalue(U, uCoord); setfltvalue(U + 1, ring[j * 3 + 2]); U += 2;
        }
    }
    free(ring);
    TValue *I = ti->array;
    for (lua_Integer j = 1; j <= ts; j++)
        for (lua_Integer i = 1; i <= rs; i++) {
            lua_Integer a = (rs + 1) * (j - 1) + (i - 1), b = (rs + 1) * j + (i - 1);
            lua_Integer c = (rs + 1) * j + i, d = (rs + 1) * (j - 1) + i;
            setivalue(I, a); setivalue(I + 1, b); setivalue(I + 2, d);
            setivalue(I + 3, b); setivalue(I + 4, c); setivalue(I + 5, d);
            I += 6;
        }
    return 4;
}

/* t3.gen_cylinder(radiusTop, radiusBottom, height, radialSegments,
 * heightSegments, openEnded, thetaStart, thetaLength): the torso, then the
 * top cap (radiusTop > 0), then the bottom one (radiusBottom > 0), as
 * three.lua emits them. Cap values that are integers in Lua (0, sign) stay
 * integers. */
static int l_gen_cylinder(lua_State *L) {
    for (int i = 1; i <= 5; i++) if (!lua_isnumber(L, i)) NO_KERNEL;
    if (!lua_isnumber(L, 7) || !lua_isnumber(L, 8)) NO_KERNEL;
    if (!lua_isinteger(L, 4) || !lua_isinteger(L, 5)) NO_KERNEL;
    double rt = lua_tonumber(L, 1), rb = lua_tonumber(L, 2), height = lua_tonumber(L, 3);
    lua_Integer rs = lua_tointeger(L, 4), hs = lua_tointeger(L, 5);
    int openEnded = lua_toboolean(L, 6);
    double thetaStart = lua_tonumber(L, 7), thetaLength = lua_tonumber(L, 8);
    if (rs < 1 || hs < 1 || (rs + 1) * (hs + 1) > (1 << 22)) NO_KERNEL;
    int top = !openEnded && rt > 0, bottom = !openEnded && rb > 0;
    lua_Integer ncap = rs + rs + 1;
    lua_Integer nv = (rs + 1) * (hs + 1) + (top ? ncap : 0) + (bottom ? ncap : 0);
    lua_Integer ni = rs * hs * 6 + (top ? rs * 3 : 0) + (bottom ? rs * 3 : 0);
    double halfHeight = height / 2;
    double slope = (rb - rt) / height;
    Table *ti = new_array(L, ni);
    Table *tp = new_array(L, nv * 3);
    Table *tn = new_array(L, nv * 3);
    Table *tu = new_array(L, nv * 2);
    TValue *P = tp->array, *N = tn->array, *U = tu->array, *I = ti->array;
    for (lua_Integer y = 0; y <= hs; y++) {
        double v = (double)y / (double)hs;
        double radius = v * (rb - rt) + rt;
        for (lua_Integer x = 0; x <= rs; x++) {
            double u = (double)x / (double)rs;
            double theta = u * thetaLength + thetaStart;
            double sinTheta = sin(theta), cosTheta = cos(theta);
            setfltvalue(P, radius * sinTheta); setfltvalue(P + 1, -v * height + halfHeight);
            setfltvalue(P + 2, radius * cosTheta); P += 3;
            double n[3] = { sinTheta, slope, cosTheta };
            v3_normalize_d(n);
            setfltvalue(N, n[0]); setfltvalue(N + 1, n[1]); setfltvalue(N + 2, n[2]); N += 3;
            setfltvalue(U, u); setfltvalue(U + 1, 1 - v); U += 2;
        }
    }
    for (lua_Integer x = 1; x <= rs; x++)
        for (lua_Integer y = 1; y <= hs; y++) {
            /* indexArray[y][x] = (y - 1) * (rs + 1) + x - 1 */
            lua_Integer a = (y - 1) * (rs + 1) + x - 1, b = y * (rs + 1) + x - 1;
            lua_Integer c = y * (rs + 1) + x, d = (y - 1) * (rs + 1) + x;
            setivalue(I, a); setivalue(I + 1, b); setivalue(I + 2, d);
            setivalue(I + 3, b); setivalue(I + 4, c); setivalue(I + 5, d);
            I += 6;
        }
    lua_Integer index = (rs + 1) * (hs + 1);
    for (int cap = 0; cap < 2; cap++) {
        int isTop = cap == 0;
        if (isTop ? !top : !bottom) continue;
        double radius = isTop ? rt : rb;
        lua_Integer sign = isTop ? 1 : -1;
        lua_Integer centerIndexStart = index;
        for (lua_Integer k = 0; k < rs; k++) {
            setivalue(P, 0); setfltvalue(P + 1, halfHeight * (double)sign); setivalue(P + 2, 0); P += 3;
            setivalue(N, 0); setivalue(N + 1, sign); setivalue(N + 2, 0); N += 3;
            setfltvalue(U, 0.5); setfltvalue(U + 1, 0.5); U += 2;
            index++;
        }
        lua_Integer centerIndexEnd = index;
        for (lua_Integer x = 0; x <= rs; x++) {
            double u = (double)x / (double)rs;
            double theta = u * thetaLength + thetaStart;
            double cosTheta = cos(theta), sinTheta = sin(theta);
            setfltvalue(P, radius * sinTheta); setfltvalue(P + 1, halfHeight * (double)sign);
            setfltvalue(P + 2, radius * cosTheta); P += 3;
            setivalue(N, 0); setivalue(N + 1, sign); setivalue(N + 2, 0); N += 3;
            setfltvalue(U, (sinTheta * 0.5 * (double)sign) + 0.5); setfltvalue(U + 1, (cosTheta * 0.5) + 0.5); U += 2;
            index++;
        }
        for (lua_Integer x = 0; x < rs; x++) {
            lua_Integer c = centerIndexStart + x, i = centerIndexEnd + x;
            if (isTop) { setivalue(I, i); setivalue(I + 1, i + 1); setivalue(I + 2, c); }
            else { setivalue(I, i + 1); setivalue(I + 1, i); setivalue(I + 2, c); }
            I += 3;
        }
    }
    if (I != ti->array + ni || P != tp->array + nv * 3) return luaL_error(L, "t3.gen_cylinder: count");
    return 4;
}

/* t3.gen_box(width, height, depth, widthSegments, heightSegments,
 * depthSegments): the six buildPlane calls (positions and uvs floats,
 * normals the integers 0 / 1 / -1, as in Lua) */
static void box_plane(int u, int v, int w, double udir, double vdir, double width, double height, double depth,
                      lua_Integer gx, lua_Integer gy, TValue **P, TValue **N, TValue **U, TValue **I, lua_Integer *nverts) {
    double segWidth = width / (double)gx, segHeight = height / (double)gy;
    double widthHalf = width / 2, heightHalf = height / 2, depthHalf = depth / 2;
    lua_Integer nsign = depth > 0 ? 1 : -1;
    for (lua_Integer iy = 0; iy <= gy; iy++) {
        double y = (double)iy * segHeight - heightHalf;
        for (lua_Integer ix = 0; ix <= gx; ix++) {
            double x = (double)ix * segWidth - widthHalf;
            double vec[3];
            vec[u] = x * udir; vec[v] = y * vdir; vec[w] = depthHalf;
            setfltvalue(*P, vec[0]); setfltvalue(*P + 1, vec[1]); setfltvalue(*P + 2, vec[2]); *P += 3;
            lua_Integer nv3[3];
            nv3[u] = 0; nv3[v] = 0; nv3[w] = nsign;
            setivalue(*N, nv3[0]); setivalue(*N + 1, nv3[1]); setivalue(*N + 2, nv3[2]); *N += 3;
            setfltvalue(*U, (double)ix / (double)gx); setfltvalue(*U + 1, 1 - ((double)iy / (double)gy)); *U += 2;
        }
    }
    lua_Integer base = *nverts;
    for (lua_Integer iy = 0; iy < gy; iy++)
        for (lua_Integer ix = 0; ix < gx; ix++) {
            lua_Integer a = base + ix + (gx + 1) * iy, b = base + ix + (gx + 1) * (iy + 1);
            lua_Integer c = base + (ix + 1) + (gx + 1) * (iy + 1), d = base + (ix + 1) + (gx + 1) * iy;
            TValue *o = *I;
            setivalue(o, a); setivalue(o + 1, b); setivalue(o + 2, d);
            setivalue(o + 3, b); setivalue(o + 4, c); setivalue(o + 5, d);
            *I += 6;
        }
    *nverts += (gx + 1) * (gy + 1);
}
static int l_gen_box(lua_State *L) {
    for (int i = 1; i <= 6; i++) if (!lua_isnumber(L, i)) NO_KERNEL;
    for (int i = 4; i <= 6; i++) if (!lua_isinteger(L, i)) NO_KERNEL;
    double width = lua_tonumber(L, 1), height = lua_tonumber(L, 2), depth = lua_tonumber(L, 3);
    lua_Integer ws = lua_tointeger(L, 4), hs = lua_tointeger(L, 5), ds = lua_tointeger(L, 6);
    if (ws < 1 || hs < 1 || ds < 1 || ws > 4096 || hs > 4096 || ds > 4096) NO_KERNEL;
    lua_Integer nv = 2 * ((ds + 1) * (hs + 1) + (ws + 1) * (ds + 1) + (ws + 1) * (hs + 1));
    lua_Integer ni = 12 * (ds * hs + ws * ds + ws * hs);
    Table *ti = new_array(L, ni);
    Table *tp = new_array(L, nv * 3);
    Table *tn = new_array(L, nv * 3);
    Table *tu = new_array(L, nv * 2);
    TValue *P = tp->array, *N = tn->array, *U = tu->array, *I = ti->array;
    lua_Integer nverts = 0;
    enum { X, Y, Z };
    box_plane(Z, Y, X, -1, -1, depth, height, width, ds, hs, &P, &N, &U, &I, &nverts);
    box_plane(Z, Y, X, 1, -1, depth, height, -width, ds, hs, &P, &N, &U, &I, &nverts);
    box_plane(X, Z, Y, 1, 1, width, depth, height, ws, ds, &P, &N, &U, &I, &nverts);
    box_plane(X, Z, Y, 1, -1, width, depth, -height, ws, ds, &P, &N, &U, &I, &nverts);
    box_plane(X, Y, Z, 1, -1, width, height, depth, ws, hs, &P, &N, &U, &I, &nverts);
    box_plane(X, Y, Z, -1, -1, width, height, -depth, ws, hs, &P, &N, &U, &I, &nverts);
    if (I != ti->array + ni || nverts != nv) return luaL_error(L, "t3.gen_box: count");
    return 4;
}

/* t3.gen_polyhedron(vertices, indices, radius, detail) -> positions,
 * normals, uvs: subdivide, applyRadius, normals, generateUVs, correctUVs,
 * correctSeam (every value a float in Lua too) */
static int l_gen_polyhedron(lua_State *L) {
    if (!lua_istable(L, 1) || !lua_istable(L, 2) || !lua_isnumber(L, 3) || !lua_isinteger(L, 4)) NO_KERNEL;
    Table *vt = tab_at(L, 1), *it = tab_at(L, 2);
    double radius = lua_tonumber(L, 3);
    lua_Integer detail = lua_tointeger(L, 4);
    lua_Integer nidx = (lua_Integer)luaH_getn(it);
    if (detail < 0 || detail > 64 || nidx % 3) NO_KERNEL;
    lua_Integer cols = detail + 1, stride = cols + 1;
    lua_Integer nv = nidx / 3 * cols * cols * 3;
    if (nv > (1 << 22)) NO_KERNEL;
    double *g = (double *)malloc(sizeof(double) * 3 * (size_t)(stride * stride));
    double *vb = (double *)malloc(sizeof(double) * 3 * (size_t)(nv ? nv : 1));
    double *uv = (double *)malloc(sizeof(double) * 2 * (size_t)(nv ? nv : 1));
    if (!g || !vb || !uv) { free(g); free(vb); free(uv); NO_KERNEL; }
    lua_Integer nb = 0;
    for (lua_Integer f = 1; f <= nidx; f += 3) {
        double c3[3][3];
        for (int k = 0; k < 3; k++) {
            lua_Integer ix = tv_index(tab_geti(it, (int)(f + k)));
            if (ix < 0) goto fail;
            for (int d = 0; d < 3; d++)
                if (!tv_dbl(tab_geti(vt, (int)(ix * 3 + d + 1)), &c3[k][d])) goto fail;
        }
        double vax = c3[0][0], vay = c3[0][1], vaz = c3[0][2], vbx = c3[1][0], vby = c3[1][1], vbz = c3[1][2];
        double vcx = c3[2][0], vcy = c3[2][1], vcz = c3[2][2];
        for (lua_Integer i = 0; i <= cols; i++) {
            double fi = (double)i / (double)cols;
            double ajx = vax + (vcx - vax) * fi, ajy = vay + (vcy - vay) * fi, ajz = vaz + (vcz - vaz) * fi;
            double bjx = vbx + (vcx - vbx) * fi, bjy = vby + (vcy - vby) * fi, bjz = vbz + (vcz - vbz) * fi;
            lua_Integer rows = cols - i;
            for (lua_Integer j = 0; j <= rows; j++) {
                double *o = g + (i * stride + j) * 3;
                if (j == 0 && i == cols) { o[0] = ajx; o[1] = ajy; o[2] = ajz; }
                else {
                    double fj = (double)j / (double)rows;
                    o[0] = ajx + (bjx - ajx) * fj; o[1] = ajy + (bjy - ajy) * fj; o[2] = ajz + (bjz - ajz) * fj;
                }
            }
        }
#define EMIT(ii, jj) do { const double *o_ = g + ((ii) * stride + (jj)) * 3; \
        vb[nb * 3] = o_[0]; vb[nb * 3 + 1] = o_[1]; vb[nb * 3 + 2] = o_[2]; nb++; } while (0)
        for (lua_Integer i = 0; i <= cols - 1; i++)
            for (lua_Integer j = 0; j <= 2 * (cols - i) - 2; j++) {
                lua_Integer k = j / 2;
                if (j % 2 == 0) { EMIT(i, k + 1); EMIT(i + 1, k); EMIT(i, k); }
                else { EMIT(i, k + 1); EMIT(i + 1, k + 1); EMIT(i + 1, k); }
            }
#undef EMIT
    }
    if (nb != nv) goto fail;
    Table *tp = new_array(L, nv * 3);
    Table *tn = new_array(L, nv * 3);
    Table *tu = new_array(L, nv * 2);
    for (lua_Integer k = 0; k < nv; k++) {
        double x = vb[k * 3], y = vb[k * 3 + 1], z = vb[k * 3 + 2];
        double len = sqrt(x * x + y * y + z * z);
        if (len == 0) len = 1;
        double inv = 1 / len;
        x = x * inv * radius; y = y * inv * radius; z = z * inv * radius;
        vb[k * 3] = x; vb[k * 3 + 1] = y; vb[k * 3 + 2] = z;
        double nlen = sqrt(x * x + y * y + z * z);
        if (nlen == 0) nlen = 1;
        double ninv = 1 / nlen;
        setfltvalue(&tn->array[k * 3], x * ninv);
        setfltvalue(&tn->array[k * 3 + 1], y * ninv);
        setfltvalue(&tn->array[k * 3 + 2], z * ninv);
        uv[k * 2] = atan2(z, -x) / 2 / L_PI + 0.5;
        uv[k * 2 + 1] = 1 - (atan2(-y, sqrt(x * x + z * z)) / L_PI + 0.5);
    }
    double third = 1.0 / 3;
    for (lua_Integer f = 0, j = 0; f < nv; f += 3, j += 6) {
        double ax = vb[f * 3], az = vb[f * 3 + 2], bx = vb[f * 3 + 3], bz = vb[f * 3 + 5];
        double cx = vb[f * 3 + 6], cz = vb[f * 3 + 8];
        double azi = atan2((az + bz + cz) * third, -((ax + bx + cx) * third));
        double poleU = azi / 2 / L_PI + 0.5;
        double *s0 = uv + j;
        if (azi < 0 && s0[0] == 1) s0[0] = s0[0] - 1;
        if (ax == 0 && az == 0) s0[0] = poleU;
        if (azi < 0 && s0[2] == 1) s0[2] = s0[2] - 1;
        if (bx == 0 && bz == 0) s0[2] = poleU;
        if (azi < 0 && s0[4] == 1) s0[4] = s0[4] - 1;
        if (cx == 0 && cz == 0) s0[4] = poleU;
    }
    for (lua_Integer i = 0; i + 4 < nv * 2; i += 6) {
        double x0 = uv[i], x1 = uv[i + 2], x2 = uv[i + 4];
        double mx = x0; if (mx < x1) mx = x1; if (mx < x2) mx = x2;   /* math.max */
        double mn = x0; if (x1 < mn) mn = x1; if (x2 < mn) mn = x2;   /* math.min */
        if (mx > 0.9 && mn < 0.1) {
            if (x0 < 0.2) uv[i] = x0 + 1;
            if (x1 < 0.2) uv[i + 2] = x1 + 1;
            if (x2 < 0.2) uv[i + 4] = x2 + 1;
        }
    }
    for (lua_Integer k = 0; k < nv * 3; k++) setfltvalue(&tp->array[k], vb[k]);
    for (lua_Integer k = 0; k < nv * 2; k++) setfltvalue(&tu->array[k], uv[k]);
    free(g); free(vb); free(uv);
    return 3;
fail:
    free(g); free(vb); free(uv);
    NO_KERNEL;
}

/* ── curves ─────────────────────────────────────────────────────────────── */

static lua_Integer lua_imod(lua_Integer a, lua_Integer b) {   /* Lua's integer %: floored */
    lua_Integer m = a % b;
    if (m != 0 && (m ^ b) < 0) m += b;
    return m;
}
static int point3(Table *pts, lua_Integer i, double out[3]) {   /* points[i] as x, y, z numbers */
    Table *p = tv_tab(tab_geti(pts, (int)i));
    if (!p) return 0;
    for (int k = 0; k < 3; k++)
        if (!tv_dbl(luaH_getshortstr(p, fkey[FK_X + k]), &out[k])) return 0;
    return 1;
}
static double cr_nonuniform(double x0, double x1, double x2, double x3, double r0, double r1, double r2, double r01,
                            double r12, double dt1, double w) {
    double t1 = ((x1 - x0) * r0 - (x2 - x0) * r01 + (x2 - x1) * r1) * dt1;
    double t2 = ((x2 - x1) * r1 - (x3 - x1) * r12 + (x3 - x2) * r2) * dt1;
    double c2 = -3 * x1 + 3 * x2 - 2 * t1 - t2;
    double c3 = 2 * x1 - 2 * x2 + t1 + t2;
    double w2 = w * w;
    return x1 + t1 * w + c2 * w2 + c3 * w2 * w;
}
static double cr_uniform(double x0, double x1, double x2, double x3, double tension, double w) {
    double t1 = tension * (x2 - x0);
    double t2 = tension * (x3 - x1);
    double c2 = -3 * x1 + 3 * x2 - 2 * t1 - t2;
    double c3 = 2 * x1 - 2 * x2 + t1 + t2;
    return x1 + t1 * w + c2 * w * w + c3 * w * w * w;
}

/* t3.catmull_rom(points, closed, curveType, tension, t, endCache) -> x, y, z
 * | false: CatmullRomCurve3:getPoint's arithmetic. false when Lua must run
 * it: the end cache missing or stale (Lua builds it), a point that is not
 * plain numbers, or a uniform result Lua would keep integer. */
static int l_catmull_rom(lua_State *L) {
    if (!lua_istable(L, 1) || !lua_isnumber(L, 5)) NO_KERNEL;
    Table *pts = tab_at(L, 1);
    int closed = lua_toboolean(L, 2);
    const char *ct = lua_tostring(L, 3);
    int mode = !ct ? -1 : !strcmp(ct, "centripetal") ? 1 : !strcmp(ct, "chordal") ? 2 : 0;
    if (mode < 0) NO_KERNEL;
    lua_Integer l = (lua_Integer)luaH_getn(pts);
    if (l < 2) NO_KERNEL;
    int t_int = lua_isinteger(L, 5);
    double t = lua_tonumber(L, 5);
    double p = (double)(l - (closed ? 0 : 1)) * t;
    double fl = floor(p);
    if (!(fl > -1e15 && fl < 1e15)) NO_KERNEL;
    lua_Integer intPoint = (lua_Integer)fl;
    double weight = p - (double)intPoint;
    int weight_int = t_int;
    if (closed) {
        intPoint = intPoint + (intPoint > 0 ? 0 : ((lua_Integer)floor((double)llabs(intPoint) / (double)l) + 1) * l);
    } else if (weight == 0 && intPoint == l - 1) {
        intPoint = l - 2;
        weight = 1;
        weight_int = 1;
    }
    double p0[3], p1[3], p2[3], p3[3];
    Table *cache = NULL;
    int need_lo = !(closed || intPoint > 0), need_hi = !(closed || intPoint + 2 < l);
    if (need_lo || need_hi) {
        cache = lua_istable(L, 6) ? tab_at(L, 6) : NULL;
        if (!cache) NO_KERNEL;
        const TValue *cn = luaH_getshortstr(cache, anim_key[AK_N]);
        if (!ttisinteger(cn) || ivalue(cn) != l) NO_KERNEL;
    }
    if (need_lo) {
        Table *lo = tv_tab(luaH_getshortstr(cache, anim_key[AK_LO]));
        if (!lo) NO_KERNEL;
        for (int k = 0; k < 3; k++) if (!tv_dbl(luaH_getshortstr(lo, fkey[FK_X + k]), &p0[k])) NO_KERNEL;
    } else if (!point3(pts, lua_imod(intPoint - 1, l) + 1, p0)) NO_KERNEL;
    if (!point3(pts, lua_imod(intPoint, l) + 1, p1) || !point3(pts, lua_imod(intPoint + 1, l) + 1, p2)) NO_KERNEL;
    if (need_hi) {
        Table *hi = tv_tab(luaH_getshortstr(cache, anim_key[AK_HI]));
        if (!hi) NO_KERNEL;
        for (int k = 0; k < 3; k++) if (!tv_dbl(luaH_getshortstr(hi, fkey[FK_X + k]), &p3[k])) NO_KERNEL;
    } else if (!point3(pts, lua_imod(intPoint + 2, l) + 1, p3)) NO_KERNEL;
    double r[3];
    if (mode) {
        double pw = mode == 2 ? 0.5 : 0.25;
        double e1x = p1[0] - p2[0], e1y = p1[1] - p2[1], e1z = p1[2] - p2[2];
        double e0x = p0[0] - p1[0], e0y = p0[1] - p1[1], e0z = p0[2] - p1[2];
        double e2x = p2[0] - p3[0], e2y = p2[1] - p3[1], e2z = p2[2] - p3[2];
        double dt0 = pow(e0x * e0x + e0y * e0y + e0z * e0z, pw);
        double dt1 = pow(e1x * e1x + e1y * e1y + e1z * e1z, pw);
        double dt2 = pow(e2x * e2x + e2y * e2y + e2z * e2z, pw);
        if (dt1 < 1e-4) dt1 = 1.0;
        if (dt0 < 1e-4) dt0 = dt1;
        if (dt2 < 1e-4) dt2 = dt1;
        double r0 = 1 / dt0, r1 = 1 / dt1, r2 = 1 / dt2, r01 = 1 / (dt0 + dt1), r12 = 1 / (dt1 + dt2);
        for (int k = 0; k < 3; k++) r[k] = cr_nonuniform(p0[k], p1[k], p2[k], p3[k], r0, r1, r2, r01, r12, dt1, weight);
    } else {
        if (weight_int) NO_KERNEL;   /* integer points and tension could stay integers */
        double tension = lua_isnumber(L, 4) ? lua_tonumber(L, 4) : 0.5;
        for (int k = 0; k < 3; k++) r[k] = cr_uniform(p0[k], p1[k], p2[k], p3[k], tension, weight);
    }
    lua_pushnumber(L, r[0]); lua_pushnumber(L, r[1]); lua_pushnumber(L, r[2]);
    return 3;
}

/* t3.bezier3(t, v0, v1, v2, v3, dims) -> x, y[, z] | false: the cubic
 * Bezier getPoint, number subtypes as Lua's (integer t and points stay
 * integers) */
static void push_num(lua_State *L, num_t v) {
    if (v.i) lua_pushinteger(L, (lua_Integer)v.d);
    else lua_pushnumber(L, v.d);
}
static int l_bezier3(lua_State *L) {
    if (!lua_isnumber(L, 1)) NO_KERNEL;
    int dims = (int)luaL_checkinteger(L, 6);
    if (dims != 2 && dims != 3) NO_KERNEL;
    num_t t = lua_isinteger(L, 1) ? nint((double)lua_tointeger(L, 1)) : nflt(lua_tonumber(L, 1));
    num_t v[4][3];
    for (int i = 0; i < 4; i++) if (!get_fields(L, 2 + i, dims, v[i], NULL)) NO_KERNEL;
    num_bail = 0;
    num_t k = nsub(nint(1), t), three = nint(3);
    num_t r[3];
    for (int d = 0; d < dims; d++)
        r[d] = nadd(nadd(nadd(nmul(nmul(nmul(k, k), k), v[0][d]), nmul(nmul(nmul(nmul(three, k), k), t), v[1][d])),
                         nmul(nmul(nmul(nmul(three, k), t), t), v[2][d])),
                    nmul(nmul(nmul(t, t), t), v[3][d]));
    if (num_bail) NO_KERNEL;
    for (int d = 0; d < dims; d++) push_num(L, r[d]);
    return dims;
}

/* t3.quat_from_euler(x, y, z, order) -> qx, qy, qz, qw (double precision) */
static int l_quat_from_euler(lua_State *L) {
    double x = lua_tonumber(L, 1), y = lua_tonumber(L, 2), z = lua_tonumber(L, 3);
    int order = I(4);
    double c1 = cos(x * 0.5), c2 = cos(y * 0.5), c3 = cos(z * 0.5);
    double s1 = sin(x * 0.5), s2 = sin(y * 0.5), s3 = sin(z * 0.5);
    double r[4];
    switch (order) {
    default:
    case T3_XYZ: r[0] = s1*c2*c3 + c1*s2*s3; r[1] = c1*s2*c3 - s1*c2*s3; r[2] = c1*c2*s3 + s1*s2*c3; r[3] = c1*c2*c3 - s1*s2*s3; break;
    case T3_YXZ: r[0] = s1*c2*c3 + c1*s2*s3; r[1] = c1*s2*c3 - s1*c2*s3; r[2] = c1*c2*s3 - s1*s2*c3; r[3] = c1*c2*c3 + s1*s2*s3; break;
    case T3_ZXY: r[0] = s1*c2*c3 - c1*s2*s3; r[1] = c1*s2*c3 + s1*c2*s3; r[2] = c1*c2*s3 + s1*s2*c3; r[3] = c1*c2*c3 - s1*s2*s3; break;
    case T3_ZYX: r[0] = s1*c2*c3 - c1*s2*s3; r[1] = c1*s2*c3 + s1*c2*s3; r[2] = c1*c2*s3 - s1*s2*c3; r[3] = c1*c2*c3 + s1*s2*s3; break;
    case T3_YZX: r[0] = s1*c2*c3 + c1*s2*s3; r[1] = c1*s2*c3 + s1*c2*s3; r[2] = c1*c2*s3 - s1*s2*c3; r[3] = c1*c2*c3 - s1*s2*s3; break;
    case T3_XZY: r[0] = s1*c2*c3 - c1*s2*s3; r[1] = c1*s2*c3 - s1*c2*s3; r[2] = c1*c2*s3 + s1*s2*c3; r[3] = c1*c2*c3 + s1*s2*s3; break;
    }
    for (int i = 0; i < 4; i++) lua_pushnumber(L, r[i]);
    return 4;
}

/* t3.trs_many(n, h1, px.., h2, px.., ...): t3.trs for n objects in one
 * call (11 values each), so a frame's sync is a few crossings, not one per
 * object. */
static int l_trs_many(lua_State *L) {
    int n = I(1), a = 2;
    for (int k = 0; k < n; k++, a += 11) {
        t3_object *o = (t3_object *)P(L, a);
        o->position.x = F(a + 1); o->position.y = F(a + 2); o->position.z = F(a + 3);
        o->quaternion.x = F(a + 4); o->quaternion.y = F(a + 5); o->quaternion.z = F(a + 6);
        o->quaternion.w = F(a + 7);
        o->scale.x = F(a + 8); o->scale.y = F(a + 9); o->scale.z = F(a + 10);
    }
    return 0;
}

/* t3.flags(h, visible, frustumCulled, castShadow, receiveShadow, renderOrder,
 *          matrixAutoUpdate, layersMask, customRaycast) */
static int l_flags(lua_State *L) {
    t3_object *o = (t3_object *)P(L, 1);
    o->visible = B(2);
    o->frustum_culled = B(3);
    o->cast_shadow = B(4);
    o->receive_shadow = B(5);
    o->render_order = I(6);
    o->matrix_auto_update = B(7);
    if (!lua_isnoneornil(L, 8)) t3_object_set_layers(o, (uint32_t)lua_tointeger(L, 8));
    t3_object_set_raycast_custom(o, B(9));
    return 0;
}

/* t3.ptr(h) -> lightuserdata: the three.c object's address, which raycast
 * results name (three.lua maps it back to its object) */
static int l_ptr(lua_State *L) { lua_pushlightuserdata(L, P(L, 1)); return 1; }

/* ── raycasting ──────────────────────────────────────────────────────── */

static t3_raycaster *g_rc;
static t3_object **g_roots; static int g_roots_n, g_roots_cap;
static TString *key_t3;

typedef struct { lua_State *L; int others; int n; } other_ctx;
static void push_other(t3_object *o, void *p) {
    other_ctx *c = (other_ctx *)p;
    lua_pushlightuserdata(c->L, o);
    lua_rawseti(c->L, c->others, ++c->n);
}

/* t3.raycast(ox,oy,oz, dx,dy,dz, near, far, layers, objects, recursive, out, others)
 *   -> hits, others   (nil when some object in objects is not mirrored)
 * out gets 13 values per hit, nearest first: object (lightuserdata),
 * distance, world point xyz, local point xyz, faceIndex, a, b, c,
 * materialIndex. others gets the objects three.c left to Lua (their own
 * raycast), in traversal order. */
static int l_raycast(lua_State *L) {
    if (!g_rc) g_rc = t3_raycaster_new();
    t3_raycaster_set(g_rc, t3_v3(F(1), F(2), F(3)), t3_v3(F(4), F(5), F(6)));
    g_rc->near = F(7);
    g_rc->far = lua_tonumber(L, 8) > 3e38 ? INFINITY : F(8);
    g_rc->layers = (uint32_t)lua_tointeger(L, 9);
    Table *objs = tab_at(L, 10);
    int n = (int)lua_rawlen(L, 10);
    bool recursive = B(11);
    /* the roots, as three.c objects; the buffer is kept while the list is
     * the same so the raycaster's candidate cache (keyed by its address)
     * holds across the rays of a frame */
    t3_object **tmp = (t3_object **)malloc(sizeof(*tmp) * (size_t)(n > 0 ? n : 1));
    for (int i = 0; i < n; i++) {
        Table *o = tv_tab(tab_geti(objs, i + 1));
        const TValue *h = o ? luaH_getshortstr(o, key_t3) : NULL;
        tmp[i] = h ? (t3_object *)tv_handle(h) : NULL;
        if (!tmp[i]) { free(tmp); lua_pushnil(L); return 1; }
    }
    if (n != g_roots_n || memcmp(tmp, g_roots, sizeof(*tmp) * (size_t)n)) {
        free(g_roots);
        g_roots = tmp;
        g_roots_n = n;
    } else {
        free(tmp);
    }
    other_ctx ctx = { L, 13, 0 };
    const t3_intersection *hits;
    int nh = t3_raycaster_intersect_objects_ex(g_rc, g_roots, n, recursive, &hits, push_other, &ctx);
    for (int k = 0; k < nh; k++) {
        const t3_intersection *h = &hits[k];
        int b = k * 13;
        t3_mat4 inv;
        t3_mat4_invert(&inv, &h->object->matrix_world);
        t3_vec3 lp = t3_vec3_apply_mat4(h->point, &inv);
        lua_pushlightuserdata(L, h->object); lua_rawseti(L, 12, b + 1);
        lua_pushnumber(L, h->distance); lua_rawseti(L, 12, b + 2);
        lua_pushnumber(L, h->point.x); lua_rawseti(L, 12, b + 3);
        lua_pushnumber(L, h->point.y); lua_rawseti(L, 12, b + 4);
        lua_pushnumber(L, h->point.z); lua_rawseti(L, 12, b + 5);
        lua_pushnumber(L, lp.x); lua_rawseti(L, 12, b + 6);
        lua_pushnumber(L, lp.y); lua_rawseti(L, 12, b + 7);
        lua_pushnumber(L, lp.z); lua_rawseti(L, 12, b + 8);
        lua_pushinteger(L, h->face_index); lua_rawseti(L, 12, b + 9);
        lua_pushinteger(L, h->a); lua_rawseti(L, 12, b + 10);
        lua_pushinteger(L, h->b); lua_rawseti(L, 12, b + 11);
        lua_pushinteger(L, h->c); lua_rawseti(L, 12, b + 12);
        lua_pushinteger(L, h->material_index); lua_rawseti(L, 12, b + 13);
    }
    lua_pushinteger(L, nh);
    lua_pushinteger(L, ctx.n);
    return 2;
}

/* t3.visible(h, visible) */
static int l_visible(lua_State *L) { ((t3_object *)P(L, 1))->visible = B(2); return 0; }

/* t3.matrix(h, elements): the local matrix, for matrixAutoUpdate = false
 * (column-major, as Matrix4.elements). Marks the world matrix for update. */
static int l_matrix(lua_State *L) {
    t3_object *o = (t3_object *)P(L, 1);
    double e[16];
    Table *tb = tab_at(L, 2);
    for (int i = 0; i < 16; i++) e[i] = tab_num(tb, i + 1);
    t3_object_set_matrix_d(o, e);
    return 0;
}

static void fill(lua_State *L, int t, const float *e, int n) {
    Table *tb = tab_at(L, t);
    if (tb && tb->alimit >= (unsigned)n) {
        /* numbers into existing array slots: no allocation, no GC barrier */
        for (int i = 0; i < n; i++) setfltvalue(&tb->array[i], (lua_Number)e[i]);
        return;
    }
    for (int i = 0; i < n; i++) {
        lua_pushnumber(L, e[i]);
        lua_rawseti(L, t, i + 1);
    }
}

static void fill_d(lua_State *L, int t, const double *e, int n) {
    Table *tb = tab_at(L, t);
    if (tb && tb->alimit >= (unsigned)n) {
        for (int i = 0; i < n; i++) setfltvalue(&tb->array[i], e[i]);
        return;
    }
    for (int i = 0; i < n; i++) {
        lua_pushnumber(L, e[i]);
        lua_rawseti(L, t, i + 1);
    }
}

/* t3.world(h, elements) / t3.local(h, elements): copy matrixWorld / matrix
 * into a Lua array (Matrix4.elements): the double-precision ones (three.c
 * built with T3_F64_MATH keeps them), so Lua reads three.js's numbers. */
#ifdef T3_F64_MATH
static int l_world(lua_State *L) { fill_d(L, 2, ((t3_object *)P(L, 1))->_mwd, 16); return 0; }
static int l_local(lua_State *L) { fill_d(L, 2, ((t3_object *)P(L, 1))->_md, 16); return 0; }
#else
static int l_world(lua_State *L) { fill(L, 2, ((t3_object *)P(L, 1))->matrix_world.e, 16); return 0; }
static int l_local(lua_State *L) { fill(L, 2, ((t3_object *)P(L, 1))->matrix.e, 16); return 0; }
#endif
/* t3.camera_matrices(cam, worldInverse, projection, projectionInverse) */
static int l_camera_matrices(lua_State *L) {
    t3_camera *c = (t3_camera *)P(L, 1);
#ifdef T3_F64_MATH
    if (lua_istable(L, 2)) fill_d(L, 2, c->_mwid, 16);
#else
    if (lua_istable(L, 2)) fill(L, 2, c->matrix_world_inverse.e, 16);
#endif
    if (lua_istable(L, 3)) fill(L, 3, c->projection_matrix.e, 16);
    if (lua_istable(L, 4)) fill(L, 4, c->projection_matrix_inverse.e, 16);
    return 0;
}

/* t3.update_world(h, force) -> changed (any world matrix recomputed) */
static int l_update_world(lua_State *L) {
    uint32_t before = t3_world_recompute_count();
    t3_object_update_matrix_world(P(L, 1), B(2));
    lua_pushboolean(L, t3_world_recompute_count() != before);
    return 1;
}
/* t3.update_world_matrix(h, updateParents, updateChildren) -> changed */
static int l_update_world_matrix(lua_State *L) {
    uint32_t before = t3_world_recompute_count();
    t3_object_update_world_matrix(P(L, 1), B(2), B(3));
    lua_pushboolean(L, t3_world_recompute_count() != before);
    return 1;
}
/* t3.world_position(h) -> x, y, z (no update: call update_world_matrix first) */
static int l_world_position(lua_State *L) {
#ifdef T3_F64_MATH
    const double *e = ((t3_object *)P(L, 1))->_mwd;
#else
    const float *e = ((t3_object *)P(L, 1))->matrix_world.e;
#endif
    lua_pushnumber(L, e[12]); lua_pushnumber(L, e[13]); lua_pushnumber(L, e[14]);
    return 3;
}

/* ── cameras and lights ──────────────────────────────────────────────── */

static int l_perspective(lua_State *L) {
    return push_owned(L, t3_perspective_camera_new(F(1), F(2), F(3), F(4)));
}
static int l_orthographic(lua_State *L) {
    return push_owned(L, t3_orthographic_camera_new(F(1), F(2), F(3), F(4), F(5), F(6)));
}
/* t3.camera_set(cam, fov, aspect, near, far, zoom, left, right, top, bottom):
 * the projection parameters, then updateProjectionMatrix. */
static int l_camera_set(lua_State *L) {
    t3_camera *c = (t3_camera *)P(L, 1);
    c->fov = F(2); c->aspect = F(3); c->near = F(4); c->far = F(5); c->zoom = F(6);
    c->left = F(7); c->right = F(8); c->top = F(9); c->bottom = F(10);
    t3_camera_update_projection_matrix(c);
    return 0;
}
/* t3.camera_projection(cam, elements, inverseElements): a projection set by
 * hand (camera.projectionMatrix written directly). */
static int l_camera_projection(lua_State *L) {
    t3_camera *c = (t3_camera *)P(L, 1);
    for (int i = 0; i < 16; i++) {
        lua_rawgeti(L, 2, i + 1); c->projection_matrix.e[i] = (float)lua_tonumber(L, -1); lua_pop(L, 1);
        lua_rawgeti(L, 3, i + 1); c->projection_matrix_inverse.e[i] = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    }
    return 0;
}

/* t3.light(kind): "ambient", "hemisphere", "directional", "point", "spot" */
static int l_light(lua_State *L) {
    const char *k = luaL_checkstring(L, 1);
    void *o;
    if (!strcmp(k, "ambient")) o = t3_ambient_light_new(0xffffff, 1);
    else if (!strcmp(k, "hemisphere")) o = t3_hemisphere_light_new(0xffffff, 0xffffff, 1);
    else if (!strcmp(k, "directional")) o = t3_directional_light_new(0xffffff, 1);
    else if (!strcmp(k, "point")) o = t3_point_light_new(0xffffff, 1, 0, 1);
    else if (!strcmp(k, "spot")) o = t3_spot_light_new(0xffffff, 1, 0, 3.14159265f / 3, 0, 1);
    else return luaL_error(L, "t3.light: unknown kind %s", k);
    return push_owned(L, o);
}
/* t3.light_set(h, r,g,b, intensity, distance, decay, angle, penumbra, gr,gg,gb) */
static int l_light_set(lua_State *L) {
    t3_light *l = (t3_light *)P(L, 1);
    l->color = color_at(L, 2);
    l->intensity = F(5);
    l->distance = F(6);
    l->decay = F(7);
    l->angle = F(8);
    l->penumbra = F(9);
    if (!lua_isnoneornil(L, 10)) l->ground_color = color_at(L, 10);
    return 0;
}
/* t3.light_target(h, x, y, z): where a directional / spot light points, as
 * a world position (light.target's). three.c's target is not in the scene,
 * so its world matrix is its position. */
static int l_light_target(lua_State *L) {
    t3_light *l = (t3_light *)P(L, 1);
    if (!l->target) return 0;
    t3_object *t = l->target;
    t->position.x = F(2); t->position.y = F(3); t->position.z = F(4);
    t3_mat4_identity(&t->matrix_world);
    t->matrix_world.e[12] = t->position.x;
    t->matrix_world.e[13] = t->position.y;
    t->matrix_world.e[14] = t->position.z;
    t->_world_version++;
    return 0;
}
/* t3.light_shadow(h, mapW, mapH, bias, normalBias, radius, near, far,
 *                 left, right, top, bottom, fov) */
static int l_light_shadow(lua_State *L) {
    t3_light *l = (t3_light *)P(L, 1);
    t3_light_shadow *s = l->shadow;
    if (!s) return 0;
    s->map_width = I(2); s->map_height = I(3);
    s->bias = F(4); s->normal_bias = F(5); s->radius = F(6);
    t3_camera *c = s->camera;
    if (c) {
        c->near = F(7); c->far = F(8);
        if (!lua_isnoneornil(L, 9)) { c->left = F(9); c->right = F(10); c->top = F(11); c->bottom = F(12); }
        if (!lua_isnoneornil(L, 13)) c->fov = F(13);
        t3_camera_update_projection_matrix(c);
    }
    return 0;
}

/* ── scene ───────────────────────────────────────────────────────────── */

/* t3.scene_set(scene, hasBackground, r,g,b, fogType, fr,fg,fb, near, far, density, autoUpdate) */
static int l_scene_set(lua_State *L) {
    t3_scene *s = (t3_scene *)P(L, 1);
    s->has_background = B(2);
    if (s->has_background) s->background = color_at(L, 3);
    s->fog.type = (t3_fog_type)I(6);
    if (s->fog.type != T3_FOG_NONE) {
        s->fog.color = color_at(L, 7);
        s->fog.near = F(10); s->fog.far = F(11); s->fog.density = F(12);
    }
    if (!lua_isnoneornil(L, 13)) s->auto_update = B(13);
    return 0;
}

/* ── geometry ────────────────────────────────────────────────────────── */

static int l_geometry(lua_State *L) { return push_owned(L, t3_geometry_new()); }

/* Read a Lua number array (1-based) of n values into a new float array. */
static float *read_floats(lua_State *L, int t, int n) {
    float *a = (float *)malloc(sizeof(float) * (size_t)(n > 0 ? n : 1));
    Table *tb = tab_at(L, t);
    for (int i = 0; i < n; i++) a[i] = (float)tab_num(tb, i + 1);
    return a;
}

/* t3.geo_attr(g, slot, array, itemSize, normalized, dynamic): set (or
 * replace the data of) one attribute from a Lua array. slot is
 * t3_attr_slot's number. A same-size update rewrites the attribute in place
 * and bumps its version, which re-uploads it with glBufferSubData. */
static int l_geo_attr(lua_State *L) {
    t3_geometry *g = (t3_geometry *)P(L, 1);
    int slot = I(2);
    if (slot < 0 || slot >= T3_ATTR_COUNT) return luaL_error(L, "t3.geo_attr: bad slot %d", slot);
    if (lua_isnoneornil(L, 3)) { t3_geometry_set_attribute(g, (t3_attr_slot)slot, NULL); return 0; }
    int item = I(4);
    int n = (int)lua_rawlen(L, 3);
    int count = item > 0 ? n / item : 0;
    t3_attribute *cur = g->attributes[slot];
    if (cur && cur->type == T3_FLOAT32 && cur->count == count && cur->item_size == item) {
        float *dst = (float *)cur->array;
        Table *tb = tab_at(L, 3);
        for (int i = 0; i < count * item; i++) dst[i] = (float)tab_num(tb, i + 1);
        cur->normalized = B(5);
        if (B(6)) cur->usage = T3_DYNAMIC_DRAW;
        cur->version++;
        g->version++;
        return 0;
    }
    float *data = read_floats(L, 3, count * item);
    t3_attribute *a = t3_attribute_new(T3_FLOAT32, data, count, item);
    free(data);
    a->normalized = B(5);
    if (B(6)) a->usage = T3_DYNAMIC_DRAW;
    t3_geometry_set_attribute(g, (t3_attr_slot)slot, a);
    t3_release(a);
    g->version++;
    return 0;
}

/* t3.geo_index(g, array | nil): Lua 0-based vertex indices */
static int l_geo_index(lua_State *L) {
    t3_geometry *g = (t3_geometry *)P(L, 1);
    if (lua_isnoneornil(L, 2)) { t3_geometry_set_index(g, NULL); return 0; }
    int n = (int)lua_rawlen(L, 2);
    uint32_t maxv = 0;
    uint32_t *idx = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)(n > 0 ? n : 1));
    Table *tb = tab_at(L, 2);
    for (int i = 0; i < n; i++) {
        idx[i] = (uint32_t)tab_num(tb, i + 1);
        if (idx[i] > maxv) maxv = idx[i];
    }
    t3_attribute *a;
    if (maxv < 65535) {
        uint16_t *s = (uint16_t *)malloc(sizeof(uint16_t) * (size_t)(n > 0 ? n : 1));
        for (int i = 0; i < n; i++) s[i] = (uint16_t)idx[i];
        a = t3_attribute_new(T3_UINT16, s, n, 1);
        free(s);
    } else {
        a = t3_attribute_new(T3_UINT32, idx, n, 1);
    }
    free(idx);
    t3_geometry_set_index(g, a);
    t3_release(a);
    g->version++;
    return 0;
}

/* t3.geo_groups(g, start1, count1, mat1, start2, ...) (replaces) */
static int l_geo_groups(lua_State *L) {
    t3_geometry *g = (t3_geometry *)P(L, 1);
    free(g->groups);
    g->groups = NULL;
    g->group_count = 0;
    int n = (lua_gettop(L) - 1) / 3;
    for (int i = 0; i < n; i++) t3_geometry_add_group(g, I(2 + i * 3), I(3 + i * 3), I(4 + i * 3));
    return 0;
}

/* t3.geo_range(g, start, count): drawRange; count < 0 = all */
static int l_geo_range(lua_State *L) {
    t3_geometry *g = (t3_geometry *)P(L, 1);
    g->draw_start = I(2);
    g->draw_count = I(3);
    return 0;
}

/* t3.geo_bounds(g): recompute the bounding sphere and box from position */
static int l_geo_bounds(lua_State *L) {
    t3_geometry *g = (t3_geometry *)P(L, 1);
    t3_geometry_compute_bounding_box(g);
    t3_geometry_compute_bounding_sphere(g);
    return 0;
}

/* t3.geo_morph(g, relative, pos1, norm1|false, pos2, ...): morph targets */
static int l_geo_morph(lua_State *L) {
    t3_geometry *g = (t3_geometry *)P(L, 1);
    bool rel = B(2);
    int n = (lua_gettop(L) - 2) / 2;
    if (n <= 0) { t3_geometry_set_morph_attributes(g, NULL, NULL, 0, rel); return 0; }
    t3_attribute **pos = (t3_attribute **)calloc((size_t)n, sizeof(*pos));
    t3_attribute **nor = (t3_attribute **)calloc((size_t)n, sizeof(*nor));
    bool any_n = false;
    for (int i = 0; i < n; i++) {
        int pi = 3 + i * 2, ni = 4 + i * 2;
        int c = (int)lua_rawlen(L, pi);
        float *d = read_floats(L, pi, c);
        pos[i] = t3_attribute_new(T3_FLOAT32, d, c / 3, 3);
        free(d);
        if (lua_istable(L, ni)) {
            c = (int)lua_rawlen(L, ni);
            d = read_floats(L, ni, c);
            nor[i] = t3_attribute_new(T3_FLOAT32, d, c / 3, 3);
            free(d);
            any_n = true;
        }
    }
    t3_geometry_set_morph_attributes(g, pos, any_n ? nor : NULL, n, rel);
    for (int i = 0; i < n; i++) { t3_release(pos[i]); if (nor[i]) t3_release(nor[i]); }
    free(pos); free(nor);
    g->version++;
    return 0;
}

/* t3.morph_influences(mesh, w1, w2, ...) */
static int l_morph_influences(lua_State *L) {
    t3_mesh *m = (t3_mesh *)P(L, 1);
    int n = lua_gettop(L) - 1;
    if (m->morph_influence_count < n) t3_mesh_update_morph_targets(m);
    if (n > m->morph_influence_count) n = m->morph_influence_count;
    for (int i = 0; i < n; i++) m->morph_influences[i] = F(2 + i);
    return 0;
}

/* ── materials and textures ──────────────────────────────────────────── */

/* t3.material(type): t3_material_type's number */
static int l_material(lua_State *L) { return push_owned(L, t3_material_new((t3_material_type)I(1))); }

enum {
    MF_TRANSPARENT = 1, MF_DEPTH_TEST = 2, MF_DEPTH_WRITE = 4, MF_VERTEX_COLORS = 8, MF_FLAT = 16,
    MF_WIREFRAME = 32, MF_VISIBLE = 64, MF_FOG = 128, MF_SKINNING = 256, MF_MORPH = 512, MF_MORPH_NORMALS = 1024
};

/* t3.mat_set(h, cr,cg,cb, er,eg,eb, sr,sg,sb, opacity, shininess, roughness,
 *            metalness, emissiveIntensity, side, flags, alphaTest,
 *            aoMapIntensity, normalScaleX, normalScaleY,
 *            blending, premultipliedAlpha, blendSrc, blendDst, blendEquation,
 *            blendSrcAlpha, blendDstAlpha, blendEquationAlpha, depthFunc)
 * (the alpha factors -1 = null)
 * flags: MF_* bits. Bumps version (three.js needsUpdate) only when a field
 * that selects the program changed; colours and scalars are uniforms. */
static int l_mat_set(lua_State *L) {
    t3_material *m = (t3_material *)P(L, 1);
    m->color = color_at(L, 2);
    m->emissive = color_at(L, 5);
    m->specular = color_at(L, 8);
    m->opacity = F(11);
    m->shininess = F(12);
    m->roughness = F(13);
    m->metalness = F(14);
    m->emissive_intensity = F(15);
    int side = I(16), fl = I(17);
    float alpha_test = F(18);
    m->ao_map_intensity = F(19);
    m->normal_scale.x = F(20);
    m->normal_scale.y = F(21);
    bool tr = fl & MF_TRANSPARENT, dt = fl & MF_DEPTH_TEST, dw = fl & MF_DEPTH_WRITE, vc = fl & MF_VERTEX_COLORS;
    bool fs = fl & MF_FLAT, wf = fl & MF_WIREFRAME, vis = fl & MF_VISIBLE, fog = fl & MF_FOG;
    bool sk = fl & MF_SKINNING, mt = fl & MF_MORPH, mn = fl & MF_MORPH_NORMALS;
    if ((int)m->side != side || m->transparent != tr || m->depth_test != dt || m->depth_write != dw ||
        m->vertex_colors != vc || m->flat_shading != fs || m->wireframe != wf || m->fog != fog ||
        m->skinning != sk || m->morph_targets != mt || m->morph_normals != mn || m->alpha_test != alpha_test) {
        m->side = (t3_side)side;
        m->transparent = tr; m->depth_test = dt; m->depth_write = dw; m->vertex_colors = vc;
        m->flat_shading = fs; m->wireframe = wf; m->fog = fog;
        m->skinning = sk; m->morph_targets = mt; m->morph_normals = mn;
        m->alpha_test = alpha_test;
        m->version++;
    }
    m->visible = vis;
    if (!lua_isnoneornil(L, 22)) {
        m->blending = I(22);
        m->premultiplied_alpha = B(23);
        m->blend_src = I(24); m->blend_dst = I(25); m->blend_equation = I(26);
        m->blend_src_alpha = I(27); m->blend_dst_alpha = I(28); m->blend_equation_alpha = I(29);
        m->depth_func = I(30);
    }
    return 0;
}

/* t3.mat_map(h, slot, texture | nil): t3_map_slot's number */
static int l_mat_map(lua_State *L) {
    t3_material *m = (t3_material *)P(L, 1);
    t3_texture *t = (t3_texture *)Popt(L, 3);
    t3_texture **slotp = NULL;
    switch (I(2)) {
    case T3_MAP: slotp = &m->map; break;
    case T3_NORMAL_MAP: slotp = &m->normal_map; break;
    case T3_AO_MAP: slotp = &m->ao_map; break;
    case T3_EMISSIVE_MAP: slotp = &m->emissive_map; break;
    case T3_ROUGHNESS_MAP: slotp = &m->roughness_map; break;
    case T3_METALNESS_MAP: slotp = &m->metalness_map; break;
    case T3_ALPHA_MAP: slotp = &m->alpha_map; break;
    default: return luaL_error(L, "t3.mat_map: bad slot");
    }
    if (*slotp == t) return 0;
    t3_material_set_texture(m, (t3_map_slot)I(2), t);
    m->version++;
    return 0;
}

/* t3.texture(w, h, rgba, isData): from RGBA8 bytes (a Lua string, row 0 at
 * the top). isData: DataTexture defaults (no flipY, no mipmaps, nearest). */
static int l_texture(lua_State *L) {
    int w = I(1), h = I(2);
    size_t len = 0;
    const char *px = luaL_checklstring(L, 3, &len);
    if (len < (size_t)w * (size_t)h * 4) return luaL_error(L, "t3.texture: %d bytes for %dx%d", (int)len, w, h);
    t3_texture *t = B(4) ? t3_data_texture_new(w, h, (const uint8_t *)px) : t3_texture_new(w, h, (const uint8_t *)px);
    return push_owned(L, t);
}

const uint8_t *wcl_image_rgba(int id, int *w, int *h);

/* t3.texture_image(imageId, isData) -> h | nil: a texture of an engine
 * image's pixels (love Image / ImageData), read in place */
static int l_texture_image(lua_State *L) {
    int w = 0, h = 0;
    const uint8_t *px = wcl_image_rgba(I(1), &w, &h);
    if (!px) { lua_pushnil(L); return 1; }
    t3_texture *t = B(2) ? t3_data_texture_new(w, h, px) : t3_texture_new(w, h, px);
    return push_owned(L, t);
}

/* t3.tex_update_image(h, imageId) -> ok: new pixels from the image (same
 * size; false when it changed size, to make the texture anew) */
static int l_tex_update_image(lua_State *L) {
    t3_texture *t = (t3_texture *)P(L, 1);
    int w = 0, h = 0;
    const uint8_t *px = wcl_image_rgba(I(2), &w, &h);
    if (!px || w != t->width || h != t->height) { lua_pushboolean(L, 0); return 1; }
    memcpy(t->pixels, px, (size_t)w * (size_t)h * 4);
    t->version++;
    lua_pushboolean(L, 1);
    return 1;
}

/* t3.tex_set(h, wrapS, wrapT, magFilter, minFilter, flipY, generateMipmaps,
 *            encoding, offx, offy, repx, repy, cx, cy, rotation, anisotropy) */
static int l_tex_set(lua_State *L) {
    t3_texture *t = (t3_texture *)P(L, 1);
    t3_wrapping ws = (t3_wrapping)I(2), wt = (t3_wrapping)I(3);
    t3_filter mag = (t3_filter)I(4), min = (t3_filter)I(5);
    bool fy = B(6), gm = B(7);
    t3_encoding enc = (t3_encoding)I(8);
    if (t->wrap_s != ws || t->wrap_t != wt || t->mag_filter != mag || t->min_filter != min || t->flip_y != fy ||
        t->generate_mipmaps != gm || t->encoding != enc) {
        t->wrap_s = ws; t->wrap_t = wt; t->mag_filter = mag; t->min_filter = min;
        t->flip_y = fy; t->generate_mipmaps = gm; t->encoding = enc;
        t->version++;
    }
    t->offset.x = F(9); t->offset.y = F(10);
    t->repeat.x = F(11); t->repeat.y = F(12);
    t->center.x = F(13); t->center.y = F(14);
    t->rotation = F(15);
    if (!lua_isnoneornil(L, 16)) t->anisotropy = F(16);
    return 0;
}

/* t3.tex_update(h, rgba): new pixels, same size (texture.needsUpdate) */
static int l_tex_update(lua_State *L) {
    t3_texture *t = (t3_texture *)P(L, 1);
    size_t len = 0;
    const char *px = luaL_checklstring(L, 2, &len);
    size_t need = (size_t)t->width * (size_t)t->height * 4;
    if (len < need) return luaL_error(L, "t3.tex_update: short pixel data");
    memcpy(t->pixels, px, need);
    t->version++;
    return 0;
}

/* ── instancing ──────────────────────────────────────────────────────── */

/* t3.instance_matrices(h, elements, first, count): count matrices from a Lua
 * array (16 per instance, column-major, as instanceMatrix.array) starting
 * at instance `first` (0-based) */
static int l_instance_matrices(lua_State *L) {
    t3_instanced_mesh *im = (t3_instanced_mesh *)P(L, 1);
    int first = (int)luaL_optinteger(L, 3, 0);
    int count = (int)luaL_optinteger(L, 4, im->count - first);
    if (first < 0 || first + count > im->count) return luaL_error(L, "t3.instance_matrices: range");
    float *dst = (float *)im->instance_matrix->array + first * 16;
    for (int i = 0; i < count * 16; i++) {
        lua_rawgeti(L, 2, first * 16 + i + 1);
        dst[i] = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
    im->instance_matrix->version++;
    return 0;
}
static float *inst_slot(lua_State *L, t3_instanced_mesh *im, int i) {
    if (i < 0 || i >= im->instance_matrix->count) {
        luaL_error(L, "t3: instance %d out of range (%d)", i, im->instance_matrix->count);
        return NULL;
    }
    im->instance_matrix->version++;
    return (float *)im->instance_matrix->array + i * 16;
}

/* t3.instance_rows(h, rows, first, count): from three.lua's row-major
 * instanceData (16 per instance, transposed relative to Matrix4.elements) */
static int l_instance_rows(lua_State *L) {
    t3_instanced_mesh *im = (t3_instanced_mesh *)P(L, 1);
    int first = (int)luaL_optinteger(L, 3, 0);
    int count = (int)luaL_optinteger(L, 4, im->instance_matrix->count - first);
    if (first < 0 || first + count > im->instance_matrix->count) return luaL_error(L, "t3.instance_rows: range");
    float *dst = (float *)im->instance_matrix->array;
    Table *tb = tab_at(L, 2);
    for (int k = 0; k < count; k++) {
        int base = (first + k) * 16;
        for (int j = 0; j < 16; j++) {
            int row = j / 4, col = j % 4;
            dst[base + col * 4 + row] = (float)tab_num(tb, base + j + 1);
        }
    }
    im->instance_matrix->version++;
    return 0;
}

/* t3.instance_set(h, i, e1..e16): setMatrixAt (Matrix4.elements order) */
static int l_instance_set(lua_State *L) {
    t3_instanced_mesh *im = (t3_instanced_mesh *)P(L, 1);
    float *d = inst_slot(L, im, I(2));
    for (int j = 0; j < 16; j++) d[j] = F(3 + j);
    return 0;
}

/* t3.instance_trs(h, i, px,py,pz, qx,qy,qz,qw, sx,sy,sz): setTransformAt */
static int l_instance_trs(lua_State *L) {
    t3_instanced_mesh *im = (t3_instanced_mesh *)P(L, 1);
    float *d = inst_slot(L, im, I(2));
    t3_quat q = { F(6), F(7), F(8), F(9) };
    t3_mat4_compose((t3_mat4 *)d, t3_v3(F(3), F(4), F(5)), q, t3_v3(F(10), F(11), F(12)));
    return 0;
}

/* t3.instance_euler(h, i, px,py,pz, ex,ey,ez, sx,sy,sz, order): composeAt */
static int l_instance_euler(lua_State *L) {
    t3_instanced_mesh *im = (t3_instanced_mesh *)P(L, 1);
    float *d = inst_slot(L, im, I(2));
    t3_euler e = { F(6), F(7), F(8), (t3_euler_order)I(12) };
    t3_mat4_compose((t3_mat4 *)d, t3_v3(F(3), F(4), F(5)), t3_quat_from_euler(e), t3_v3(F(9), F(10), F(11)));
    return 0;
}

/* t3.instance_euler_range(h, first, count, src, order): composeRange, src
 * holding px,py,pz, ex,ey,ez, sx,sy,sz per instance */
static int l_instance_euler_range(lua_State *L) {
    t3_instanced_mesh *im = (t3_instanced_mesh *)P(L, 1);
    int first = I(2), count = I(3);
    t3_euler_order order = (t3_euler_order)luaL_optinteger(L, 5, 0);
    if (first < 0 || first + count > im->instance_matrix->count) return luaL_error(L, "t3.instance_euler_range: range");
    float *base = (float *)im->instance_matrix->array;
    float v[9];
    Table *tb = tab_at(L, 4);
    for (int k = 0; k < count; k++) {
        for (int j = 0; j < 9; j++) v[j] = (float)tab_num(tb, k * 9 + j + 1);
        t3_euler e = { v[3], v[4], v[5], order };
        t3_mat4_compose((t3_mat4 *)(base + (first + k) * 16), t3_v3(v[0], v[1], v[2]), t3_quat_from_euler(e),
                        t3_v3(v[6], v[7], v[8]));
    }
    im->instance_matrix->version++;
    return 0;
}

/* t3.instance_get(h, i, elements): getMatrixAt into a Matrix4.elements */
static int l_instance_get(lua_State *L) {
    t3_instanced_mesh *im = (t3_instanced_mesh *)P(L, 1);
    int i = I(2);
    if (i < 0 || i >= im->instance_matrix->count) return luaL_error(L, "t3.instance_get: range");
    fill(L, 3, (const float *)im->instance_matrix->array + i * 16, 16);
    return 0;
}

/* t3.instance_element(h, k [, v]): one element of instanceMatrix.array
 * (0-based, Matrix4 order per instance), read or written */
static int l_instance_element(lua_State *L) {
    t3_instanced_mesh *im = (t3_instanced_mesh *)P(L, 1);
    int k = I(2);
    if (k < 0 || k >= im->instance_matrix->count * 16) { lua_pushnil(L); return 1; }
    float *d = (float *)im->instance_matrix->array;
    if (lua_gettop(L) >= 3) { d[k] = F(3); im->instance_matrix->version++; return 0; }
    lua_pushnumber(L, d[k]);
    return 1;
}

/* t3.instance_color(h, i, r, g, b): setColorAt */
static int l_instance_color(lua_State *L) {
    t3_instanced_mesh *im = (t3_instanced_mesh *)P(L, 1);
    t3_instanced_mesh_set_color_at(im, I(2), color_at(L, 3));
    im->instance_color->version++;
    return 0;
}

/* t3.instance_count(h, count): mesh.count (drawn instances, <= capacity) */
static int l_instance_count(lua_State *L) {
    t3_instanced_mesh *im = (t3_instanced_mesh *)P(L, 1);
    int n = I(2);
    if (n >= 0 && n <= im->instance_matrix->count) im->count = n;
    return 0;
}
/* t3.instance_colors(h, rgb array (3 per instance)) */
static int l_instance_colors(lua_State *L) {
    t3_instanced_mesh *im = (t3_instanced_mesh *)P(L, 1);
    int n = (int)lua_rawlen(L, 2) / 3;
    for (int i = 0; i < n && i < im->instance_matrix->count; i++) {
        t3_color c;
        lua_rawgeti(L, 2, i * 3 + 1); c.r = (float)lua_tonumber(L, -1);
        lua_rawgeti(L, 2, i * 3 + 2); c.g = (float)lua_tonumber(L, -1);
        lua_rawgeti(L, 2, i * 3 + 3); c.b = (float)lua_tonumber(L, -1);
        lua_pop(L, 3);
        t3_instanced_mesh_set_color_at(im, i, c);
    }
    if (im->instance_color) im->instance_color->version++;
    return 0;
}

/* ── renderer ────────────────────────────────────────────────────────── */

typedef struct { t3_renderer *r; } rbox_t;
static int renderer_gc(lua_State *L) {
    rbox_t *b = (rbox_t *)lua_touserdata(L, 1);
    if (b && b->r) { t3_renderer_destroy(b->r); b->r = NULL; }
    return 0;
}

/* t3.renderer(w, h) */
static int l_renderer(lua_State *L) {
    if (!wcl_r2d_active()) return luaL_error(L, "t3.renderer: needs the GL engine (this frame is on the CPU path)");
    wcl_r2d__flush_for_external_gl();
    t3_renderer *r = t3_renderer_new(I(1), I(2));
    wcl_r2d__after_external_gl();
    if (!r) { lua_pushnil(L); return 1; }
    rbox_t *b = (rbox_t *)lua_newuserdatauv(L, sizeof(rbox_t), 0);
    b->r = r;
    luaL_setmetatable(L, "t3.renderer");
    return 1;
}
static t3_renderer *R(lua_State *L, int i) {
    rbox_t *b = (rbox_t *)lua_touserdata(L, i);
    return b ? b->r : NULL;
}

/* t3.renderer_set(r, clearHex, clearAlpha, autoClear, shadowsOn, shadowType,
 *                 outputEncoding, antialiasSamples) */
static int l_renderer_set(lua_State *L) {
    t3_renderer *r = R(L, 1);
    t3_renderer_set_clear_color(r, (uint32_t)lua_tointeger(L, 2), F(3));
    t3_renderer_set_auto_clear(r, B(4));
    t3_renderer_set_shadow_map(r, B(5), (t3_shadow_map_type)I(6));
    t3_renderer_set_output_encoding(r, (t3_encoding)I(7));
    if (!lua_isnoneornil(L, 8)) t3_renderer_set_antialias(r, I(8));
    return 0;
}

/* t3.renderer_size(r, w, h) */
static int l_renderer_size(lua_State *L) { t3_renderer_set_size(R(L, 1), I(2), I(3)); return 0; }

/* t3.renderer_viewport(r, x, y, w, h [, scissorX, scissorY, scissorW, scissorH | nil])
 * in the renderer's drawing-buffer pixels, bottom-left origin */
static int l_renderer_viewport(lua_State *L) {
    t3_renderer *r = R(L, 1);
    t3_renderer_set_viewport(r, I(2), I(3), I(4), I(5));
    if (lua_isnoneornil(L, 6)) t3_renderer_set_scissor_test(r, false);
    else {
        t3_renderer_set_scissor(r, I(6), I(7), I(8), I(9));
        t3_renderer_set_scissor_test(r, true);
    }
    return 0;
}

/* Aim the renderer where the engine is drawing: the screen or the bound
 * canvas. three.c draws into "the screen" with its own size and viewport, so
 * those come from the cart (renderer_size / renderer_viewport); only the
 * framebuffer is the engine's. */
static void aim(t3_renderer *r) {
    unsigned fbo; int x, y, w, h;
    wcl_r2d__current_target(&fbo, &x, &y, &w, &h);
    t3_renderer_set_framebuffer(r, fbo);
    take_over(r);
}

/* t3.render(r, scene, camera) */
static int l_render(lua_State *L) {
    t3_renderer *r = R(L, 1);
    if (!r || !wcl_r2d_active()) return 0;
    wcl_r2d__flush_for_external_gl();
    aim(r);
    uint32_t before = t3_world_recompute_count();
    t3_renderer_render(r, (t3_scene *)P(L, 2), (t3_camera *)P(L, 3));
    hand_back(r);
    /* the frame's numbers, so render does not need a second call for them */
    const t3_render_info *info = t3_renderer_info(r);
    lua_pushinteger(L, info->calls);
    lua_pushinteger(L, info->triangles);
    lua_pushboolean(L, t3_renderer_last_error(r) != NULL);
    lua_pushboolean(L, t3_world_recompute_count() != before);
    return 4;
}

/* t3.clear(r, color, depth, stencil) */
static int l_clear(lua_State *L) {
    t3_renderer *r = R(L, 1);
    if (!r || !wcl_r2d_active()) return 0;
    wcl_r2d__flush_for_external_gl();
    aim(r);
    t3_renderer_clear(r, B(2), B(3), B(4));
    hand_back(r);
    return 0;
}

/* t3.info(r) -> frame, calls, triangles, programs, geometries, textures */
static int l_info(lua_State *L) {
    const t3_render_info *i = t3_renderer_info(R(L, 1));
    lua_pushinteger(L, i->frame); lua_pushinteger(L, i->calls); lua_pushinteger(L, i->triangles);
    lua_pushinteger(L, i->programs); lua_pushinteger(L, i->geometries); lua_pushinteger(L, i->textures);
    return 6;
}

/* t3.last_error(r) -> string | nil */
static int l_last_error(lua_State *L) {
    const char *e = t3_renderer_last_error(R(L, 1));
    if (e) lua_pushstring(L, e); else lua_pushnil(L);
    return 1;
}

/* t3.release(h): drop the reference now, ahead of collection (dispose) */
static int l_release(lua_State *L) { return handle_gc(L); }

static const luaL_Reg t3_lib[] = {
    { "object", l_object }, { "mesh", l_mesh }, { "instanced", l_instanced }, { "mesh_set", l_mesh_set },
    { "add", l_add }, { "remove", l_remove },
    { "trs", l_trs }, { "flush", l_flush }, { "anim_step", l_anim_step }, { "m4_copy", l_m4_copy }, { "m4_compose", l_m4_compose }, { "v3_apply_m4", l_v3_apply_m4 }, { "v3_apply_q", l_v3_apply_q }, { "q_from_euler", l_q_from_euler }, { "q_slerp", l_q_slerp }, { "m4_mul", l_m4_mul }, { "m4_invert", l_m4_invert }, { "anim_apply", l_anim_apply }, { "anim_register", l_anim_register }, { "anim_register_more", l_anim_register_more }, { "deindex", l_deindex }, { "gen_sphere", l_gen_sphere }, { "gen_torus", l_gen_torus }, { "catmull_rom", l_catmull_rom }, { "bezier3", l_bezier3 }, { "gen_torus_knot", l_gen_torus_knot }, { "gen_cylinder", l_gen_cylinder }, { "gen_box", l_gen_box }, { "gen_polyhedron", l_gen_polyhedron }, { "vertex_normals", l_vertex_normals }, { "ptr", l_ptr }, { "raycast", l_raycast }, { "trs_euler", l_trs_euler }, { "quat_from_euler", l_quat_from_euler }, { "trs_many", l_trs_many }, { "flags", l_flags }, { "visible", l_visible },
    { "matrix", l_matrix },
    { "world", l_world }, { "local_matrix", l_local }, { "camera_matrices", l_camera_matrices },
    { "update_world", l_update_world }, { "update_world_matrix", l_update_world_matrix },
    { "world_position", l_world_position },
    { "perspective", l_perspective }, { "orthographic", l_orthographic }, { "camera_set", l_camera_set },
    { "camera_projection", l_camera_projection },
    { "light", l_light }, { "light_set", l_light_set }, { "light_target", l_light_target },
    { "light_shadow", l_light_shadow },
    { "scene_set", l_scene_set },
    { "geometry", l_geometry }, { "geo_attr", l_geo_attr }, { "geo_index", l_geo_index },
    { "geo_groups", l_geo_groups }, { "geo_range", l_geo_range }, { "geo_bounds", l_geo_bounds },
    { "geo_morph", l_geo_morph }, { "morph_influences", l_morph_influences },
    { "material", l_material }, { "mat_set", l_mat_set }, { "mat_map", l_mat_map },
    { "texture", l_texture }, { "tex_set", l_tex_set }, { "tex_update", l_tex_update },
    { "texture_image", l_texture_image }, { "tex_update_image", l_tex_update_image },
    { "instance_matrices", l_instance_matrices }, { "instance_count", l_instance_count },
    { "instance_colors", l_instance_colors }, { "instance_rows", l_instance_rows },
    { "instance_set", l_instance_set }, { "instance_trs", l_instance_trs }, { "instance_euler", l_instance_euler },
    { "instance_euler_range", l_instance_euler_range }, { "instance_get", l_instance_get },
    { "instance_element", l_instance_element }, { "instance_color", l_instance_color },
    { "renderer", l_renderer }, { "renderer_set", l_renderer_set }, { "renderer_size", l_renderer_size },
    { "renderer_viewport", l_renderer_viewport },
    { "render", l_render }, { "clear", l_clear }, { "info", l_info }, { "last_error", l_last_error },
    { "release", l_release },
    { NULL, NULL }
};

void wcl_open_three_c(lua_State *L) {
    static const char *const names[K_COUNT] = { "_x", "_y", "_z", "_order" };
    for (int i = 0; i < K_COUNT; i++) {
        lua_pushstring(L, names[i]);
        key_ts[i] = (TString *)lua_topointer(L, -1);   /* short strings are interned: the same object Lua uses */
        key_ref[i] = luaL_ref(L, LUA_REGISTRYINDEX);  /* and kept alive */
    }
    lua_pushstring(L, "_t3");
    key_t3 = (TString *)lua_topointer(L, -1);
    luaL_ref(L, LUA_REGISTRYINDEX);
    static const char *const anames[AK_COUNT] = { "parameterPositions", "sampleValues", "valueSize", "_cachedIndex",
                                                  "buffer", "cumulativeWeight", "resultBuffer", "_t3mk",
                                                  "cumulativeWeightAdditive", "binding", "setValue",
                                                  "resolvedProperty", "_onChange", "x", "y", "z", "w",
                                                  "targetObject", "matrixWorldNeedsUpdate", "n", "lo", "hi", "_rec", "propertyName" };
    for (int i = 0; i < AK_COUNT; i++) {
        lua_pushstring(L, anames[i]);
        anim_key[i] = (TString *)lua_topointer(L, -1);
        luaL_ref(L, LUA_REGISTRYINDEX);
    }
    static const char *const fnames[FK_COUNT] = { "x", "y", "z", "w", "_x", "_y", "_z", "_order" };
    for (int i = 0; i < FK_COUNT; i++) {
        lua_pushstring(L, fnames[i]);
        fkey[i] = (TString *)lua_topointer(L, -1);
        luaL_ref(L, LUA_REGISTRYINDEX);
    }
    luaL_newmetatable(L, HANDLE_MT);
    lua_pushcfunction(L, handle_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
    luaL_newmetatable(L, "t3.renderer");
    lua_pushcfunction(L, renderer_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
    luaL_newlib(L, t3_lib);
    lua_pushstring(L, T3_REVISION);
    lua_setfield(L, -2, "REVISION");
    lua_setglobal(L, "t3");
}

#endif /* WCL_THREE_C */
