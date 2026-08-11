/* physics3d.c — Box3D bound as the Lua global `b3`.
 *
 * Deliberately shaped like physics.c (Box2D as `b2`): integer handles into
 * fixed slot tables rather than userdata, a flat function table, and the
 * same meter convention. A cart that knows one knows the other.
 *
 * Differences that are real, not stylistic:
 *   - positions are DOUBLE (b3Pos) while rotations are float quaternions.
 *     That is Box3D's own split, for worlds bigger than float can address.
 *   - rotation is a quaternion (x,y,z,w), not an angle. Helpers take an
 *     axis+angle so a cart never has to build one by hand.
 *   - there is no love.physics equivalent to imitate: LOVE has no 3D
 *     physics, so this surface is Box3D's own vocabulary rather than a
 *     LOVE-shaped wrapper.
 *
 * Threads and SIMD are build-time properties: Box3D is compiled with the
 * widest SIMD its target supports, and worlds are created with the shared
 * worker pool (wc_taskpool) when the build has threads. b3.info() reports
 * both so a cart — or a test — can assert on what it actually got.
 */
#include "box3d/box3d.h"
#include "wc_taskpool.h"

#include "lua.h"
#include "lauxlib.h"

#include <string.h>
#include <math.h>

#define MAX_WORLDS 4
#define MAX_BODIES 2048
#define MAX_SHAPES 4096

static b3WorldId worlds[MAX_WORLDS];
static int       world_alive[MAX_WORLDS];

typedef struct { b3BodyId id; int world; int active; } body_slot;
typedef struct { b3ShapeId id; int body;  int active; } shape_slot;

static body_slot  bodies[MAX_BODIES];
static shape_slot shapes[MAX_SHAPES];

/* Same convention as the 2D binding: carts think in pixels, the solver
 * thinks in meters, and one number converts between them. */
static double pixels_per_meter = 64.0;

static inline float  px2m(double px) { return (float)(px / pixels_per_meter); }
static inline double m2px(double m)  { return m * pixels_per_meter; }

static b3Vec3 vec3_arg(lua_State *L, int i) {
    b3Vec3 v;
    v.x = px2m(luaL_optnumber(L, i, 0));
    v.y = px2m(luaL_optnumber(L, i + 1, 0));
    v.z = px2m(luaL_optnumber(L, i + 2, 0));
    return v;
}
static b3Pos pos_arg(lua_State *L, int i) {
    b3Pos p;
    p.x = px2m(luaL_optnumber(L, i, 0));
    p.y = px2m(luaL_optnumber(L, i + 1, 0));
    p.z = px2m(luaL_optnumber(L, i + 2, 0));
    return p;
}

static body_slot *body_at(lua_State *L, int i) {
    int h = (int)luaL_checkinteger(L, i);
    if (h < 1 || h > MAX_BODIES || !bodies[h - 1].active)
        luaL_error(L, "b3: bad body handle %d", h);
    return &bodies[h - 1];
}

/* ── world ──────────────────────────────────────────────────────────── */

/* b3.world_new(gx, gy, gz [, workers]) -> handle
 * workers: nil = every hardware thread, 1 = serial, N = that many. */
static int l_world_new(lua_State *L) {
    b3Vec3 g = vec3_arg(L, 1);
    int slot = -1;
    for (int i = 0; i < MAX_WORLDS; i++) if (!world_alive[i]) { slot = i; break; }
    if (slot < 0) return luaL_error(L, "b3: too many worlds (max %d)", MAX_WORLDS);

    int want = (int)luaL_optinteger(L, 4, wc_taskpool_hw_threads());
    int workers = wc_taskpool_init(want);

    b3WorldDef def = b3DefaultWorldDef();
    def.gravity = g;
    if (workers > 0) {
        /* Box3D forks its solver across these. With no pool the callbacks
         * run the task inline and return NULL, which Box3D reads as "done
         * serially" — so this is safe to wire unconditionally. */
        def.workerCount     = (uint32_t)workers;
        def.enqueueTask     = (b3EnqueueTaskCallback *)wc_taskpool_enqueue;
        def.finishTask      = (b3FinishTaskCallback *)wc_taskpool_finish;
        def.userTaskContext = NULL;
    }
    worlds[slot] = b3CreateWorld(&def);
    world_alive[slot] = 1;
    lua_pushinteger(L, slot + 1);
    return 1;
}

