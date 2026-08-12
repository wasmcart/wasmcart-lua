/*
 * physics.c - Box2D v3 bound to Lua for the wasmcart Lua engine.
 *
 * Box2D 3.2.0 is pure C with an opaque handle API, which makes it a far
 * better embedding target than the C++ v2 that LOVE wraps. It also has
 * first-class wasm SIMD support (B2_CPU_WASM -> B2_SIMD_SSE2, width 4),
 * which emcc lowers to wasm128 -- so the solver is genuinely vectorized
 * here, not scalar-fallback.
 *
 * Design: Lua never sees a Box2D pointer. Bodies and shapes are integer
 * handles into slot tables here, so a stale handle from Lua is a checked
 * error rather than a wild pointer into the heap. That matters because
 * game code destroys colliders constantly (Cavern: 21 :destroy() calls).
 *
 * The Lua-facing surface is `b2.*`. The LOVE-shaped `love.physics` module
 * and the windfield-shaped `wf` collider API are both built on top of this
 * in prelude.lua, where they are far easier to change.
 */
#include "box2d/box2d.h"

#include "lua.h"
#include "lauxlib.h"

#include <string.h>
#include <math.h>

#define MAX_BODIES 4096
#define MAX_SHAPES 8192
#define MAX_WORLDS 4

typedef struct {
    b2BodyId id;
    int      active;
    int      user;      /* a Lua-side collision-class index */
    int      world;     /* owning world handle */
} body_slot_t;

typedef struct {
    b2ShapeId id;
    int       active;
    int       body;     /* owning body handle */
} shape_slot_t;

/* Multiple worlds are required, not a nicety: real games run a separate
 * zero-gravity world for gameplay collision alongside a gravity world for
 * debris and particles. A single global world would force callers to fake
 * per-body gravity scaling, which changes behavior. */
static b2WorldId    worlds[MAX_WORLDS];
static int          world_alive[MAX_WORLDS];
static body_slot_t  bodies[MAX_BODIES];
static shape_slot_t shapes[MAX_SHAPES];
/* Joints live up here with the other slot tables rather than beside the
 * joint code, because l_world_destroy has to clear a destroyed world's
 * joint slots and is defined long before it. */
#define MAX_JOINTS 2048
typedef struct { b2JointId id; int active; int world; } joint_slot_t;
static joint_slot_t joints[MAX_JOINTS];
static float        pixels_per_meter = 32.0f;

static b2WorldId world_get(lua_State *L, int h) {
    if (h < 1 || h > MAX_WORLDS || !world_alive[h - 1]) {
        luaL_error(L, "physics: invalid world handle (%d)", h);
    }
    return worlds[h - 1];
}

/* ── handle helpers ─────────────────────────────────────────────────── */

static int body_alloc(b2BodyId id, int world) {
    for (int i = 0; i < MAX_BODIES; i++) {
        if (!bodies[i].active) {
            bodies[i].id = id;
            bodies[i].active = 1;
            bodies[i].user = 0;
            bodies[i].world = world;
            return i + 1;               /* 1-based: 0 means "none" */
        }
    }
    return 0;
}

static body_slot_t *body_get(lua_State *L, int handle) {
    if (handle < 1 || handle > MAX_BODIES || !bodies[handle - 1].active) {
        luaL_error(L, "physics: invalid or destroyed body handle (%d)", handle);
        return NULL;
    }
    return &bodies[handle - 1];
}

static int shape_alloc(b2ShapeId id, int body) {
    for (int i = 0; i < MAX_SHAPES; i++) {
        if (!shapes[i].active) {
            shapes[i].id = id;
            shapes[i].active = 1;
            shapes[i].body = body;
            return i + 1;
        }
    }
    return 0;
}

/* Convert pixels (what games think in) to meters (what Box2D wants).
 * Box2D is tuned for objects roughly 0.1..10 meters; feeding it pixel
 * coordinates directly makes the solver behave badly, which is why LOVE
 * exposes a meter scale too. */
static inline float px2m(double px) { return (float)(px / pixels_per_meter); }
static inline double m2px(float m)  { return (double)m * pixels_per_meter; }

/* ── world ──────────────────────────────────────────────────────────── */

/* world_new(gx, gy) -> worldHandle */
static int l_world_new(lua_State *L) {
    double gx = luaL_optnumber(L, 1, 0);
    double gy = luaL_optnumber(L, 2, 0);

    int slot = -1;
    for (int i = 0; i < MAX_WORLDS; i++) if (!world_alive[i]) { slot = i; break; }
    if (slot < 0) return luaL_error(L, "physics: too many worlds (max %d)", MAX_WORLDS);

    b2WorldDef def = b2DefaultWorldDef();
    def.gravity = (b2Vec2){ px2m(gx), px2m(gy) };
    worlds[slot] = b2CreateWorld(&def);
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
    /* Joints too. b2DestroyWorld frees the joints themselves, but the slot
     * table is ours -- leaving entries marked active would hand a later
     * caller a handle into a destroyed world. */
    for (int i = 0; i < MAX_JOINTS; i++)
        if (joints[i].active && joints[i].world == h) joints[i].active = 0;
    b2DestroyWorld(worlds[h - 1]);
    world_alive[h - 1] = 0;
    return 0;
}

static int l_world_gravity(lua_State *L) {
    b2WorldId w = world_get(L, (int)luaL_checkinteger(L, 1));
    b2World_SetGravity(w, (b2Vec2){ px2m(luaL_checknumber(L, 2)),
                                    px2m(luaL_checknumber(L, 3)) });
    return 0;
}

static int l_world_step(lua_State *L) {
    b2WorldId w = world_get(L, (int)luaL_checkinteger(L, 1));
    float dt = (float)luaL_optnumber(L, 2, 1.0 / 60.0);
    int sub = (int)luaL_optinteger(L, 3, 4);
    b2World_Step(w, dt, sub);
    return 0;
}

static int l_set_meter(lua_State *L) {
    double m = luaL_checknumber(L, 1);
    if (m > 0) pixels_per_meter = (float)m;
    return 0;
}

static int l_get_meter(lua_State *L) {
    lua_pushnumber(L, pixels_per_meter);
    return 1;
}

/* ── bodies ─────────────────────────────────────────────────────────── */

/* body_new(world, x, y, type)  type: 0 static, 1 kinematic, 2 dynamic */
static int l_body_new(lua_State *L) {
    int wh = (int)luaL_checkinteger(L, 1);
    b2WorldId w = world_get(L, wh);
    double x = luaL_checknumber(L, 2);
    double y = luaL_checknumber(L, 3);
    int type = (int)luaL_optinteger(L, 4, 2);

    b2BodyDef def = b2DefaultBodyDef();
    def.position = (b2Vec2){ px2m(x), px2m(y) };
    def.type = (type == 0) ? b2_staticBody
             : (type == 1) ? b2_kinematicBody
                           : b2_dynamicBody;

    /* Continuous collision stays OFF by default, matching Box2D's own
     * default and LOVE's. Callers opt in per body with setBullet() for
     * genuinely fast movers (projectiles). Enabling it globally costs
     * solver time on every body to fix a problem most games don't have. */
    b2BodyId id = b2CreateBody(w, &def);
    int h = body_alloc(id, wh);
    if (!h) { b2DestroyBody(id); lua_pushnil(L); return 1; }
    lua_pushinteger(L, h);
    return 1;
}

