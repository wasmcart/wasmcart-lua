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