static int l_world_destroy(lua_State *L) {
    int h = (int)luaL_checkinteger(L, 1);
    if (h < 1 || h > MAX_WORLDS || !world_alive[h - 1]) return 0;
    for (int i = 0; i < MAX_BODIES; i++)
        if (bodies[i].active && bodies[i].world == h) bodies[i].active = 0;
    for (int i = 0; i < MAX_SHAPES; i++)
        if (shapes[i].active && shapes[i].body >= 1 &&
            !bodies[shapes[i].body - 1].active) shapes[i].active = 0;
    b3DestroyWorld(worlds[h - 1]);
    world_alive[h - 1] = 0;
    return 0;
}

/* b3.world_step(world, dt [, subSteps]) */
static int l_world_step(lua_State *L) {
    int h = (int)luaL_checkinteger(L, 1);
    if (h < 1 || h > MAX_WORLDS || !world_alive[h - 1])
        return luaL_error(L, "b3: bad world handle %d", h);
    float dt = (float)luaL_checknumber(L, 2);
    int sub = (int)luaL_optinteger(L, 3, 4);
    b3World_Step(worlds[h - 1], dt, sub);
    return 0;
}

static int l_set_meter(lua_State *L) {
    double m = luaL_checknumber(L, 1);
    if (m > 0) pixels_per_meter = m;
    return 0;
}
static int l_get_meter(lua_State *L) { lua_pushnumber(L, pixels_per_meter); return 1; }

/* b3.info() -> { workers=, simd=, threads= } — what this BUILD actually got,
 * not what it asked for. Tests assert on it. */
static int l_info(lua_State *L) {
    lua_newtable(L);
    lua_pushinteger(L, wc_taskpool_workers());   lua_setfield(L, -2, "workers");
    lua_pushinteger(L, wc_taskpool_hw_threads()); lua_setfield(L, -2, "hw_threads");
#if defined(__wasm__) && !defined(__EMSCRIPTEN_PTHREADS__)
    lua_pushboolean(L, 0);
#else
    lua_pushboolean(L, 1);
#endif
    lua_setfield(L, -2, "threads");
    lua_pushstring(L, WC_PHYSICS_SIMD); lua_setfield(L, -2, "simd");
    return 1;
}

/* ── bodies ─────────────────────────────────────────────────────────── */

/* b3.body_new(world, x, y, z [, type])  type: 0 static, 1 kinematic,
 * 2 dynamic (default). Argument ORDER matches b2.body_new(world, x, y,
 * type) deliberately -- position first, type last and optional. Getting
 * this backwards is a silent bug, not an error: every argument is a
 * number, so a swapped type reads as a coordinate and the body quietly
 * becomes the wrong kind in the wrong place. */
static int l_body_new(lua_State *L) {
    int wh = (int)luaL_checkinteger(L, 1);
    if (wh < 1 || wh > MAX_WORLDS || !world_alive[wh - 1])
        return luaL_error(L, "b3: bad world handle %d", wh);
    int type = (int)luaL_optinteger(L, 5, 2);

    int slot = -1;
    for (int i = 0; i < MAX_BODIES; i++) if (!bodies[i].active) { slot = i; break; }
    if (slot < 0) return luaL_error(L, "b3: too many bodies (max %d)", MAX_BODIES);

    b3BodyDef def = b3DefaultBodyDef();
    def.type = (b3BodyType)(type < 0 ? 0 : (type > 2 ? 2 : type));
    def.position = pos_arg(L, 2);
    bodies[slot].id     = b3CreateBody(worlds[wh - 1], &def);
    bodies[slot].world  = wh;
    bodies[slot].active = 1;
    lua_pushinteger(L, slot + 1);
    return 1;
}

static int l_body_destroy(lua_State *L) {
    body_slot *b = body_at(L, 1);
    b3DestroyBody(b->id);
    b->active = 0;
    return 0;
}