static int l_body_destroy(lua_State *L) {
    int h = (int)luaL_checkinteger(L, 1);
    if (h < 1 || h > MAX_BODIES || !bodies[h - 1].active) return 0;
    /* drop shape slots owned by this body first, so their handles can't
     * outlive the body they point into */
    for (int i = 0; i < MAX_SHAPES; i++)
        if (shapes[i].active && shapes[i].body == h) shapes[i].active = 0;
    b2DestroyBody(bodies[h - 1].id);
    bodies[h - 1].active = 0;
    return 0;
}

static int l_body_alive(lua_State *L) {
    int h = (int)luaL_checkinteger(L, 1);
    lua_pushboolean(L, h >= 1 && h <= MAX_BODIES && bodies[h - 1].active);
    return 1;
}

static int l_body_position(lua_State *L) {
    body_slot_t *b = body_get(L, (int)luaL_checkinteger(L, 1));
    b2Vec2 p = b2Body_GetPosition(b->id);
    lua_pushnumber(L, m2px(p.x));
    lua_pushnumber(L, m2px(p.y));
    return 2;
}

static int l_body_set_position(lua_State *L) {
    body_slot_t *b = body_get(L, (int)luaL_checkinteger(L, 1));
    b2Vec2 p = { px2m(luaL_checknumber(L, 2)), px2m(luaL_checknumber(L, 3)) };
    b2Body_SetTransform(b->id, p, b2Body_GetRotation(b->id));
    return 0;
}

static int l_body_angle(lua_State *L) {
    body_slot_t *b = body_get(L, (int)luaL_checkinteger(L, 1));
    b2Rot r = b2Body_GetRotation(b->id);
    lua_pushnumber(L, b2Rot_GetAngle(r));
    return 1;
}

static int l_body_set_angle(lua_State *L) {
    body_slot_t *b = body_get(L, (int)luaL_checkinteger(L, 1));
    b2Body_SetTransform(b->id, b2Body_GetPosition(b->id),
                        b2MakeRot((float)luaL_checknumber(L, 2)));
    return 0;
}

static int l_body_velocity(lua_State *L) {
    body_slot_t *b = body_get(L, (int)luaL_checkinteger(L, 1));
    b2Vec2 v = b2Body_GetLinearVelocity(b->id);
    lua_pushnumber(L, m2px(v.x));
    lua_pushnumber(L, m2px(v.y));
    return 2;
}

static int l_body_set_velocity(lua_State *L) {
    body_slot_t *b = body_get(L, (int)luaL_checkinteger(L, 1));
    b2Body_SetLinearVelocity(b->id, (b2Vec2){ px2m(luaL_checknumber(L, 2)),
                                              px2m(luaL_checknumber(L, 3)) });
    return 0;
}

static int l_body_apply_force(lua_State *L) {
    body_slot_t *b = body_get(L, (int)luaL_checkinteger(L, 1));
    b2Body_ApplyForceToCenter(b->id, (b2Vec2){ px2m(luaL_checknumber(L, 2)),
                                               px2m(luaL_checknumber(L, 3)) }, true);
    return 0;
}

static int l_body_apply_impulse(lua_State *L) {
    body_slot_t *b = body_get(L, (int)luaL_checkinteger(L, 1));
    b2Body_ApplyLinearImpulseToCenter(b->id, (b2Vec2){ px2m(luaL_checknumber(L, 2)),
                                                       px2m(luaL_checknumber(L, 3)) }, true);
    return 0;
}

static int l_body_set_type(lua_State *L) {
    body_slot_t *b = body_get(L, (int)luaL_checkinteger(L, 1));
    int t = (int)luaL_checkinteger(L, 2);
    b2Body_SetType(b->id, t == 0 ? b2_staticBody
                        : t == 1 ? b2_kinematicBody : b2_dynamicBody);
    return 0;
}

/* v2's setFixedRotation became v3's motion locks. Games use it to stop a
 * character capsule from tipping over, so only angularZ is locked here. */
static int l_body_set_fixed_rotation(lua_State *L) {
    body_slot_t *b = body_get(L, (int)luaL_checkinteger(L, 1));
    b2MotionLocks locks = { false, false, lua_toboolean(L, 2) ? true : false };
    b2Body_SetMotionLocks(b->id, locks);
    return 0;
}

static int l_body_set_gravity_scale(lua_State *L) {
    body_slot_t *b = body_get(L, (int)luaL_checkinteger(L, 1));
    b2Body_SetGravityScale(b->id, (float)luaL_checknumber(L, 2));
    return 0;
}

static int l_body_set_linear_damping(lua_State *L) {
    body_slot_t *b = body_get(L, (int)luaL_checkinteger(L, 1));
    b2Body_SetLinearDamping(b->id, (float)luaL_checknumber(L, 2));
    return 0;
}

static int l_body_set_bullet(lua_State *L) {
    body_slot_t *b = body_get(L, (int)luaL_checkinteger(L, 1));
    b2Body_SetBullet(b->id, lua_toboolean(L, 2));
    return 0;
}

static int l_body_mass(lua_State *L) {
    body_slot_t *b = body_get(L, (int)luaL_checkinteger(L, 1));
    lua_pushnumber(L, b2Body_GetMass(b->id));
    return 1;
}

/* the Lua-side collision-class index, stored here so it survives with the
 * body rather than in a Lua table keyed by a handle that may be reused */
static int l_body_set_user(lua_State *L) {
    body_slot_t *b = body_get(L, (int)luaL_checkinteger(L, 1));
    b->user = (int)luaL_checkinteger(L, 2);
    return 0;
}

static int l_body_get_user(lua_State *L) {
    body_slot_t *b = body_get(L, (int)luaL_checkinteger(L, 1));
    lua_pushinteger(L, b->user);
    return 1;
}

/* ── shapes ─────────────────────────────────────────────────────────── */

static b2ShapeDef shape_def_from(lua_State *L, int idx) {
    b2ShapeDef sd = b2DefaultShapeDef();
    if (lua_istable(L, idx)) {
        lua_getfield(L, idx, "density");
        if (lua_isnumber(L, -1)) sd.density = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, idx, "friction");
        if (lua_isnumber(L, -1)) sd.material.friction = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, idx, "restitution");
        if (lua_isnumber(L, -1)) sd.material.restitution = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, idx, "sensor");
        if (lua_toboolean(L, -1)) sd.isSensor = true;
        lua_pop(L, 1);
    }
    sd.enableContactEvents = true;
    return sd;
}

