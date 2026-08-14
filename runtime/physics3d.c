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

static shape_slot *shape_at(lua_State *L, int i) {
    int h = (int)luaL_checkinteger(L, i);
    if (h < 1 || h > MAX_SHAPES || !shapes[h - 1].active)
        luaL_error(L, "b3: bad shape handle %d", h);
    return &shapes[h - 1];
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

/* b3.shape_cylinder(body, height, radius [, yOffset [, sides [, density]]])
 *
 * A tessellated cylinder, along local Y. Box3D builds it as a convex hull,
 * so unlike a mesh or a compound this is legal on a DYNAMIC body — which is
 * what makes a stacked profile (a bowling pin: flat base, belly, neck,
 * head) possible at all. yOffset shifts it along Y in the body's own frame,
 * so several of these attach to ONE body and fuse into a single solid.
 *
 * A flat-bottomed cylinder is not decoration: a capsule's rounded end makes
 * an upright object rock and self-right, so pins built from capsules refuse
 * to stay down. */
static int l_shape_cylinder(lua_State *L) {
    body_slot *b = body_at(L, 1);
    float h  = px2m(luaL_checknumber(L, 2));
    float r  = px2m(luaL_checknumber(L, 3));
    float yo = px2m(luaL_optnumber(L, 4, 0.0));
    int sides = (int)luaL_optinteger(L, 5, 12);
    if (sides < 3)  sides = 3;
    if (sides > 64) sides = 64;
    b3ShapeDef def = b3DefaultShapeDef();
    def.density = (float)luaL_optnumber(L, 6, 1.0);
    b3HullData *hull = b3CreateCylinder(h, r, yo, sides);
    if (!hull) luaL_error(L, "b3: could not build cylinder hull");
    b3ShapeId id = b3CreateHullShape(b->id, &def, hull);
    /* The create call clones the hull, so the builder's copy is ours to
     * free -- leaking it once per pin per rack adds up fast. */
    b3DestroyHull(hull);
    return shape_store(L, id, (int)luaL_checkinteger(L, 1));
}

/* b3.shape_cone(body, height, radius1, radius2 [, yOffset [, slices
 *               [, density]]])
 *
 * A tapered hull: radius1 at the bottom, radius2 at the top. Equal radii
 * give a cylinder; a zero top gives a true cone. This is the piece that
 * makes a pin's neck flare into its belly instead of stepping.
 *
 * b3CreateCone takes no yOffset the way b3CreateCylinder does, so the
 * offset is applied by cloning through a transform. Without this a stacked
 * profile could only ever have ONE cone in it, at the origin. */
static int l_shape_cone(lua_State *L) {
    body_slot *b = body_at(L, 1);
    float h  = px2m(luaL_checknumber(L, 2));
    float r1 = px2m(luaL_checknumber(L, 3));
    float r2 = px2m(luaL_checknumber(L, 4));
    float yo = px2m(luaL_optnumber(L, 5, 0.0));
    int slices = (int)luaL_optinteger(L, 6, 12);
    if (slices < 3)  slices = 3;
    if (slices > 64) slices = 64;
    b3ShapeDef def = b3DefaultShapeDef();
    def.density = (float)luaL_optnumber(L, 7, 1.0);
    b3HullData *hull = b3CreateCone(h, r1, r2, slices);
    if (!hull) luaL_error(L, "b3: could not build cone hull");
    b3HullData *placed = hull;
    if (yo != 0.0f) {
        b3Transform xf = b3Transform_identity;
        xf.p = (b3Vec3){ 0, yo, 0 };
        placed = b3CloneAndTransformHull(hull, xf, (b3Vec3){ 1, 1, 1 });
        if (!placed) { b3DestroyHull(hull); luaL_error(L, "b3: could not place cone hull"); }
    }
    b3ShapeId id = b3CreateHullShape(b->id, &def, placed);
    if (placed != hull) b3DestroyHull(placed);
    b3DestroyHull(hull);
    return shape_store(L, id, (int)luaL_checkinteger(L, 1));
}

/* ── surface material ───────────────────────────────────────────────────
 *
 * Box3D's shape defaults are friction 0.6, restitution 0, rolling
 * resistance 0. That is a sensible crate; it is NOT a billiard ball, which
 * needs a real bounce off a cushion and needs to eventually STOP rolling.
 * Without these three the solver is technically correct and the game is
 * unplayable, so they are part of the base surface, not an extra.
 *
 * rollingResistance has no standalone setter in Box3D — it lives in the
 * material struct — so all three go through get/modify/set on the whole
 * material rather than the individual Set* calls. */
static int l_shape_set_friction(lua_State *L) {
    b3Shape_SetFriction(shape_at(L, 1)->id, (float)luaL_checknumber(L, 2));
    return 0;
}
static int l_shape_get_friction(lua_State *L) {
    lua_pushnumber(L, b3Shape_GetFriction(shape_at(L, 1)->id));
    return 1;
}
static int l_shape_set_restitution(lua_State *L) {
    b3Shape_SetRestitution(shape_at(L, 1)->id, (float)luaL_checknumber(L, 2));
    return 0;
}
static int l_shape_get_restitution(lua_State *L) {
    lua_pushnumber(L, b3Shape_GetRestitution(shape_at(L, 1)->id));
    return 1;
}
/* b3.shape_set_rolling_resistance(shape, r) — the reason a struck ball
 * coasts to a stop instead of rolling forever on a frictionless plane. */
static int l_shape_set_rolling(lua_State *L) {
    b3ShapeId id = shape_at(L, 1)->id;
    b3SurfaceMaterial m = b3Shape_GetSurfaceMaterial(id);
    m.rollingResistance = (float)luaL_checknumber(L, 2);
    b3Shape_SetSurfaceMaterial(id, m);
    return 0;
}
static int l_shape_get_rolling(lua_State *L) {
    lua_pushnumber(L, b3Shape_GetSurfaceMaterial(shape_at(L, 1)->id).rollingResistance);
    return 1;
}
/* b3.shape_set_material(shape, friction, restitution [, rolling]) — the
 * common case in one call, since these three are almost always set together. */
static int l_shape_set_material(lua_State *L) {
    b3ShapeId id = shape_at(L, 1)->id;
    b3SurfaceMaterial m = b3Shape_GetSurfaceMaterial(id);
    m.friction          = (float)luaL_checknumber(L, 2);
    m.restitution       = (float)luaL_checknumber(L, 3);
    m.rollingResistance = (float)luaL_optnumber(L, 4, m.rollingResistance);
    b3Shape_SetSurfaceMaterial(id, m);
    return 0;
}
static int l_shape_set_density(lua_State *L) {
    /* updateBodyMass=true: otherwise the body keeps the mass it computed at
     * create time and the new density silently does nothing. */
    b3Shape_SetDensity(shape_at(L, 1)->id, (float)luaL_checknumber(L, 2), true);
    return 0;
}
static int l_shape_get_density(lua_State *L) {
    lua_pushnumber(L, b3Shape_GetDensity(shape_at(L, 1)->id));
    return 1;
}
static int l_shape_destroy(lua_State *L) {
    int h = (int)luaL_checkinteger(L, 1);
    if (h < 1 || h > MAX_SHAPES || !shapes[h - 1].active) return 0;
    b3DestroyShape(shapes[h - 1].id, true);
    shapes[h - 1].active = 0;
    return 0;
}
/* Hit events are OFF by default in Box3D. A cart that wants collision
 * sounds must opt the shape in, and then read b3.contact_events(). */
static int l_shape_enable_hit_events(lua_State *L) {
    b3Shape_EnableHitEvents(shape_at(L, 1)->id, lua_toboolean(L, 2));
    return 0;
}
static int l_shape_enable_contact_events(lua_State *L) {
    b3Shape_EnableContactEvents(shape_at(L, 1)->id, lua_toboolean(L, 2));
    return 0;
}

/* ── damping, sleep, and the rest of the body surface ──────────────────
 *
 * Damping is what makes a rolling ball lose speed to the cloth; sleep is
 * what lets the game know the table has settled and it is the next
 * player's turn. Neither was reachable from Lua. */
static int l_body_set_linear_damping(lua_State *L) {
    b3Body_SetLinearDamping(body_at(L, 1)->id, (float)luaL_checknumber(L, 2));
    return 0;
}
static int l_body_get_linear_damping(lua_State *L) {
    lua_pushnumber(L, b3Body_GetLinearDamping(body_at(L, 1)->id));
    return 1;
}
static int l_body_set_angular_damping(lua_State *L) {
    b3Body_SetAngularDamping(body_at(L, 1)->id, (float)luaL_checknumber(L, 2));
    return 0;
}
static int l_body_get_angular_damping(lua_State *L) {
    lua_pushnumber(L, b3Body_GetAngularDamping(body_at(L, 1)->id));
    return 1;
}
static int l_body_angular_velocity(lua_State *L) {
    b3Vec3 w = b3Body_GetAngularVelocity(body_at(L, 1)->id);
    lua_pushnumber(L, w.x); lua_pushnumber(L, w.y); lua_pushnumber(L, w.z);
    return 3;
}
/* radians/sec, so NOT scaled by pixels_per_meter */
static int l_body_set_angular_velocity(lua_State *L) {
    b3Vec3 w;
    w.x = (float)luaL_optnumber(L, 2, 0);
    w.y = (float)luaL_optnumber(L, 3, 0);
    w.z = (float)luaL_optnumber(L, 4, 0);
    b3Body_SetAngularVelocity(body_at(L, 1)->id, w);
    return 0;
}
static int l_body_apply_torque(lua_State *L) {
    b3Vec3 t;
    t.x = (float)luaL_optnumber(L, 2, 0);
    t.y = (float)luaL_optnumber(L, 3, 0);
    t.z = (float)luaL_optnumber(L, 4, 0);
    b3Body_ApplyTorque(body_at(L, 1)->id, t, true);
    return 0;
}
static int l_body_apply_angular_impulse(lua_State *L) {
    b3Vec3 t;
    t.x = (float)luaL_optnumber(L, 2, 0);
    t.y = (float)luaL_optnumber(L, 3, 0);
    t.z = (float)luaL_optnumber(L, 4, 0);
    b3Body_ApplyAngularImpulse(body_at(L, 1)->id, t, true);
    return 0;
}
static int l_body_is_awake(lua_State *L) {
    lua_pushboolean(L, b3Body_IsAwake(body_at(L, 1)->id));
    return 1;
}
static int l_body_set_awake(lua_State *L) {
    b3Body_SetAwake(body_at(L, 1)->id, lua_toboolean(L, 2));
    return 0;
}
static int l_body_enable_sleep(lua_State *L) {
    b3Body_EnableSleep(body_at(L, 1)->id, lua_toboolean(L, 2));
    return 0;
}
/* Below this speed the body is allowed to fall asleep. In px/s in, meters
 * out, same as every other length in this binding. */
static int l_body_set_sleep_threshold(lua_State *L) {
    b3Body_SetSleepThreshold(body_at(L, 1)->id, px2m(luaL_checknumber(L, 2)));
    return 0;
}
static int l_body_get_sleep_threshold(lua_State *L) {
    lua_pushnumber(L, m2px(b3Body_GetSleepThreshold(body_at(L, 1)->id)));
    return 1;
}
static int l_body_set_type(lua_State *L) {
    int t = (int)luaL_checkinteger(L, 2);
    b3Body_SetType(body_at(L, 1)->id, (b3BodyType)(t < 0 ? 0 : (t > 2 ? 2 : t)));
    return 0;
}
static int l_body_get_type(lua_State *L) {
    lua_pushinteger(L, (int)b3Body_GetType(body_at(L, 1)->id));
    return 1;
}
static int l_body_set_bullet(lua_State *L) {
    /* Continuous collision. A hard-struck ball can cross a cushion's
     * thickness inside one step; this is what stops it tunnelling out. */
    b3Body_SetBullet(body_at(L, 1)->id, lua_toboolean(L, 2));
    return 0;
}
static int l_body_is_bullet(lua_State *L) {
    lua_pushboolean(L, b3Body_IsBullet(body_at(L, 1)->id));
    return 1;
}
static int l_body_set_gravity_scale(lua_State *L) {
    b3Body_SetGravityScale(body_at(L, 1)->id, (float)luaL_checknumber(L, 2));
    return 0;
}
static int l_body_get_gravity_scale(lua_State *L) {
    lua_pushnumber(L, b3Body_GetGravityScale(body_at(L, 1)->id));
    return 1;
}
static int l_body_enable_hit_events(lua_State *L) {
    b3Body_EnableHitEvents(body_at(L, 1)->id, lua_toboolean(L, 2));
    return 0;
}
static int l_body_apply_impulse_at(lua_State *L) {
    /* Off-centre impulse: the difference between a centre-ball hit and one
     * with english on it. */
    b3Vec3 imp = vec3_arg(L, 2);
    b3Pos  at  = pos_arg(L, 5);
    b3Body_ApplyLinearImpulse(body_at(L, 1)->id, imp, at, true);
    return 0;
}

/* ── contact events ─────────────────────────────────────────────────────
 *
 * b3.contact_events(world) -> { hits = { {shapeA, shapeB, x,y,z, nx,ny,nz,
 * speed}, ... }, begins = {{shapeA, shapeB}, ...}, ends = {...} }
 *
 * Shape IDs are mapped back to cart-side handles; a shape the cart never
 * created (or already destroyed) reports 0 rather than a dangling index. */
static int shape_handle_of(b3ShapeId id) {
    for (int i = 0; i < MAX_SHAPES; i++)
        if (shapes[i].active && B3_ID_EQUALS(shapes[i].id, id)) return i + 1;
    return 0;
}

static int l_contact_events(lua_State *L) {
    int wh = (int)luaL_checkinteger(L, 1);
    if (wh < 1 || wh > MAX_WORLDS || !world_alive[wh - 1])
        return luaL_error(L, "b3: bad world handle %d", wh);
    b3ContactEvents ev = b3World_GetContactEvents(worlds[wh - 1]);

    lua_newtable(L);

    lua_newtable(L);                                   /* hits */
    for (int i = 0; i < ev.hitCount; i++) {
        b3ContactHitEvent *h = &ev.hitEvents[i];
        lua_newtable(L);
        lua_pushinteger(L, shape_handle_of(h->shapeIdA)); lua_setfield(L, -2, "a");
        lua_pushinteger(L, shape_handle_of(h->shapeIdB)); lua_setfield(L, -2, "b");
        lua_pushnumber(L, m2px(h->point.x)); lua_setfield(L, -2, "x");
        lua_pushnumber(L, m2px(h->point.y)); lua_setfield(L, -2, "y");
        lua_pushnumber(L, m2px(h->point.z)); lua_setfield(L, -2, "z");
        lua_pushnumber(L, h->normal.x); lua_setfield(L, -2, "nx");
        lua_pushnumber(L, h->normal.y); lua_setfield(L, -2, "ny");
        lua_pushnumber(L, h->normal.z); lua_setfield(L, -2, "nz");
        /* approachSpeed is m/s from the solver; report px/s to match the
         * cart's own units, so a volume curve written in pixels works. */
        lua_pushnumber(L, m2px(h->approachSpeed)); lua_setfield(L, -2, "speed");
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "hits");

    lua_newtable(L);                                   /* begins */
    for (int i = 0; i < ev.beginCount; i++) {
        lua_newtable(L);
        lua_pushinteger(L, shape_handle_of(ev.beginEvents[i].shapeIdA)); lua_setfield(L, -2, "a");
        lua_pushinteger(L, shape_handle_of(ev.beginEvents[i].shapeIdB)); lua_setfield(L, -2, "b");
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "begins");

    lua_newtable(L);                                   /* ends */
    for (int i = 0; i < ev.endCount; i++) {
        lua_newtable(L);
        lua_pushinteger(L, shape_handle_of(ev.endEvents[i].shapeIdA)); lua_setfield(L, -2, "a");
        lua_pushinteger(L, shape_handle_of(ev.endEvents[i].shapeIdB)); lua_setfield(L, -2, "b");
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "ends");

    return 1;
}

/* b3.world_set_hit_threshold(world, px_per_sec) — below this an impact
 * raises no hit event, so a rack of balls jostling does not machine-gun
 * the click sound. */
static int l_world_set_hit_threshold(lua_State *L) {
    int wh = (int)luaL_checkinteger(L, 1);
    if (wh < 1 || wh > MAX_WORLDS || !world_alive[wh - 1])
        return luaL_error(L, "b3: bad world handle %d", wh);
    b3World_SetHitEventThreshold(worlds[wh - 1], px2m(luaL_checknumber(L, 2)));
    return 0;
}

static int l_world_set_gravity(lua_State *L) {
    int wh = (int)luaL_checkinteger(L, 1);
    if (wh < 1 || wh > MAX_WORLDS || !world_alive[wh - 1])
        return luaL_error(L, "b3: bad world handle %d", wh);
    b3World_SetGravity(worlds[wh - 1], vec3_arg(L, 2));
    return 0;
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

    { "body_set_linear_damping",  l_body_set_linear_damping },
    { "body_get_linear_damping",  l_body_get_linear_damping },
    { "body_set_angular_damping", l_body_set_angular_damping },
    { "body_get_angular_damping", l_body_get_angular_damping },
    { "body_angular_velocity",     l_body_angular_velocity },
    { "body_set_angular_velocity", l_body_set_angular_velocity },
    { "body_apply_torque",          l_body_apply_torque },
    { "body_apply_angular_impulse", l_body_apply_angular_impulse },
    { "body_apply_impulse_at",      l_body_apply_impulse_at },
    { "body_is_awake",            l_body_is_awake },
    { "body_set_awake",           l_body_set_awake },
    { "body_enable_sleep",        l_body_enable_sleep },
    { "body_set_sleep_threshold", l_body_set_sleep_threshold },
    { "body_get_sleep_threshold", l_body_get_sleep_threshold },
    { "body_set_type",            l_body_set_type },
    { "body_get_type",            l_body_get_type },
    { "body_set_bullet",          l_body_set_bullet },
    { "body_is_bullet",           l_body_is_bullet },
    { "body_set_gravity_scale",   l_body_set_gravity_scale },
    { "body_get_gravity_scale",   l_body_get_gravity_scale },
    { "body_enable_hit_events",   l_body_enable_hit_events },

    { "shape_box",          l_shape_box },
    { "shape_sphere",       l_shape_sphere },
    { "shape_capsule",      l_shape_capsule },
    { "shape_cylinder",     l_shape_cylinder },
    { "shape_cone",         l_shape_cone },
    { "shape_destroy",      l_shape_destroy },

    { "shape_set_friction",     l_shape_set_friction },
    { "shape_get_friction",     l_shape_get_friction },
    { "shape_set_restitution",  l_shape_set_restitution },
    { "shape_get_restitution",  l_shape_get_restitution },
    { "shape_set_rolling_resistance", l_shape_set_rolling },
    { "shape_get_rolling_resistance", l_shape_get_rolling },
    { "shape_set_material",     l_shape_set_material },
    { "shape_set_density",      l_shape_set_density },
    { "shape_get_density",      l_shape_get_density },
    { "shape_enable_hit_events",     l_shape_enable_hit_events },
    { "shape_enable_contact_events", l_shape_enable_contact_events },

    { "contact_events",          l_contact_events },
    { "world_set_hit_threshold", l_world_set_hit_threshold },
    { "world_set_gravity",       l_world_set_gravity },

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