static int l_body_position(lua_State *L) {
    b3Pos p = b3Body_GetPosition(body_at(L, 1)->id);
    lua_pushnumber(L, m2px(p.x));
    lua_pushnumber(L, m2px(p.y));
    lua_pushnumber(L, m2px(p.z));
    return 3;
}

/* rotation as a quaternion (x, y, z, w) */
static int l_body_rotation(lua_State *L) {
    b3Quat q = b3Body_GetRotation(body_at(L, 1)->id);
    lua_pushnumber(L, q.v.x); lua_pushnumber(L, q.v.y);
    lua_pushnumber(L, q.v.z); lua_pushnumber(L, q.s);
    return 4;
}

/* b3.body_set_transform(body, x,y,z, ax,ay,az, radians) — axis+angle, so a
 * cart never has to build a quaternion by hand. */
static int l_body_set_transform(lua_State *L) {
    body_slot *b = body_at(L, 1);
    b3Pos p = pos_arg(L, 2);
    b3Vec3 axis = { (float)luaL_optnumber(L, 5, 0),
                    (float)luaL_optnumber(L, 6, 1),
                    (float)luaL_optnumber(L, 7, 0) };
    float ang = (float)luaL_optnumber(L, 8, 0);
    float len = sqrtf(axis.x*axis.x + axis.y*axis.y + axis.z*axis.z);
    if (len > 0) { axis.x /= len; axis.y /= len; axis.z /= len; }
    else         { axis.x = 0; axis.y = 1; axis.z = 0; }
    b3Body_SetTransform(b->id, p, b3MakeQuatFromAxisAngle(axis, ang));
    return 0;
}

static int l_body_velocity(lua_State *L) {
    b3Vec3 v = b3Body_GetLinearVelocity(body_at(L, 1)->id);
    lua_pushnumber(L, m2px(v.x));
    lua_pushnumber(L, m2px(v.y));
    lua_pushnumber(L, m2px(v.z));
    return 3;
}
static int l_body_set_velocity(lua_State *L) {
    body_slot *b = body_at(L, 1);
    b3Body_SetLinearVelocity(b->id, vec3_arg(L, 2));
    return 0;
}
static int l_body_apply_force(lua_State *L) {
    body_slot *b = body_at(L, 1);
    b3Body_ApplyForceToCenter(b->id, vec3_arg(L, 2), 1);
    return 0;
}
static int l_body_apply_impulse(lua_State *L) {
    body_slot *b = body_at(L, 1);
    b3Body_ApplyLinearImpulseToCenter(b->id, vec3_arg(L, 2), 1);
    return 0;
}
static int l_body_mass(lua_State *L) {
    lua_pushnumber(L, b3Body_GetMass(body_at(L, 1)->id));
    return 1;
}

/* ── shapes ─────────────────────────────────────────────────────────── */

static int shape_store(lua_State *L, b3ShapeId id, int bodyHandle) {
    int slot = -1;
    for (int i = 0; i < MAX_SHAPES; i++) if (!shapes[i].active) { slot = i; break; }
    if (slot < 0) return luaL_error(L, "b3: too many shapes (max %d)", MAX_SHAPES);
    shapes[slot].id = id; shapes[slot].body = bodyHandle; shapes[slot].active = 1;
    lua_pushinteger(L, slot + 1);
    return 1;
}

/* b3.shape_box(body, hx, hy, hz [, density]) — half-extents, like Box2D's */
static int l_shape_box(lua_State *L) {
    body_slot *b = body_at(L, 1);
    float hx = px2m(luaL_checknumber(L, 2));
    float hy = px2m(luaL_checknumber(L, 3));
    float hz = px2m(luaL_checknumber(L, 4));
    b3ShapeDef def = b3DefaultShapeDef();
    def.density = (float)luaL_optnumber(L, 5, 1.0);
    /* The box hull owns the arrays its base points into, so it must outlive
     * the create call — keep it on the stack, not in a temporary. */
    b3BoxHull hull = b3MakeBoxHull(hx, hy, hz);
    b3ShapeId id = b3CreateHullShape(b->id, &def, &hull.base);
    return shape_store(L, id, (int)luaL_checkinteger(L, 1));
}