/* shape_box(body, halfW, halfH, [opts]) */
static int l_shape_box(lua_State *L) {
    int bh = (int)luaL_checkinteger(L, 1);
    body_slot_t *b = body_get(L, bh);
    float hw = px2m(luaL_checknumber(L, 2));
    float hh = px2m(luaL_checknumber(L, 3));
    b2ShapeDef sd = shape_def_from(L, 4);
    b2Polygon box = b2MakeBox(hw, hh);
    b2ShapeId sid = b2CreatePolygonShape(b->id, &sd, &box);
    lua_pushinteger(L, shape_alloc(sid, bh));
    return 1;
}

/* shape_circle(body, radius, [opts]) */
static int l_shape_circle(lua_State *L) {
    int bh = (int)luaL_checkinteger(L, 1);
    body_slot_t *b = body_get(L, bh);
    b2Circle c = { { 0, 0 }, px2m(luaL_checknumber(L, 2)) };
    b2ShapeDef sd = shape_def_from(L, 3);
    b2ShapeId sid = b2CreateCircleShape(b->id, &sd, &c);
    lua_pushinteger(L, shape_alloc(sid, bh));
    return 1;
}

/* shape_polygon(body, {x1,y1,x2,y2,...}, [opts]) - convex, <= 8 points */
static int l_shape_polygon(lua_State *L) {
    int bh = (int)luaL_checkinteger(L, 1);
    body_slot_t *b = body_get(L, bh);
    luaL_checktype(L, 2, LUA_TTABLE);
    int n = (int)lua_rawlen(L, 2) / 2;
    if (n < 3) return luaL_error(L, "physics: polygon needs at least 3 points");
    if (n > B2_MAX_POLYGON_VERTICES) n = B2_MAX_POLYGON_VERTICES;
    b2Vec2 pts[B2_MAX_POLYGON_VERTICES];
    for (int i = 0; i < n; i++) {
        lua_rawgeti(L, 2, i * 2 + 1); pts[i].x = px2m(lua_tonumber(L, -1)); lua_pop(L, 1);
        lua_rawgeti(L, 2, i * 2 + 2); pts[i].y = px2m(lua_tonumber(L, -1)); lua_pop(L, 1);
    }
    b2Hull hull = b2ComputeHull(pts, n);
    if (hull.count < 3) return luaL_error(L, "physics: polygon is not convex");
    b2Polygon poly = b2MakePolygon(&hull, 0.0f);
    b2ShapeDef sd = shape_def_from(L, 3);
    b2ShapeId sid = b2CreatePolygonShape(b->id, &sd, &poly);
    lua_pushinteger(L, shape_alloc(sid, bh));
    return 1;
}

/* shape_segment(body, x1,y1,x2,y2, [opts]) - static level geometry */
static int l_shape_segment(lua_State *L) {
    int bh = (int)luaL_checkinteger(L, 1);
    body_slot_t *b = body_get(L, bh);
    b2Segment seg = {
        { px2m(luaL_checknumber(L, 2)), px2m(luaL_checknumber(L, 3)) },
        { px2m(luaL_checknumber(L, 4)), px2m(luaL_checknumber(L, 5)) },
    };
    b2ShapeDef sd = shape_def_from(L, 6);
    b2ShapeId sid = b2CreateSegmentShape(b->id, &sd, &seg);
    lua_pushinteger(L, shape_alloc(sid, bh));
    return 1;
}

/* ── collision filtering ────────────────────────────────────────────── */

/* shape_filter(shape, category, mask) - bitmask filtering, which is how
 * windfield's named collision classes are implemented on the Lua side */
static int l_shape_filter(lua_State *L) {
    int sh = (int)luaL_checkinteger(L, 1);
    if (sh < 1 || sh > MAX_SHAPES || !shapes[sh - 1].active) return 0;
    b2Filter f = b2DefaultFilter();
    f.categoryBits = (uint64_t)luaL_checkinteger(L, 2);
    f.maskBits = (uint64_t)luaL_checkinteger(L, 3);
    b2Shape_SetFilter(shapes[sh - 1].id, f);
    return 0;
}

static int l_shape_set_sensor(lua_State *L) {
    int sh = (int)luaL_checkinteger(L, 1);
    if (sh < 1 || sh > MAX_SHAPES || !shapes[sh - 1].active) return 0;
    /* v3 fixes sensor-ness at creation; report rather than silently ignore */
    lua_pushboolean(L, b2Shape_IsSensor(shapes[sh - 1].id));
    return 1;
}

/* ── queries ────────────────────────────────────────────────────────── */

typedef struct {
    lua_State *L;
    int count;
} query_ctx_t;

static bool overlap_cb(b2ShapeId shapeId, void *context) {
    query_ctx_t *ctx = (query_ctx_t *)context;
    b2BodyId bid = b2Shape_GetBody(shapeId);
    /* map the Box2D body back to our handle */
    for (int i = 0; i < MAX_BODIES; i++) {
        if (bodies[i].active && B2_ID_EQUALS(bodies[i].id, bid)) {
            ctx->count++;
            lua_pushinteger(ctx->L, i + 1);
            lua_rawseti(ctx->L, -2, ctx->count);
            break;
        }
    }
    return true;   /* keep going: callers want every overlap */
}

/* query_circle(world, x, y, radius) -> { bodyHandle, ... } */
static int l_query_circle(lua_State *L) {
    b2WorldId world = world_get(L, (int)luaL_checkinteger(L, 1));
    float x = px2m(luaL_checknumber(L, 2));
    float y = px2m(luaL_checknumber(L, 3));
    float r = px2m(luaL_checknumber(L, 4));

    lua_newtable(L);
    query_ctx_t ctx = { L, 0 };
    b2Circle circle = { { 0, 0 }, r };
    b2ShapeProxy proxy = b2MakeProxy(&(b2Vec2){ x, y }, 1, r);
    (void)circle;
    /* Box2D gained a double-precision world ORIGIN for queries (same split
     * Box3D uses: double translation, float rotation). These carts work in
     * absolute coordinates, so the origin is zero. */
    b2World_OverlapShape(world, (b2Pos){ 0, 0 }, &proxy, b2DefaultQueryFilter(),
                         overlap_cb, &ctx);
    return 1;
}

/* query_box(world, x, y, w, h) -> { bodyHandle, ... }  (x,y = top-left) */
static int l_query_box(lua_State *L) {
    b2WorldId world = world_get(L, (int)luaL_checkinteger(L, 1));
    float x = px2m(luaL_checknumber(L, 2));
    float y = px2m(luaL_checknumber(L, 3));
    float w = px2m(luaL_checknumber(L, 4));
    float h = px2m(luaL_checknumber(L, 5));

    lua_newtable(L);
    query_ctx_t ctx = { L, 0 };
    b2AABB aabb = { { x, y }, { x + w, y + h } };
    b2World_OverlapAABB(world, (b2Pos){ 0, 0 }, aabb, b2DefaultQueryFilter(),
                        overlap_cb, &ctx);
    return 1;
}