static int l_shape_sphere(lua_State *L) {
    body_slot *b = body_at(L, 1);
    b3Sphere s;
    s.center = (b3Vec3){ 0, 0, 0 };
    s.radius = px2m(luaL_checknumber(L, 2));
    b3ShapeDef def = b3DefaultShapeDef();
    def.density = (float)luaL_optnumber(L, 3, 1.0);
    b3ShapeId id = b3CreateSphereShape(b->id, &def, &s);
    return shape_store(L, id, (int)luaL_checkinteger(L, 1));
}

/* b3.shape_capsule(body, halfHeight, radius [, density]) — along local Y */
static int l_shape_capsule(lua_State *L) {
    body_slot *b = body_at(L, 1);
    float hh = px2m(luaL_checknumber(L, 2));
    float r  = px2m(luaL_checknumber(L, 3));
    b3Capsule c;
    c.center1 = (b3Vec3){ 0, -hh, 0 };
    c.center2 = (b3Vec3){ 0,  hh, 0 };
    c.radius  = r;
    b3ShapeDef def = b3DefaultShapeDef();
    def.density = (float)luaL_optnumber(L, 4, 1.0);
    b3ShapeId id = b3CreateCapsuleShape(b->id, &def, &c);
    return shape_store(L, id, (int)luaL_checkinteger(L, 1));
}

/* ── queries ────────────────────────────────────────────────────────── */

/* b3.raycast(world, ox,oy,oz, dx,dy,dz) -> hitX,hitY,hitZ, nx,ny,nz, frac
 * or nil when nothing is hit. */
static int l_raycast(lua_State *L) {
    int wh = (int)luaL_checkinteger(L, 1);
    if (wh < 1 || wh > MAX_WORLDS || !world_alive[wh - 1])
        return luaL_error(L, "b3: bad world handle %d", wh);
    b3Pos  origin = pos_arg(L, 2);
    b3Vec3 delta  = vec3_arg(L, 5);
    b3RayResult r = b3World_CastRayClosest(worlds[wh - 1], origin, delta,
                                           b3DefaultQueryFilter());
    if (!r.hit) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, m2px(r.point.x));
    lua_pushnumber(L, m2px(r.point.y));
    lua_pushnumber(L, m2px(r.point.z));
    lua_pushnumber(L, r.normal.x);
    lua_pushnumber(L, r.normal.y);
    lua_pushnumber(L, r.normal.z);
    lua_pushnumber(L, r.fraction);
    return 7;
}

/* ── registration ───────────────────────────────────────────────────── */

static const luaL_Reg b3_lib[] = {
    { "world_new",          l_world_new },
    { "world_destroy",      l_world_destroy },
    { "world_step",         l_world_step },
    { "set_meter",          l_set_meter },
    { "get_meter",          l_get_meter },
    { "info",               l_info },

    { "body_new",           l_body_new },
    { "body_destroy",       l_body_destroy },
    { "body_position",      l_body_position },
    { "body_rotation",      l_body_rotation },
    { "body_set_transform", l_body_set_transform },
    { "body_velocity",      l_body_velocity },
    { "body_set_velocity",  l_body_set_velocity },
    { "body_apply_force",   l_body_apply_force },
    { "body_apply_impulse", l_body_apply_impulse },
    { "body_mass",          l_body_mass },

    { "shape_box",          l_shape_box },
    { "shape_sphere",       l_shape_sphere },
    { "shape_capsule",      l_shape_capsule },

    { "raycast",            l_raycast },
    { NULL, NULL }
};

void wcl_open_physics3d(lua_State *L) {
    /* Start the pool at boot, not at world_new. Otherwise b3.info() reports
     * zero workers until a world happens to exist, which is both a lie and
     * unhelpful to a cart deciding how much work to schedule. */
    wc_taskpool_init(wc_taskpool_hw_threads());
    luaL_newlib(L, b3_lib);
    lua_setglobal(L, "b3");
}