/* ── contact events (drained once per frame by the Lua layer) ───────── */

static int push_body_handle(lua_State *L, b2BodyId bid) {
    for (int i = 0; i < MAX_BODIES; i++) {
        if (bodies[i].active && B2_ID_EQUALS(bodies[i].id, bid)) {
            lua_pushinteger(L, i + 1);
            return 1;
        }
    }
    return 0;
}

/* contacts(world) -> { {a, b}, ... } for contacts that BEGAN this step */
static int l_contacts(lua_State *L) {
    b2WorldId world = world_get(L, (int)luaL_checkinteger(L, 1));
    lua_newtable(L);
    b2ContactEvents ev = b2World_GetContactEvents(world);
    int n = 0;
    for (int i = 0; i < ev.beginCount; i++) {
        b2ContactBeginTouchEvent *e = &ev.beginEvents[i];
        b2BodyId ba = b2Shape_GetBody(e->shapeIdA);
        b2BodyId bb = b2Shape_GetBody(e->shapeIdB);
        lua_newtable(L);
        if (!push_body_handle(L, ba)) { lua_pop(L, 1); continue; }
        lua_rawseti(L, -2, 1);
        if (!push_body_handle(L, bb)) { lua_pop(L, 1); continue; }
        lua_rawseti(L, -2, 2);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

/* ── stats (so a game can see the solver is actually working) ───────── */

static int l_stats(lua_State *L) {
    lua_newtable(L);
    int live_bodies = 0, live_shapes = 0;
    for (int i = 0; i < MAX_BODIES; i++) if (bodies[i].active) live_bodies++;
    for (int i = 0; i < MAX_SHAPES; i++) if (shapes[i].active) live_shapes++;
    lua_pushinteger(L, live_bodies); lua_setfield(L, -2, "bodies");
    lua_pushinteger(L, live_shapes); lua_setfield(L, -2, "shapes");
    /* B2_SIMD_* live in Box2D's private src/core.h, which the docs say not
     * to include. We compile the library with -msimd128 -msse2, so on wasm
     * Box2D selects its SSE2 path and clang lowers those intrinsics to
     * wasm128 -- report what our own build flags guarantee rather than
     * reaching into library internals. */
#if defined( __wasm_simd128__ )
    lua_pushstring(L, "wasm-simd128");
    lua_setfield(L, -2, "simd");
    lua_pushinteger(L, 4);
#else
    lua_pushstring(L, "scalar");
    lua_setfield(L, -2, "simd");
    lua_pushinteger(L, 1);
#endif
    lua_setfield(L, -2, "simdWidth");
    return 1;
}

/* ── joints ─────────────────────────────────────────────────────────────
 *
 * Box2D's whole constraint vocabulary was unreachable from Lua: no hinge,
 * no rope, no slider, no weld. That rules out most of what a 2D physics
 * game is actually made of (ragdolls, vehicles, bridges, chains), so the
 * five general-purpose joints are bound here.
 *
 * This Box2D uses local FRAMES (b2Transform) rather than the older plain
 * anchor points, so the anchors a cart passes in world pixels are
 * converted to each body's local space here. A cart should not have to
 * know that detail to hang a door on a hinge. */
static int joint_alloc(b2JointId id, int world) {
    for (int i = 0; i < MAX_JOINTS; i++) {
        if (!joints[i].active) {
            joints[i].id = id; joints[i].active = 1; joints[i].world = world;
            return i + 1;
        }
    }
    return 0;
}

static b2JointId joint_get(lua_State *L, int i) {
    int h = (int)luaL_checkinteger(L, i);
    if (h < 1 || h > MAX_JOINTS || !joints[h - 1].active)
        luaL_error(L, "physics: invalid joint handle (%d)", h);
    return joints[h - 1].id;
}

/* Fill the shared base of every joint def: the two bodies, and each
 * body's local frame derived from a world-space anchor in pixels.
 *
 * argCollide is where the collideConnected flag lives. It is passed
 * SEPARATELY rather than assumed to be argAnchor+2, because the joints
 * that take an axis put two more numbers in between: joint_prismatic and
 * joint_wheel were both reading their axisX as the collide flag, so any
 * caller passing an axis silently turned collision on as well. */
static void joint_base_from_at(lua_State *L, b2JointDef *base,
                               int bodyA, int bodyB, int argAnchor,
                               int argCollide) {
    body_slot_t *ba = &bodies[bodyA - 1];
    body_slot_t *bb = &bodies[bodyB - 1];
    base->bodyIdA = ba->id;
    base->bodyIdB = bb->id;
    b2Vec2 anchor;
    anchor.x = px2m(luaL_optnumber(L, argAnchor, 0));
    anchor.y = px2m(luaL_optnumber(L, argAnchor + 1, 0));
    base->localFrameA.p = b2Body_GetLocalPoint(ba->id, anchor);
    base->localFrameA.q = b2Rot_identity;
    base->localFrameB.p = b2Body_GetLocalPoint(bb->id, anchor);
    base->localFrameB.q = b2Rot_identity;
    base->collideConnected = lua_toboolean(L, argCollide);
}

/* The common case: collide flag sits right after the anchor pair. */
static void joint_base_from(lua_State *L, b2JointDef *base,
                            int bodyA, int bodyB, int argAnchor) {
    joint_base_from_at(L, base, bodyA, bodyB, argAnchor, argAnchor + 2);
}

static int two_bodies(lua_State *L, int *a, int *b) {
    *a = (int)luaL_checkinteger(L, 2);
    *b = (int)luaL_checkinteger(L, 3);
    if (*a < 1 || *a > MAX_BODIES || !bodies[*a - 1].active)
        return luaL_error(L, "physics: invalid body handle (%d)", *a);
    if (*b < 1 || *b > MAX_BODIES || !bodies[*b - 1].active)
        return luaL_error(L, "physics: invalid body handle (%d)", *b);
    return 0;
}

/* b2.joint_revolute(world, bodyA, bodyB, ax, ay [, collide]) -> handle
 * A hinge: the two bodies share a pivot point. */
static int l_joint_revolute(lua_State *L) {
    int wh = (int)luaL_checkinteger(L, 1);
    b2WorldId world = world_get(L, wh);
    int a, b; two_bodies(L, &a, &b);
    b2RevoluteJointDef def = b2DefaultRevoluteJointDef();
    joint_base_from(L, &def.base, a, b, 4);
    int h = joint_alloc(b2CreateRevoluteJoint(world, &def), wh);
    if (!h) return luaL_error(L, "physics: too many joints (max %d)", MAX_JOINTS);
    lua_pushinteger(L, h);
    return 1;
}

/* b2.joint_distance(world, bodyA, bodyB, ax, ay, length [, collide]) */
static int l_joint_distance(lua_State *L) {
    int wh = (int)luaL_checkinteger(L, 1);
    b2WorldId world = world_get(L, wh);
    int a, b; two_bodies(L, &a, &b);
    b2DistanceJointDef def = b2DefaultDistanceJointDef();
    joint_base_from_at(L, &def.base, a, b, 4, 7);   /* length occupies 6 */
    /* A distance joint measures between the two local frames, so unlike a
     * hinge the frames must NOT be the same world point: anchoring both at
     * one spot leaves a zero-length separation and the rest length has
     * nothing to act on (the bodies just hold their initial spacing).
     * Frame A stays at the given anchor; frame B rides body B's origin. */
    def.base.localFrameB.p = (b2Vec2){ 0.0f, 0.0f };
    def.length = px2m(luaL_optnumber(L, 6, 32.0));
    if (def.length < 0.005f) def.length = 0.005f;
    int h = joint_alloc(b2CreateDistanceJoint(world, &def), wh);
    if (!h) return luaL_error(L, "physics: too many joints (max %d)", MAX_JOINTS);
    lua_pushinteger(L, h);
    return 1;
}

/* b2.joint_prismatic(world, bodyA, bodyB, ax, ay, axisX, axisY [, collide])
 * A slider along an axis.
 *
 * THE TWO BODIES MUST NOT SHARE AN ORIGIN. If they do, both local frames
 * land on the same point, the solver has a zero-length separation to work
 * from, and the slider jitters a few pixels sideways instead of sliding.
 * Measured: co-located bodies under a pure +x force moved (+0.16, -3.63);
 * the same rig with the bodies half a pixel apart moved (+7220.48, +0.00).
 *
 * This is the same degeneracy already noted on joint_distance below, and
 * it looks exactly like "the axis argument is being ignored" -- every axis
 * value produces the same non-motion. It is not; the axis is fine. */
static int l_joint_prismatic(lua_State *L) {
    int wh = (int)luaL_checkinteger(L, 1);
    b2WorldId world = world_get(L, wh);
    int a, b; two_bodies(L, &a, &b);
    b2PrismaticJointDef def = b2DefaultPrismaticJointDef();
    joint_base_from_at(L, &def.base, a, b, 4, 8);   /* axis occupies 6,7 */
    /* The slide axis is encoded as the local frames' rotation. */
    float ax = (float)luaL_optnumber(L, 6, 1.0);
    float ay = (float)luaL_optnumber(L, 7, 0.0);
    float len = sqrtf(ax * ax + ay * ay);
    if (len < 1e-6f) { ax = 1.0f; ay = 0.0f; len = 1.0f; }
    b2Vec2 unit = { ax / len, ay / len };
    b2Rot r = b2MakeRotFromUnitVector(unit);
    def.base.localFrameA.q = b2InvMulRot(b2Body_GetRotation(bodies[a - 1].id), r);
    def.base.localFrameB.q = b2InvMulRot(b2Body_GetRotation(bodies[b - 1].id), r);
    int h = joint_alloc(b2CreatePrismaticJoint(world, &def), wh);
    if (!h) return luaL_error(L, "physics: too many joints (max %d)", MAX_JOINTS);
    lua_pushinteger(L, h);
    return 1;
}

/* b2.joint_weld(world, bodyA, bodyB, ax, ay [, collide]) — rigid glue */
static int l_joint_weld(lua_State *L) {
    int wh = (int)luaL_checkinteger(L, 1);
    b2WorldId world = world_get(L, wh);
    int a, b; two_bodies(L, &a, &b);
    b2WeldJointDef def = b2DefaultWeldJointDef();
    joint_base_from(L, &def.base, a, b, 4);
    int h = joint_alloc(b2CreateWeldJoint(world, &def), wh);
    if (!h) return luaL_error(L, "physics: too many joints (max %d)", MAX_JOINTS);
    lua_pushinteger(L, h);
    return 1;
}

/* b2.joint_motor(world, bodyA, bodyB [, collide]) — drives B toward a
 * target offset from A; the usual top-down "move this body" constraint. */
static int l_joint_motor(lua_State *L) {
    int wh = (int)luaL_checkinteger(L, 1);
    b2WorldId world = world_get(L, wh);
    int a, b; two_bodies(L, &a, &b);
    b2MotorJointDef def = b2DefaultMotorJointDef();
    def.base.bodyIdA = bodies[a - 1].id;
    def.base.bodyIdB = bodies[b - 1].id;
    def.base.collideConnected = lua_toboolean(L, 4);
    int h = joint_alloc(b2CreateMotorJoint(world, &def), wh);
    if (!h) return luaL_error(L, "physics: too many joints (max %d)", MAX_JOINTS);
    lua_pushinteger(L, h);
    return 1;
}

/* b2.joint_wheel(world, bodyA, bodyB, ax, ay, axisX, axisY [, collide])
 * A suspension strut: body B slides along an axis relative to A and spins
 * freely about the anchor. The car-wheel joint, hence the name. */
static int l_joint_wheel(lua_State *L) {
    int wh = (int)luaL_checkinteger(L, 1);
    b2WorldId world = world_get(L, wh);
    int a, b; two_bodies(L, &a, &b);
    b2WheelJointDef def = b2DefaultWheelJointDef();
    joint_base_from_at(L, &def.base, a, b, 4, 8);   /* axis occupies 6,7 */
    /* Same axis-as-rotation encoding as the prismatic joint above. */
    float ax = (float)luaL_optnumber(L, 6, 0.0);
    float ay = (float)luaL_optnumber(L, 7, 1.0);
    float len = sqrtf(ax * ax + ay * ay);
    if (len < 1e-6f) { ax = 0.0f; ay = 1.0f; len = 1.0f; }
    b2Vec2 unit = { ax / len, ay / len };
    b2Rot r = b2MakeRotFromUnitVector(unit);
    def.base.localFrameA.q = b2InvMulRot(b2Body_GetRotation(bodies[a - 1].id), r);
    def.base.localFrameB.q = b2InvMulRot(b2Body_GetRotation(bodies[b - 1].id), r);
    /* Suspension on by default: a wheel joint with no spring is just a
     * prismatic that also spins, which is never what someone asking for a
     * wheel wants. */
    def.enableSpring = true;
    def.hertz = 4.0f;
    def.dampingRatio = 0.7f;
    int h = joint_alloc(b2CreateWheelJoint(world, &def), wh);
    if (!h) return luaL_error(L, "physics: too many joints (max %d)", MAX_JOINTS);
    lua_pushinteger(L, h);
    return 1;
}

/* b2.joint_rope(world, bodyA, bodyB, ax, ay, maxLength [, collide])
 *
 * A rope: the bodies may come as close as they like but can never separate
 * further than maxLength. Box2D v3 has no dedicated rope joint -- 2.x's was
 * folded into the distance joint -- so this is a distance joint with its
 * lower limit at zero, its upper at maxLength, and the rigid rest-length
 * constraint switched OFF so only the limit acts. That is the same
 * construction upstream recommends, not an approximation. */
static int l_joint_rope(lua_State *L) {
    int wh = (int)luaL_checkinteger(L, 1);
    b2WorldId world = world_get(L, wh);
    int a, b; two_bodies(L, &a, &b);
    b2DistanceJointDef def = b2DefaultDistanceJointDef();
    joint_base_from_at(L, &def.base, a, b, 4, 7);   /* maxLength occupies 6 */
    def.base.localFrameB.p = (b2Vec2){ 0.0f, 0.0f };   /* see joint_distance */
    float maxLen = px2m(luaL_optnumber(L, 6, 64.0));
    if (maxLen < 0.005f) maxLen = 0.005f;
    def.length = maxLen;
    def.minLength = 0.0f;
    def.maxLength = maxLen;
    def.enableLimit = true;
    def.enableSpring = true;   /* springy along the free range... */
    def.hertz = 0.0f;          /* ...with zero stiffness, so it is slack */
    def.dampingRatio = 0.0f;
    int h = joint_alloc(b2CreateDistanceJoint(world, &def), wh);
    if (!h) return luaL_error(L, "physics: too many joints (max %d)", MAX_JOINTS);
    lua_pushinteger(L, h);
    return 1;
}

/* b2.joint_friction(world, bodyA, bodyB, ax, ay, maxForce, maxTorque
 *                   [, collide])
 *
 * Top-down friction: resists relative motion without preventing it, which
 * is how you fake ground drag in a game with no gravity. v3 dropped the
 * dedicated friction joint; it is a motor joint whose target velocity is
 * zero, so all it ever does is brake. Capping the force is what makes it
 * friction rather than glue. */
static int l_joint_friction(lua_State *L) {
    int wh = (int)luaL_checkinteger(L, 1);
    b2WorldId world = world_get(L, wh);
    int a, b; two_bodies(L, &a, &b);
    b2MotorJointDef def = b2DefaultMotorJointDef();
    joint_base_from_at(L, &def.base, a, b, 4, 8);   /* force caps occupy 6,7 */
    def.linearVelocity = (b2Vec2){ 0.0f, 0.0f };
    def.angularVelocity = 0.0f;
    def.maxVelocityForce  = (float)luaL_optnumber(L, 6, 100.0);
    def.maxVelocityTorque = (float)luaL_optnumber(L, 7, 100.0);
    /* No positional spring: friction resists MOTION, it does not pull the
     * body back toward where it started. */
    def.linearHertz = 0.0f;
    def.angularHertz = 0.0f;
    def.maxSpringForce = 0.0f;
    int h = joint_alloc(b2CreateMotorJoint(world, &def), wh);
    if (!h) return luaL_error(L, "physics: too many joints (max %d)", MAX_JOINTS);
    lua_pushinteger(L, h);
    return 1;
}

/* b2.joint_mouse(world, bodyGround, bodyTarget, tx, ty, maxForce
 *                [, collide]) -> handle
 *
 * Drag a body toward a moving point. v3 has no mouse joint, and this is
 * NOT an invention to paper over that: Box2D 3.2's own samples implement
 * mouse dragging with exactly this construction (samples/sample.cpp,
 * Sample::MouseDown) -- a motor joint with a linear spring, anchored on a
 * body the caller moves. So the substitute is the upstream one.
 *
 * The target is set afterwards with joint_set_target. */
static int l_joint_mouse(lua_State *L) {
    int wh = (int)luaL_checkinteger(L, 1);
    b2WorldId world = world_get(L, wh);
    int a, b; two_bodies(L, &a, &b);
    b2MotorJointDef def = b2DefaultMotorJointDef();
    def.base.bodyIdA = bodies[a - 1].id;
    def.base.bodyIdB = bodies[b - 1].id;
    /* Grab the target body at the point the caller named, so it swings
     * from where it was actually grabbed rather than from its centre. */
    b2Vec2 grab;
    grab.x = px2m(luaL_optnumber(L, 4, 0));
    grab.y = px2m(luaL_optnumber(L, 5, 0));
    def.base.localFrameA.p = b2Body_GetLocalPoint(bodies[a - 1].id, grab);
    def.base.localFrameA.q = b2Rot_identity;
    def.base.localFrameB.p = b2Body_GetLocalPoint(bodies[b - 1].id, grab);
    def.base.localFrameB.q = b2Rot_identity;
    def.base.collideConnected = lua_toboolean(L, 7);
    /* Spring values from the upstream sample. */
    def.linearHertz = 7.5f;
    def.linearDampingRatio = 1.0f;
    def.maxSpringForce = (float)luaL_optnumber(L, 6, 1000.0);
    /* Angular friction proportional to the body's lever arm, so a dragged
     * body does not spin wildly. Straight from the sample. */
    b2MassData md = b2Body_GetMassData(bodies[b - 1].id);
    if (md.mass > 0.0f) {
        float g = b2Length(b2World_GetGravity(world));
        float mg = md.mass * (g > 0.0f ? g : 9.8f);
        float lever = sqrtf(md.rotationalInertia / md.mass);
        def.maxVelocityTorque = 0.25f * lever * mg;
    }
    int h = joint_alloc(b2CreateMotorJoint(world, &def), wh);
    if (!h) return luaL_error(L, "physics: too many joints (max %d)", MAX_JOINTS);
    lua_pushinteger(L, h);
    return 1;
}

/* b2.joint_set_target(joint, x, y) -- move a mouse joint's grab point.
 * Implemented by moving the anchor body, which is what the sample does. */
static int l_joint_set_target(lua_State *L) {
    b2JointId j = joint_get(L, 1);
    b2BodyId anchor = b2Joint_GetBodyA(j);
    b2Vec2 p;
    p.x = px2m(luaL_checknumber(L, 2));
    p.y = px2m(luaL_checknumber(L, 3));
    b2Body_SetTransform(anchor, p, b2Body_GetRotation(anchor));
    b2Body_SetAwake(b2Joint_GetBodyB(j), true);
    return 0;
}

/* ── joint tuning ──────────────────────────────────────────────────────
 * LOVE's joint objects expose motors and limits, which is most of what
 * makes a joint useful: a hinge with no motor cannot drive anything, and
 * a slider with no limit slides forever. */

static int l_joint_set_motor(lua_State *L) {
    b2JointId j = joint_get(L, 1);
    int on = lua_toboolean(L, 2);
    float speed = (float)luaL_optnumber(L, 3, 0.0);
    float maxv  = (float)luaL_optnumber(L, 4, 1000.0);
    switch (b2Joint_GetType(j)) {
        case b2_revoluteJoint:
            b2RevoluteJoint_EnableMotor(j, on);
            b2RevoluteJoint_SetMotorSpeed(j, speed);
            b2RevoluteJoint_SetMaxMotorTorque(j, maxv);
            break;
        case b2_prismaticJoint:
            b2PrismaticJoint_EnableMotor(j, on);
            b2PrismaticJoint_SetMotorSpeed(j, speed);
            b2PrismaticJoint_SetMaxMotorForce(j, px2m(maxv));
            break;
        case b2_wheelJoint:
            b2WheelJoint_EnableMotor(j, on);
            b2WheelJoint_SetMotorSpeed(j, speed);
            b2WheelJoint_SetMaxMotorTorque(j, maxv);
            break;
        default:
            return luaL_error(L, "physics: this joint has no motor");
    }
    return 0;
}

static int l_joint_set_limits(lua_State *L) {
    b2JointId j = joint_get(L, 1);
    int on = lua_toboolean(L, 2);
    switch (b2Joint_GetType(j)) {
        case b2_revoluteJoint:
            /* radians, not pixels */
            b2RevoluteJoint_EnableLimit(j, on);
            b2RevoluteJoint_SetLimits(j, (float)luaL_optnumber(L, 3, 0.0),
                                         (float)luaL_optnumber(L, 4, 0.0));
            break;
        case b2_prismaticJoint:
            b2PrismaticJoint_EnableLimit(j, on);
            b2PrismaticJoint_SetLimits(j, px2m(luaL_optnumber(L, 3, 0.0)),
                                          px2m(luaL_optnumber(L, 4, 0.0)));
            break;
        case b2_wheelJoint:
            b2WheelJoint_EnableLimit(j, on);
            b2WheelJoint_SetLimits(j, px2m(luaL_optnumber(L, 3, 0.0)),
                                     px2m(luaL_optnumber(L, 4, 0.0)));
            break;
        case b2_distanceJoint:
            b2DistanceJoint_EnableLimit(j, on);
            b2DistanceJoint_SetLengthRange(j, px2m(luaL_optnumber(L, 3, 0.0)),
                                              px2m(luaL_optnumber(L, 4, 0.0)));
            break;
        default:
            return luaL_error(L, "physics: this joint has no limits");
    }
    return 0;
}

/* Spring stiffness, for the joints that have one. */
static int l_joint_set_spring(lua_State *L) {
    b2JointId j = joint_get(L, 1);
    int on = lua_toboolean(L, 2);
    float hz   = (float)luaL_optnumber(L, 3, 4.0);
    float damp = (float)luaL_optnumber(L, 4, 0.7);
    switch (b2Joint_GetType(j)) {
        case b2_wheelJoint:
            b2WheelJoint_EnableSpring(j, on);
            b2WheelJoint_SetSpringHertz(j, hz);
            b2WheelJoint_SetSpringDampingRatio(j, damp);
            break;
        case b2_distanceJoint:
            b2DistanceJoint_EnableSpring(j, on);
            b2DistanceJoint_SetSpringHertz(j, hz);
            b2DistanceJoint_SetSpringDampingRatio(j, damp);
            break;
        case b2_prismaticJoint:
            b2PrismaticJoint_EnableSpring(j, on);
            b2PrismaticJoint_SetSpringHertz(j, hz);
            b2PrismaticJoint_SetSpringDampingRatio(j, damp);
            break;
        case b2_revoluteJoint:
            b2RevoluteJoint_EnableSpring(j, on);
            b2RevoluteJoint_SetSpringHertz(j, hz);
            b2RevoluteJoint_SetSpringDampingRatio(j, damp);
            break;
        default:
            return luaL_error(L, "physics: this joint has no spring");
    }
    return 0;
}

/* Current joint state, for carts that steer off it. */
static int l_joint_length(lua_State *L) {
    b2JointId j = joint_get(L, 1);
    if (b2Joint_GetType(j) != b2_distanceJoint)
        return luaL_error(L, "physics: joint_length is distance/rope only");
    lua_pushnumber(L, m2px(b2DistanceJoint_GetLength(j)));
    return 1;
}

static int l_joint_set_length(lua_State *L) {
    b2JointId j = joint_get(L, 1);
    if (b2Joint_GetType(j) != b2_distanceJoint)
        return luaL_error(L, "physics: joint_set_length is distance/rope only");
    b2DistanceJoint_SetLength(j, px2m(luaL_checknumber(L, 2)));
    return 0;
}

/* Angle (revolute) or translation (prismatic).
 *
 * The WHEEL joint is deliberately absent from both. Box2D 3.2 exposes no
 * b2WheelJoint_GetAngle or _GetTranslation -- only motor/spring/limit
 * controls -- so there is nothing to report. Refusing loudly is the right
 * answer: a cart steering off a silently-zero suspension travel would be
 * far worse than one that gets told the reading does not exist. */
static int l_joint_angle(lua_State *L) {
    b2JointId j = joint_get(L, 1);
    switch (b2Joint_GetType(j)) {
        case b2_revoluteJoint: lua_pushnumber(L, b2RevoluteJoint_GetAngle(j)); break;
        default: return luaL_error(L, "physics: this joint has no angle reading");
    }
    return 1;
}

static int l_joint_translation(lua_State *L) {
    b2JointId j = joint_get(L, 1);
    switch (b2Joint_GetType(j)) {
        case b2_prismaticJoint:
            lua_pushnumber(L, m2px(b2PrismaticJoint_GetTranslation(j))); break;
        default: return luaL_error(L, "physics: this joint has no translation reading");
    }
    return 1;
}

/* Which kind is this? Lets the Lua layer give a joint the right methods. */
static int l_joint_type(lua_State *L) {
    switch (b2Joint_GetType(joint_get(L, 1))) {
        case b2_revoluteJoint:  lua_pushstring(L, "revolute"); break;
        case b2_prismaticJoint: lua_pushstring(L, "prismatic"); break;
        case b2_distanceJoint:  lua_pushstring(L, "distance"); break;
        case b2_weldJoint:      lua_pushstring(L, "weld"); break;
        case b2_motorJoint:     lua_pushstring(L, "motor"); break;
        case b2_wheelJoint:     lua_pushstring(L, "wheel"); break;
        default:                lua_pushstring(L, "unknown"); break;
    }
    return 1;
}

static int l_joint_destroy(lua_State *L) {
    int h = (int)luaL_checkinteger(L, 1);
    if (h < 1 || h > MAX_JOINTS || !joints[h - 1].active) return 0;
    b2DestroyJoint(joints[h - 1].id, true);
    joints[h - 1].active = 0;
    return 0;
}

/* Reaction force/torque: how hard a joint is working. A rope bridge that
 * snaps under load needs this. */
static int l_joint_force(lua_State *L) {
    b2Vec2 f = b2Joint_GetConstraintForce(joint_get(L, 1));
    lua_pushnumber(L, m2px(f.x));
    lua_pushnumber(L, m2px(f.y));
    return 2;
}
static int l_joint_torque(lua_State *L) {
    lua_pushnumber(L, b2Joint_GetConstraintTorque(joint_get(L, 1)));
    return 1;
}

/* ── post-creation shape material ──────────────────────────────────────
 * shape_def_from() covers creation time, but a cart that wants ice to get
 * slippery mid-level, or a ball to lose its bounce, had no way to say so. */
static b2ShapeId shape_get_id(lua_State *L, int i) {
    int h = (int)luaL_checkinteger(L, i);
    if (h < 1 || h > MAX_SHAPES || !shapes[h - 1].active)
        luaL_error(L, "physics: invalid shape handle (%d)", h);
    return shapes[h - 1].id;
}
static int l_shape_set_friction(lua_State *L) {
    b2Shape_SetFriction(shape_get_id(L, 1), (float)luaL_checknumber(L, 2));
    return 0;
}
static int l_shape_get_friction(lua_State *L) {
    lua_pushnumber(L, b2Shape_GetFriction(shape_get_id(L, 1)));
    return 1;
}
static int l_shape_set_restitution(lua_State *L) {
    b2Shape_SetRestitution(shape_get_id(L, 1), (float)luaL_checknumber(L, 2));
    return 0;
}
static int l_shape_get_restitution(lua_State *L) {
    lua_pushnumber(L, b2Shape_GetRestitution(shape_get_id(L, 1)));
    return 1;
}
static int l_shape_set_density2(lua_State *L) {
    b2Shape_SetDensity(shape_get_id(L, 1), (float)luaL_checknumber(L, 2), true);
    return 0;
}

/* ── body angular surface (2D counterpart of the b3 additions) ───────── */
static int l_body_angular_velocity(lua_State *L) {
    lua_pushnumber(L, b2Body_GetAngularVelocity(body_get(L, (int)luaL_checkinteger(L, 1))->id));
    return 1;
}
static int l_body_set_angular_velocity(lua_State *L) {
    b2Body_SetAngularVelocity(body_get(L, (int)luaL_checkinteger(L, 1))->id,
                              (float)luaL_checknumber(L, 2));
    return 0;
}
static int l_body_apply_torque(lua_State *L) {
    b2Body_ApplyTorque(body_get(L, (int)luaL_checkinteger(L, 1))->id,
                       (float)luaL_checknumber(L, 2), true);
    return 0;
}
static int l_body_set_angular_damping(lua_State *L) {
    b2Body_SetAngularDamping(body_get(L, (int)luaL_checkinteger(L, 1))->id,
                             (float)luaL_checknumber(L, 2));
    return 0;
}
static int l_body_is_awake(lua_State *L) {
    lua_pushboolean(L, b2Body_IsAwake(body_get(L, (int)luaL_checkinteger(L, 1))->id));
    return 1;
}
static int l_body_set_awake(lua_State *L) {
    b2Body_SetAwake(body_get(L, (int)luaL_checkinteger(L, 1))->id, lua_toboolean(L, 2));
    return 0;
}
static int l_body_enable_sleep(lua_State *L) {
    b2Body_EnableSleep(body_get(L, (int)luaL_checkinteger(L, 1))->id, lua_toboolean(L, 2));
    return 0;
}

static const luaL_Reg b2_lib[] = {
    { "world_new",        l_world_new },
    { "world_destroy",    l_world_destroy },
    { "world_gravity",    l_world_gravity },
    { "world_step",       l_world_step },
    { "set_meter",        l_set_meter },
    { "get_meter",        l_get_meter },

    { "body_new",         l_body_new },
    { "body_destroy",     l_body_destroy },
    { "body_alive",       l_body_alive },
    { "body_position",    l_body_position },
    { "body_set_position", l_body_set_position },
    { "body_angle",       l_body_angle },
    { "body_set_angle",   l_body_set_angle },
    { "body_velocity",    l_body_velocity },
    { "body_set_velocity", l_body_set_velocity },
    { "body_apply_force", l_body_apply_force },
    { "body_apply_impulse", l_body_apply_impulse },
    { "body_set_type",    l_body_set_type },
    { "body_set_fixed_rotation", l_body_set_fixed_rotation },
    { "body_set_gravity_scale",  l_body_set_gravity_scale },
    { "body_set_linear_damping", l_body_set_linear_damping },
    { "body_set_bullet",  l_body_set_bullet },
    { "body_mass",        l_body_mass },
    { "body_set_user",    l_body_set_user },
    { "body_get_user",    l_body_get_user },

    { "shape_box",        l_shape_box },
    { "shape_circle",     l_shape_circle },
    { "shape_polygon",    l_shape_polygon },
    { "shape_segment",    l_shape_segment },
    { "shape_filter",     l_shape_filter },
    { "shape_is_sensor",  l_shape_set_sensor },

    { "shape_set_friction",    l_shape_set_friction },
    { "shape_get_friction",    l_shape_get_friction },
    { "shape_set_restitution", l_shape_set_restitution },
    { "shape_get_restitution", l_shape_get_restitution },
    { "shape_set_density",     l_shape_set_density2 },

    { "body_angular_velocity",     l_body_angular_velocity },
    { "body_set_angular_velocity", l_body_set_angular_velocity },
    { "body_apply_torque",         l_body_apply_torque },
    { "body_set_angular_damping",  l_body_set_angular_damping },
    { "body_is_awake",             l_body_is_awake },
    { "body_set_awake",            l_body_set_awake },
    { "body_enable_sleep",         l_body_enable_sleep },

    { "joint_revolute",   l_joint_revolute },
    { "joint_distance",   l_joint_distance },
    { "joint_prismatic",  l_joint_prismatic },
    { "joint_weld",       l_joint_weld },
    { "joint_motor",      l_joint_motor },
    { "joint_wheel",      l_joint_wheel },
    { "joint_rope",       l_joint_rope },
    { "joint_friction",   l_joint_friction },
    { "joint_mouse",      l_joint_mouse },
    { "joint_set_target", l_joint_set_target },
    { "joint_set_motor",  l_joint_set_motor },
    { "joint_set_limits", l_joint_set_limits },
    { "joint_set_spring", l_joint_set_spring },
    { "joint_length",     l_joint_length },
    { "joint_set_length", l_joint_set_length },
    { "joint_angle",      l_joint_angle },
    { "joint_translation", l_joint_translation },
    { "joint_type",       l_joint_type },
    { "joint_destroy",    l_joint_destroy },
    { "joint_force",      l_joint_force },
    { "joint_torque",     l_joint_torque },

    { "query_circle",     l_query_circle },
    { "query_box",        l_query_box },
    { "contacts",         l_contacts },
    { "stats",            l_stats },
    { NULL, NULL }
};

void wcl_open_physics(lua_State *L) {
    luaL_newlib(L, b2_lib);
    lua_setglobal(L, "b2");
}
