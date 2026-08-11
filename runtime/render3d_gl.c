/*
 * render3d_gl.c - the 3D pipeline for the Lua engine.
 *
 * A second pipeline beside render2d_gl.c, sharing its GL context and nothing
 * else. render3d_gl.h explains why the two cannot share a vertex format, a
 * VAO, or a texture cache; this file is the implementation of that split.
 *
 * WHAT THIS DOES NOT DO, deliberately: there is no camera, no matrix stack,
 * no lighting, no model loader, no scene graph. Those live in Lua, in the
 * cart's own 3D library (g3d and friends). This layer is the GPU seam LOVE
 * exposes -- meshes with a custom vertex format, a depth buffer, cull state,
 * and a cart-supplied vertex shader that does the transform -- and no more.
 * Every LOVE 3D library in the wild is written against exactly that surface,
 * so matching it is what makes them run unmodified.
 */
#include "render3d_gl.h"

#ifdef WCL_ENABLE_GL2D

#define WC_USE_GL
#include "wasmcart.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>

/* Enums this file needs that wasmcart.h does not spell out. */
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW 0x88E4
#define GL_TRIANGLES 0x0004
#define GL_UNSIGNED_INT 0x1405
#define GL_UNPACK_ALIGNMENT 0x0CF5

/* The seam into render2d_gl.c. Not in a header: nothing else may call it. */
extern void   wcl_r2d__flush_for_3d(void);
extern GLuint wcl_r2d__active_program(void);
extern void   wcl_r2d__rebind_2d_state(void);
extern void   wcl_r2d__invalidate_texture_binding(void);
extern GLuint wcl_r2d__upload_standalone(const void *pixels, int w, int h);
extern int    wcl_r2d_active(void);
/* Restore the screen's viewport and clip scaling after a render target is
 * unbound: the 2D path derives clip space from the SCREEN size, and leaving
 * a target's viewport in place would squash every subsequent 2D draw. */
extern void   wcl_r2d__screen_viewport(void);
/* Set the clip-space scale the 2D path uses, so a 2D draw into a GPU render
 * target is scaled for THAT target rather than for the screen. */
extern void   wcl_r2d__set_clip_size(int w, int h);
/* The program object for a cart shader handle (not just the active one), so
 * a uniform can be set on a shader that is not currently bound -- which is
 * what LOVE's Shader:send does. */
extern GLuint wcl_r2d__program_of(int handle);
/* Re-bind whatever program was current before a send temporarily switched. */
extern void   wcl_r2d__restore_program(void);
/* The SHARED texture-unit allocator. Both pipelines hand out units 1..15
 * from one table, so an Image sampler and a render-target sampler in the
 * same shader can never be assigned the same unit. Returns 0 when full. */
extern int    wcl_r2d__claim_texture_unit(int handle, GLint loc, GLuint tex,
                                          GLenum textarget);

/* ── attribute locations ──────────────────────────────────────────────
 *
 * These are the names LOVE's shader preprocessor exposes, and the numbers
 * are pinned so one 3D VAO stays valid across every cart shader -- the same
 * doctrine render2d_gl.c applies to its own four, for the same reason.
 *
 * They are pinned to the SAME indices 0..3 as the 2D pipeline, which is safe
 * only because each pipeline binds its own VAO before drawing: a VAO records
 * the attribute pointers per index, so index 0 means "2D a_pos, stride 36"
 * under the 2D VAO and "3D VertexPosition, stride 48" under this one. Get
 * that wrong and geometry reads as garbage rather than failing loudly, which
 * is why wcl_r3d_leave() rebinds the 2D VAO after every 3D draw. */
#define A3_POS    0
#define A3_UV     1
#define A3_COLOR  2
#define A3_NORMAL 3

/* These MUST match render2d_gl.c's ATTR_*, which is what bind_attribs_and_link
 * pins for every program. If they drift, nothing fails loudly: the shader
 * reads colour from the normal attribute and the model renders in the
 * colours of its own surface directions, which looks like a lighting choice.
 * A compile error is the only honest failure mode here, so assert it. */
#if A3_POS != 0 || A3_UV != 1 || A3_COLOR != 2 || A3_NORMAL != 3
#error "render3d_gl.c A3_* attribute indices disagree with render2d_gl.c ATTR_*"
#endif

#define MAX_MESHES3D 64
typedef struct {
    GLuint vbo, ebo;
    int count;          /* vertices in the VBO */
    int cap;            /* vertices allocated, for in-place update */
    int index_count;    /* 0 = draw arrays */
    GLuint tex;         /* standalone texture, or 0 */
    const void *tex_key;
    /* A DECLARED format, or n_attribs == 0 for the built-in wcl_vertex3d_t
     * layout. Kept per mesh because the VAO is shared: the attribute
     * pointers are re-established from this on every draw. */
    wcl_attrib_t attribs[WCL_MAX_ATTRIBS];
    int n_attribs;
    int stride;
    int used;
} mesh3d_t;
static mesh3d_t meshes3d[MAX_MESHES3D];

/* One VAO for every 3D mesh: the layout is identical across meshes (the
 * format is fixed), so per-mesh VAOs would differ only in which VBO they
 * reference, and that is a bind either way. */
static GLuint vao3d;
static int vao3d_ready;

static uint32_t depth_compare;   /* 0 = test disabled */
static int depth_write;
static int cull_mode;            /* 0 none, 1 back, 2 front */
static int front_face_cw;
static int used_this_frame;
/* Mirrors of the GL state this file switches on, so a draw does not re-issue
 * state the driver already has. Reset by wcl_r3d_leave, which turns both off. */
static int depth_test_on;
static int cull_on;

static mesh3d_t *mesh_by_handle(int h) {
    if (h < 0 || h >= MAX_MESHES3D || !meshes3d[h].used) return NULL;
    return &meshes3d[h];
}

/* Engine-supplied uniform locations, cached per program object. There are at
 * most MAX_SHADERS cart programs (8 in render2d_gl.c), so a tiny linear
 * cache covers every program a cart can have with room to spare. */
typedef struct {
    GLuint prog;
    GLint love_color, textured, tex;
    int used;
} uniform_cache_t;
#define MAX_UNIFORM_CACHE 16
static uniform_cache_t uniform_cache[MAX_UNIFORM_CACHE];

static const uniform_cache_t *uniforms_for(GLuint prog) {
    for (int i = 0; i < MAX_UNIFORM_CACHE; i++)
        if (uniform_cache[i].used && uniform_cache[i].prog == prog)
            return &uniform_cache[i];
    int slot = -1;
    for (int i = 0; i < MAX_UNIFORM_CACHE; i++)
        if (!uniform_cache[i].used) { slot = i; break; }
    /* Full: fall back to slot 0 and overwrite it. A cart cannot reach this
     * (8 shader slots), and returning a wrong-program cache would be worse
     * than one extra lookup. */
    if (slot < 0) slot = 0;
    uniform_cache_t *u = &uniform_cache[slot];
    u->prog = prog;
    u->love_color = glGetUniformLocation(prog, "love_Color");
    u->textured = glGetUniformLocation(prog, "u_textured");
    u->tex = glGetUniformLocation(prog, "u_tex");
    u->used = 1;
    return u;
}

/* The 3D VAO exists; its attribute POINTERS do not live here.
 *
 * glVertexAttribPointer records the currently bound GL_ARRAY_BUFFER into the
 * VAO along with the offsets, so the layout is a property of (VAO, VBO)
 * together, not of the VAO alone. With one VAO shared across meshes, the
 * pointers must therefore be re-established against each mesh's own VBO at
 * draw time -- that is bind_mesh_attribs, and it is why this function only
 * creates the object. */
static int ensure_vao(void) {
    if (vao3d_ready) return 1;
    if (!wcl_r2d_active()) return 0;
    glGenVertexArrays(1, &vao3d);
    vao3d_ready = 1;
    return 1;
}

/* ── generic vertex formats ───────────────────────────────────────────
 *
 * A REGISTRY of every attribute name any mesh has declared, in first-seen
 * order. The index a name gets here is the index it binds to everywhere:
 * in each mesh's VAO layout and in every shader program's
 * glBindAttribLocation. One global registry rather than per-format indices,
 * because a shader is linked once and drawn with many meshes -- if two
 * formats numbered "VertexTangent" differently, a program linked against
 * one would read garbage when drawn with the other.
 *
 * Indices 0..3 are pre-seeded with the built-in names so the fixed-layout
 * path and the declared-format path agree, and so a declared format that
 * happens to be the standard one costs nothing extra. */
static struct {
    char name[WCL_ATTRIB_NAME_MAX];
    int used;
} attrib_registry[WCL_MAX_ATTRIBS];
static int attrib_registry_init;

static void registry_seed(void) {
    if (attrib_registry_init) return;
    attrib_registry_init = 1;
    /* MUST match A3_POS/A3_UV/A3_COLOR/A3_NORMAL. */
    const char *seed[4] = { "VertexPosition", "VertexTexCoord",
                            "VertexColor", "VertexNormal" };
    for (int i = 0; i < 4; i++) {
        snprintf(attrib_registry[i].name, WCL_ATTRIB_NAME_MAX, "%s", seed[i]);
        attrib_registry[i].used = 1;
    }
}

/* The index for `name`, registering it if new. -1 when the registry is
 * full, which is a hard refusal rather than a silent collision. */
static int attrib_index(const char *name) {
    registry_seed();
    for (int i = 0; i < WCL_MAX_ATTRIBS; i++)
        if (attrib_registry[i].used && !strcmp(attrib_registry[i].name, name))
            return i;
    for (int i = 0; i < WCL_MAX_ATTRIBS; i++) {
        if (!attrib_registry[i].used) {
            snprintf(attrib_registry[i].name, WCL_ATTRIB_NAME_MAX, "%s", name);
            attrib_registry[i].used = 1;
            return i;
        }
    }
    return -1;
}

int wcl_r3d_format_stride(const wcl_attrib_t *attribs, int n_attribs) {
    int stride = 0;
    for (int i = 0; i < n_attribs; i++)
        stride += attribs[i].is_byte ? 4 : (attribs[i].components * 4);
    return stride;
}

void wcl_r3d_bind_format_attribs(unsigned int program) {
    registry_seed();
    /* Bind EVERY known name. Binding a name the program does not declare is
     * a no-op in GL, so one unconditional pass covers every shader without
     * knowing which format it will be drawn with. */
    for (int i = 0; i < WCL_MAX_ATTRIBS; i++)
        if (attrib_registry[i].used)
            glBindAttribLocation((GLuint)program, (GLuint)i, attrib_registry[i].name);
}

/* Point a declared-format mesh's attributes at its VBO. */
static void bind_format_attribs(mesh3d_t *m) {
    glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
    /* Which indices THIS mesh supplies. Everything else must be DISABLED:
     * the VAO is shared, so a previous mesh's enabled array would still be
     * pointing into a buffer this mesh does not use -- reading whatever is
     * there, at this mesh's vertex count. That is a use-after-free the
     * driver will happily rasterize. */
    int supplied[WCL_MAX_ATTRIBS] = { 0 };
    size_t offset = 0;
    for (int i = 0; i < m->n_attribs; i++) {
        const wcl_attrib_t *a = &m->attribs[i];
        int idx = attrib_index(a->name);
        if (idx < 0) continue;
        supplied[idx] = 1;
        glEnableVertexAttribArray((GLuint)idx);
        if (a->is_byte) {
            /* NORMALIZED: a byte attribute carries 0..255 and the shader
             * reads 0..1, which is what LOVE does and what the packing code
             * on the other side assumes (it multiplies by 255). Passing
             * normalized=0 here delivers 0..255 to the shader and every
             * colour and normal comes out saturated. */
            glVertexAttribPointer((GLuint)idx, 4, GL_UNSIGNED_BYTE, 1,
                                  (GLsizei)m->stride, (const void *)offset);
            offset += 4;
        } else {
            glVertexAttribPointer((GLuint)idx, a->components, GL_FLOAT, 0,
                                  (GLsizei)m->stride, (const void *)offset);
            offset += (size_t)a->components * 4;
        }
    }
    for (int i = 0; i < WCL_MAX_ATTRIBS; i++)
        if (!supplied[i]) glDisableVertexAttribArray((GLuint)i);
    if (m->index_count) glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m->ebo);
}

int wcl_r3d_mesh_new_format(const wcl_attrib_t *attribs, int n_attribs,
                            const void *verts, int count, int stride) {
    if (!wcl_r2d_active() || !attribs || n_attribs < 1 || count < 1) return -1;
    if (n_attribs > WCL_MAX_ATTRIBS) {
        WC_LOG("love.graphics.newMesh: too many vertex attributes (8 max)");
        return -1;
    }
    if (!ensure_vao()) return -1;

    /* Register every name up front, so a format that would overflow the
     * registry fails HERE rather than half-bound at draw time. */
    for (int i = 0; i < n_attribs; i++) {
        if (attrib_index(attribs[i].name) < 0) {
            char b[160];
            int n = snprintf(b, sizeof b,
                             "love.graphics.newMesh: out of vertex attribute "
                             "slots (8 max across the whole cart); '%s' does "
                             "not fit", attribs[i].name);
            if (n > 0) wc_log(b, (unsigned)n);
            return -1;
        }
    }

    int slot = -1;
    for (int i = 0; i < MAX_MESHES3D; i++) if (!meshes3d[i].used) { slot = i; break; }
    if (slot < 0) {
        WC_LOG("love.graphics.newMesh: out of 3D mesh slots (64 max)");
        return -1;
    }
    mesh3d_t *m = &meshes3d[slot];
    memset(m, 0, sizeof *m);
    for (int i = 0; i < n_attribs; i++) m->attribs[i] = attribs[i];
    m->n_attribs = n_attribs;
    m->stride = stride > 0 ? stride : wcl_r3d_format_stride(attribs, n_attribs);

    wcl_r2d__flush_for_3d();
    glGenBuffers(1, &m->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)count * m->stride),
                 verts, GL_STATIC_DRAW);
    m->count = count;
    m->cap = count;
    m->used = 1;
    wcl_r2d__rebind_2d_state();
    return slot;
}

/* Point the 3D attributes at `m`'s VBO. Must run with vao3d bound. */
static void bind_mesh_attribs(mesh3d_t *m) {
    if (m->n_attribs > 0) { bind_format_attribs(m); return; }
    glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
    const GLsizei stride = (GLsizei)sizeof(wcl_vertex3d_t);
    glEnableVertexAttribArray(A3_POS);
    glVertexAttribPointer(A3_POS, 3, GL_FLOAT, 0, stride,
                          (const void *)(size_t)offsetof(wcl_vertex3d_t, x));
    glEnableVertexAttribArray(A3_UV);
    glVertexAttribPointer(A3_UV, 2, GL_FLOAT, 0, stride,
                          (const void *)(size_t)offsetof(wcl_vertex3d_t, u));
    /* Offsets follow wcl_vertex3d_t's field order, NOT the attribute index
     * order: normal sits at byte 20 in the struct but binds to index 3. */
    glEnableVertexAttribArray(A3_NORMAL);
    glVertexAttribPointer(A3_NORMAL, 3, GL_FLOAT, 0, stride,
                          (const void *)(size_t)offsetof(wcl_vertex3d_t, nx));
    glEnableVertexAttribArray(A3_COLOR);
    glVertexAttribPointer(A3_COLOR, 4, GL_FLOAT, 0, stride,
                          (const void *)(size_t)offsetof(wcl_vertex3d_t, r));
    /* Disable anything a DECLARED-format mesh enabled earlier: the VAO is
     * shared, and a leftover array still points into that mesh's buffer.
     * Only indices past the built-in four can be affected. */
    for (int i = 4; i < WCL_MAX_ATTRIBS; i++)
        glDisableVertexAttribArray((GLuint)i);
    if (m->index_count) glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m->ebo);
}

int wcl_r3d_mesh_new(const wcl_vertex3d_t *verts, int count) {
    if (!wcl_r2d_active() || !verts || count < 1) return -1;
    if (!ensure_vao()) return -1;
    int slot = -1;
    for (int i = 0; i < MAX_MESHES3D; i++) if (!meshes3d[i].used) { slot = i; break; }
    if (slot < 0) {
        WC_LOG("love.graphics.newMesh: out of 3D mesh slots (64 max)");
        return -1;
    }
    mesh3d_t *m = &meshes3d[slot];
    memset(m, 0, sizeof *m);
    wcl_r2d__flush_for_3d();
    glGenBuffers(1, &m->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
    /* STATIC_DRAW: 3D geometry is uploaded once and drawn for the life of
     * the cart. That is the difference that makes a 20k-triangle model cost
     * one bind and one draw per frame instead of a 960 KB upload. */
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)((size_t)count * sizeof(wcl_vertex3d_t)),
                 verts, GL_STATIC_DRAW);
    m->count = count;
    m->cap = count;
    m->used = 1;
    wcl_r2d__rebind_2d_state();
    return slot;
}

/* Replace a declared-format mesh's bytes wholesale. The caller packed them
 * to the format's stride, so this is a straight glBufferSubData -- which is
 * the entire point of the path: a renderer that builds its own interleaved
 * buffer pays no per-vertex cost crossing into C. */
int wcl_r3d_mesh_set_bytes(int handle, const void *data, int len) {
    mesh3d_t *m = mesh_by_handle(handle);
    if (!m || !data || len < 1) return 0;
    if (m->stride <= 0) return 0;              /* not a declared-format mesh */
    if (len > m->cap * m->stride) return 0;    /* would overrun the buffer */
    wcl_r2d__flush_for_3d();
    glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)len, data);
    m->count = len / m->stride;
    wcl_r2d__rebind_2d_state();
    return 1;
}

int wcl_r3d_mesh_update(int handle, const wcl_vertex3d_t *verts, int count) {
    mesh3d_t *m = mesh_by_handle(handle);
    if (!m || !verts || count < 1 || count > m->cap) return 0;
    wcl_r2d__flush_for_3d();
    glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    (GLsizeiptr)((size_t)count * sizeof(wcl_vertex3d_t)), verts);
    m->count = count;
    wcl_r2d__rebind_2d_state();
    return 1;
}

int wcl_r3d_mesh_set_indices(int handle, const uint32_t *idx, int n) {
    mesh3d_t *m = mesh_by_handle(handle);
    if (!m) return 0;
    wcl_r2d__flush_for_3d();
    if (!idx || n < 1) {
        m->index_count = 0;
        return 1;
    }
    if (!m->ebo) glGenBuffers(1, &m->ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 (GLsizeiptr)((size_t)n * sizeof(uint32_t)), idx, GL_STATIC_DRAW);
    m->index_count = n;
    wcl_r2d__rebind_2d_state();
    return 1;
}

int wcl_r3d_mesh_set_texture(int handle, const void *pixels, int w, int h) {
    mesh3d_t *m = mesh_by_handle(handle);
    if (!m) return 0;
    if (!pixels) { m->tex = 0; m->tex_key = NULL; return 1; }
    if (m->tex && m->tex_key == pixels) return 1;   /* already current */
    GLuint t = wcl_r2d__upload_standalone(pixels, w, h);
    if (!t) return 0;
    m->tex = t;
    m->tex_key = pixels;
    return 1;
}

void wcl_r3d_mesh_free(int handle) {
    mesh3d_t *m = mesh_by_handle(handle);
    if (!m) return;
    if (m->vbo) glDeleteBuffers(1, &m->vbo);
    if (m->ebo) glDeleteBuffers(1, &m->ebo);
    if (m->tex) glDeleteTextures(1, &m->tex);
    memset(m, 0, sizeof *m);
}

/* ── GPU render targets ───────────────────────────────────────────────
 *
 * See render3d_gl.h for why these are separate from the CPU-backed 2D
 * canvas. Here is the mechanism.
 */

/* Each format is a (internalformat, format, type) triple plus whether it is
 * a depth format. The triple must be one GLES 3.0 accepts TOGETHER --
 * mismatching them is the classic silent GL_INVALID_OPERATION that leaves a
 * texture allocated but undefined. */
typedef struct {
    const char *name;
    GLenum internal, format, type;
    int is_depth;
    int channels;
} fmt_info_t;

static const fmt_info_t FORMATS[WCL_FMT__COUNT] = {
    [WCL_FMT_RGBA8]   = { "rgba8",   GL_RGBA8,   GL_RGBA, GL_UNSIGNED_BYTE, 0, 4 },
    [WCL_FMT_R8]      = { "r8",      GL_R8,      GL_RED,  GL_UNSIGNED_BYTE, 0, 1 },
    [WCL_FMT_RG8]     = { "rg8",     GL_RG8,     GL_RG,   GL_UNSIGNED_BYTE, 0, 2 },
    /* HALF_FLOAT for the 16F family: GLES 3.0 pairs *16F internal formats
     * with GL_HALF_FLOAT or GL_FLOAT, and passing UNSIGNED_BYTE here is
     * accepted by some drivers and rejected by others. */
    [WCL_FMT_R16F]    = { "r16f",    GL_R16F,    GL_RED,  GL_HALF_FLOAT, 0, 1 },
    [WCL_FMT_RG16F]   = { "rg16f",   GL_RG16F,   GL_RG,   GL_HALF_FLOAT, 0, 2 },
    [WCL_FMT_RGBA16F] = { "rgba16f", GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 0, 4 },
    [WCL_FMT_R32F]    = { "r32f",    GL_R32F,    GL_RED,  GL_FLOAT, 0, 1 },
    [WCL_FMT_RGBA32F] = { "rgba32f", GL_RGBA32F, GL_RGBA, GL_FLOAT, 0, 4 },
    [WCL_FMT_DEPTH16] = { "depth16", GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT,
                          GL_UNSIGNED_SHORT, 1, 1 },
    [WCL_FMT_DEPTH24] = { "depth24", GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT,
                          GL_UNSIGNED_INT, 1, 1 },
    [WCL_FMT_DEPTH32F]= { "depth32f",GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT,
                          GL_FLOAT, 1, 1 },
    [WCL_FMT_DEPTH24_STENCIL8] = { "depth24stencil8", GL_DEPTH24_STENCIL8,
                          GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 1, 1 },
};

#define MAX_TARGETS3D 32
typedef struct {
    GLuint tex;
    GLuint fbo;          /* lazily created, for the currently bound layer */
    int fbo_layer;       /* which layer `fbo` is currently attached to */
    int w, h, layers;
    wcl_format_t fmt;
    wcl_textype_t type;
    int mipmaps;
    int used;
} target3d_t;
static target3d_t targets3d[MAX_TARGETS3D];

static int bound_color_count;

static target3d_t *target_by_handle(int h) {
    if (h < 0 || h >= MAX_TARGETS3D || !targets3d[h].used) return NULL;
    return &targets3d[h];
}

static GLenum tex_target_of(const target3d_t *t) {
    switch (t->type) {
        case WCL_TEX_CUBE:   return GL_TEXTURE_CUBE_MAP;
        case WCL_TEX_ARRAY:  return GL_TEXTURE_2D_ARRAY;
        case WCL_TEX_VOLUME: return GL_TEXTURE_3D;
        default:             return GL_TEXTURE_2D;
    }
}

/* Format support is a RUNTIME question. On WebGL2 the float colour formats
 * are renderable only with EXT_color_buffer_float, and a driver that lacks
 * it will allocate the texture happily and then fail to complete the
 * framebuffer -- which surfaces as an empty screen, not an error. So the
 * answer is cached per format by actually trying it once. */
static signed char fmt_ok_cache[WCL_FMT__COUNT];   /* 0 unknown, 1 yes, -1 no */

int wcl_r3d_format_supported(wcl_format_t fmt) {
    if (fmt < 0 || fmt >= WCL_FMT__COUNT) return 0;
    if (!wcl_r2d_active()) return 0;
    if (fmt_ok_cache[fmt]) return fmt_ok_cache[fmt] > 0;

    const fmt_info_t *f = &FORMATS[fmt];
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)f->internal, 4, 4, 0,
                 f->format, f->type, (const void *)0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,
                           f->is_depth ? GL_DEPTH_ATTACHMENT : GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);
    /* A depth-only FBO has no colour attachment, which is incomplete unless
     * the draw/read buffers are set to NONE. */
    if (f->is_depth) {
        GLenum none = GL_NONE;
        glDrawBuffers(1, &none);
    }
    int ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    while (glGetError() != GL_NO_ERROR) { }   /* the probe's own errors are not the cart's */
    wcl_r2d__invalidate_texture_binding();
    wcl_r2d__rebind_2d_state();

    fmt_ok_cache[fmt] = ok ? 1 : -1;
    return ok;
}

int wcl_r3d_target_new(int w, int h, wcl_format_t fmt, wcl_textype_t type,
                       int layers, int mipmaps, int msaa) {
    (void)msaa;   /* MSAA targets would need a resolve blit; not yet */
    if (!wcl_r2d_active() || w < 1 || h < 1) return -1;
    if (fmt < 0 || fmt >= WCL_FMT__COUNT) return -1;
    if (!wcl_r3d_format_supported(fmt)) {
        char b[160];
        int n = snprintf(b, sizeof b,
                         "love.graphics.newCanvas: this driver cannot render to "
                         "format '%s'", FORMATS[fmt].name);
        if (n > 0) wc_log(b, (unsigned)n);
        return -1;
    }
    if (type == WCL_TEX_CUBE) layers = 6;
    if (type == WCL_TEX_2D) layers = 1;
    if (layers < 1) layers = 1;

    int slot = -1;
    for (int i = 0; i < MAX_TARGETS3D; i++) if (!targets3d[i].used) { slot = i; break; }
    if (slot < 0) {
        WC_LOG("love.graphics.newCanvas: out of GPU targets (32 max)");
        return -1;
    }
    target3d_t *t = &targets3d[slot];
    memset(t, 0, sizeof *t);

    wcl_r2d__flush_for_3d();
    const fmt_info_t *f = &FORMATS[fmt];
    const GLenum tt = (type == WCL_TEX_CUBE) ? GL_TEXTURE_CUBE_MAP
                    : (type == WCL_TEX_ARRAY) ? GL_TEXTURE_2D_ARRAY
                    : (type == WCL_TEX_VOLUME) ? GL_TEXTURE_3D : GL_TEXTURE_2D;

    glGenTextures(1, &t->tex);
    glBindTexture(tt, t->tex);
    if (type == WCL_TEX_CUBE) {
        for (int face = 0; face < 6; face++) {
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, (GLint)f->internal,
                         w, h, 0, f->format, f->type, (const void *)0);
        }
    } else if (type == WCL_TEX_ARRAY || type == WCL_TEX_VOLUME) {
        glTexImage3D(tt, 0, (GLint)f->internal, w, h, layers, 0,
                     f->format, f->type, (const void *)0);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, (GLint)f->internal, w, h, 0,
                     f->format, f->type, (const void *)0);
    }

    /* A depth texture sampled with LINEAR is a comparison sampler in GLES,
     * which is not what a renderer reading raw depth wants; and float
     * colour is only linearly filterable with OES_texture_float_linear.
     * NEAREST is the safe default for both, and a cart that wants
     * filtering on a colour target gets it below. */
    const int filterable = !f->is_depth && fmt != WCL_FMT_R32F && fmt != WCL_FMT_RGBA32F;
    const GLenum minf = mipmaps ? GL_LINEAR_MIPMAP_LINEAR
                                : (filterable ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(tt, GL_TEXTURE_MIN_FILTER, (GLint)minf);
    glTexParameteri(tt, GL_TEXTURE_MAG_FILTER, filterable ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(tt, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(tt, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (type == WCL_TEX_VOLUME || type == WCL_TEX_ARRAY)
        glTexParameteri(tt, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    if (mipmaps) glGenerateMipmap(tt);

    t->w = w; t->h = h; t->layers = layers;
    t->fmt = fmt; t->type = type; t->mipmaps = mipmaps;
    t->fbo_layer = -1;
    t->used = 1;

    wcl_r2d__invalidate_texture_binding();
    wcl_r2d__rebind_2d_state();
    return slot;
}

void wcl_r3d_target_free(int handle) {
    target3d_t *t = target_by_handle(handle);
    if (!t) return;
    if (t->fbo) glDeleteFramebuffers(1, &t->fbo);
    if (t->tex) glDeleteTextures(1, &t->tex);
    memset(t, 0, sizeof *t);
}

int wcl_r3d_target_size(int handle, int *w, int *h, int *layers) {
    target3d_t *t = target_by_handle(handle);
    if (!t) return 0;
    if (w) *w = t->w;
    if (h) *h = t->h;
    if (layers) *layers = t->layers;
    return 1;
}

void wcl_r3d_target_generate_mipmaps(int handle) {
    target3d_t *t = target_by_handle(handle);
    if (!t || !t->mipmaps) return;
    wcl_r2d__flush_for_3d();
    const GLenum tt = tex_target_of(t);
    glBindTexture(tt, t->tex);
    glGenerateMipmap(tt);
    wcl_r2d__invalidate_texture_binding();
}

/* Attach one target's chosen layer to a framebuffer attachment point. */
static void attach(GLuint fbo, GLenum attachment, target3d_t *t, int layer) {
    (void)fbo;
    switch (t->type) {
        case WCL_TEX_CUBE:
            glFramebufferTexture2D(GL_FRAMEBUFFER, attachment,
                                   GL_TEXTURE_CUBE_MAP_POSITIVE_X + (layer % 6),
                                   t->tex, 0);
            break;
        case WCL_TEX_ARRAY:
        case WCL_TEX_VOLUME:
            glFramebufferTextureLayer(GL_FRAMEBUFFER, attachment, t->tex, 0, layer);
            break;
        default:
            glFramebufferTexture2D(GL_FRAMEBUFFER, attachment,
                                   GL_TEXTURE_2D, t->tex, 0);
            break;
    }
}

/* One FBO per BIND, owned by the first colour target (or the depth target if
 * there is no colour). Reusing the first target's FBO keeps the common case
 * -- the same set of targets bound every frame -- free of allocation, while
 * still re-attaching when the layer or the target set changes. */
static GLuint bind_fbo;

int wcl_r3d_target_bind(const int *handles, const int *layer, int n,
                        int depth, int depth_layer) {
    if (!wcl_r2d_active()) return 0;
    wcl_r2d__flush_for_3d();

    if (n <= 0 && depth < 0) {                 /* back to the screen */
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        bound_color_count = 0;
        wcl_r2d__screen_viewport();
        wcl_r2d__rebind_2d_state();
        return 1;
    }
    if (n > 8) n = 8;

    if (!bind_fbo) glGenFramebuffers(1, &bind_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, bind_fbo);

    /* Detach every attachment first. A previous bind with more targets would
     * otherwise leave attachment 3 live while this bind only writes 0..1,
     * and the framebuffer would be incomplete or, worse, complete with a
     * stale target still being written. */
    for (int i = 0; i < 8; i++)
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i,
                               GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                           GL_TEXTURE_2D, 0, 0);

    GLenum bufs[8];
    int vw = 0, vh = 0;
    for (int i = 0; i < n; i++) {
        target3d_t *t = target_by_handle(handles[i]);
        if (!t) return 0;
        attach(bind_fbo, GL_COLOR_ATTACHMENT0 + i, t, layer ? layer[i] : 0);
        bufs[i] = (GLenum)(GL_COLOR_ATTACHMENT0 + i);
        if (!vw) { vw = t->w; vh = t->h; }
    }
    if (n > 0) glDrawBuffers((GLsizei)n, bufs);
    else {
        /* Depth-only: no colour is written, and an FBO with no draw buffer
         * has to say so explicitly or it is incomplete. */
        GLenum none = GL_NONE;
        glDrawBuffers(1, &none);
    }

    if (depth >= 0) {
        target3d_t *d = target_by_handle(depth);
        if (!d) return 0;
        GLenum att = (d->fmt == WCL_FMT_DEPTH24_STENCIL8)
                   ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
        attach(bind_fbo, att, d, depth_layer);
        if (!vw) { vw = d->w; vh = d->h; }
    }

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        WC_LOG("love.graphics.setCanvas: the framebuffer is incomplete. The "
               "targets must agree on size, and the driver must support "
               "rendering to every format in the set.");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        bound_color_count = 0;
        wcl_r2d__screen_viewport();
        return 0;
    }

    /* The 2D path derives clip space from ndc_scale, not from the viewport,
     * so setting the viewport alone leaves every 2D draw into this target
     * scaled for the SCREEN. A rectangle then lands at the wrong size and
     * the wrong place -- which reads as a layout bug in the cart. */
    wcl_r2d__set_clip_size(vw, vh);
    glViewport(0, 0, vw, vh);
    bound_color_count = n;
    wcl_r2d__rebind_2d_state();
    return 1;
}

int wcl_r3d_target_bound(void) { return bound_color_count; }

int wcl_r3d_target_send(int shader_handle, const char *name, int target, int unit) {
    (void)unit;   /* the unit is ASSIGNED here, not chosen by the caller */
    target3d_t *t = target_by_handle(target);
    if (!t) return 0;
    GLuint prog = wcl_r2d__program_of(shader_handle);
    if (!prog) return 0;
    GLint loc = glGetUniformLocation(prog, name);
    if (loc < 0) return 0;

    const GLenum tt = tex_target_of(t);
    /* The SHARED allocator, not a private one. Both this and the 2D image
     * path hand out units 1..15; two independent counters would each start
     * at 1, and a shader sampling both an Image and a render target would
     * have the second binding quietly replace the first. */
    int u = wcl_r2d__claim_texture_unit(shader_handle, loc, t->tex, tt);
    if (!u) {
        WC_LOG("Shader:send: out of texture units (15 sampler uniforms max "
               "per shader)");
        return 0;
    }

    wcl_r2d__flush_for_3d();
    glUseProgram(prog);
    glUniform1i(loc, u);
    glActiveTexture((GLenum)(GL_TEXTURE0 + u));
    glBindTexture(tt, t->tex);
    glActiveTexture(GL_TEXTURE0);
    wcl_r2d__invalidate_texture_binding();
    wcl_r2d__restore_program();
    return 1;
}

/* ── cube / array / volume images ─────────────────────────────────────
 *
 * The same texture objects as render targets, filled from cart assets
 * instead of drawn into, so they share the table and the sampler-binding
 * path. Always RGBA8: these come from decoded PNGs.
 */
int wcl_r3d_image_new(int w, int h, wcl_textype_t type, int layers, int mipmaps) {
    /* Deliberately routed through target_new: an image and a target differ
     * only in who writes the pixels, and giving them separate storage would
     * mean two of everything -- two tables, two frees, two sampler paths. */
    return wcl_r3d_target_new(w, h, WCL_FMT_RGBA8, type, layers, mipmaps, 0);
}

int wcl_r3d_image_upload(int handle, int layer, const void *rgba, int w, int h) {
    target3d_t *t = target_by_handle(handle);
    if (!t || !rgba) return 0;
    if (w != t->w || h != t->h) {
        char b[160];
        int n = snprintf(b, sizeof b,
                         "love.graphics.newCubeImage/newArrayImage: every face "
                         "must be %dx%d; got %dx%d", t->w, t->h, w, h);
        if (n > 0) wc_log(b, (unsigned)n);
        return 0;
    }
    if (layer < 0 || layer >= t->layers) return 0;

    wcl_r2d__flush_for_3d();
    const GLenum tt = tex_target_of(t);
    glBindTexture(tt, t->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (t->type == WCL_TEX_CUBE) {
        glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + layer, 0, 0, 0, w, h,
                        GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    } else if (t->type == WCL_TEX_ARRAY || t->type == WCL_TEX_VOLUME) {
        glTexSubImage3D(tt, 0, 0, 0, layer, w, h, 1,
                        GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                        GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }
    wcl_r2d__invalidate_texture_binding();
    return 1;
}

void wcl_r3d_image_finish(int handle) {
    target3d_t *t = target_by_handle(handle);
    if (!t) return;
    if (t->mipmaps) wcl_r3d_target_generate_mipmaps(handle);
}

void wcl_r3d_image_wrap(int handle, int repeat_s, int repeat_t, int repeat_r) {
    target3d_t *t = target_by_handle(handle);
    if (!t) return;
    wcl_r2d__flush_for_3d();
    const GLenum tt = tex_target_of(t);
    glBindTexture(tt, t->tex);
    glTexParameteri(tt, GL_TEXTURE_WRAP_S, repeat_s ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(tt, GL_TEXTURE_WRAP_T, repeat_t ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    if (t->type == WCL_TEX_ARRAY || t->type == WCL_TEX_VOLUME)
        glTexParameteri(tt, GL_TEXTURE_WRAP_R, repeat_r ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    wcl_r2d__invalidate_texture_binding();
}

void wcl_r3d_image_filter(int handle, int linear) {
    target3d_t *t = target_by_handle(handle);
    if (!t) return;
    wcl_r2d__flush_for_3d();
    const GLenum tt = tex_target_of(t);
    glBindTexture(tt, t->tex);
    GLenum minf = linear
        ? (t->mipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR)
        : (t->mipmaps ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST);
    glTexParameteri(tt, GL_TEXTURE_MIN_FILTER, (GLint)minf);
    glTexParameteri(tt, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    wcl_r2d__invalidate_texture_binding();
}

/* ── colour mask ──────────────────────────────────────────────────── */

static int mask_r = 1, mask_g = 1, mask_b = 1, mask_a = 1;

void wcl_r3d_color_mask(int r, int g, int b, int a) {
    if (r == mask_r && g == mask_g && b == mask_b && a == mask_a) return;
    wcl_r2d__flush_for_3d();
    mask_r = r; mask_g = g; mask_b = b; mask_a = a;
    glColorMask((GLboolean)!!r, (GLboolean)!!g, (GLboolean)!!b, (GLboolean)!!a);
}

void wcl_r3d_get_color_mask(int *r, int *g, int *b, int *a) {
    if (r) *r = mask_r; if (g) *g = mask_g;
    if (b) *b = mask_b; if (a) *a = mask_a;
}

/* ── driver limits ────────────────────────────────────────────────── */

int wcl_r3d_limit(int which) {
    if (!wcl_r2d_active()) return 0;
    GLenum pname;
    switch (which) {
        case 0: pname = GL_MAX_COLOR_ATTACHMENTS; break;
        case 1: pname = GL_MAX_TEXTURE_SIZE; break;
        case 2: pname = GL_MAX_CUBE_MAP_TEXTURE_SIZE; break;
        case 3: pname = GL_MAX_3D_TEXTURE_SIZE; break;
        case 4: pname = GL_MAX_ARRAY_TEXTURE_LAYERS; break;
        case 5: pname = GL_MAX_SAMPLES; break;
        default: return 0;
    }
    GLint v = 0;
    glGetIntegerv(pname, &v);
    return (int)v;
}

/* ── depth and cull state ─────────────────────────────────────────── */

static void apply_depth(void) {
    int want = depth_compare != 0;
    if (want != depth_test_on) {
        if (want) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        depth_test_on = want;
    }
    if (want) glDepthFunc((GLenum)depth_compare);
    /* The write mask applies even with the test disabled, which is how
     * LOVE's setDepthMode("always", true) depth-prepass idiom works. */
    glDepthMask((GLboolean)(depth_write ? 1 : 0));
}

static void apply_cull(void) {
    int want = cull_mode != 0;
    if (want != cull_on) {
        if (want) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
        cull_on = want;
    }
    if (want) glCullFace(cull_mode == 1 ? GL_BACK : GL_FRONT);
    glFrontFace(front_face_cw ? GL_CW : GL_CCW);
}

void wcl_r3d_depth_mode(uint32_t compare, int write) {
    /* Depth state is pipeline state, so pending 2D vertices queued under the
     * old mode have to be drawn under the old mode. */
    wcl_r2d__flush_for_3d();
    depth_compare = compare;
    depth_write = write;
    /* Not applied here: the 2D batcher runs with depth off, and applying now
     * would depth-test every subsequent sprite. wcl_r3d_mesh_draw applies it,
     * and wcl_r3d_leave takes it back off. */
}

void wcl_r3d_get_depth_mode(uint32_t *compare, int *write) {
    if (compare) *compare = depth_compare;
    if (write) *write = depth_write;
}

void wcl_r3d_cull_mode(int mode) {
    wcl_r2d__flush_for_3d();
    cull_mode = mode;
}
int wcl_r3d_get_cull_mode(void) { return cull_mode; }

void wcl_r3d_front_face(int cw) {
    wcl_r2d__flush_for_3d();
    front_face_cw = cw;
}
int wcl_r3d_get_front_face(void) { return front_face_cw; }

void wcl_r3d_clear_depth(void) {
    if (!wcl_r2d_active()) return;
    /* glClear honours the write mask, so a cart left in depth_write=false
     * would clear nothing and every frame after the first would test against
     * stale depths. Force the mask on for the clear, then restore. */
    glDepthMask(1);
    glClear(GL_DEPTH_BUFFER_BIT);
    if (!depth_write) glDepthMask(0);
}

void wcl_r3d_frame_begin(void) {
    /* Clear last frame's depths BEFORE anything draws. Doing this lazily at
     * the first 3D draw of a frame would be wrong for a cart that renders 3D
     * into a canvas: the clear has to happen against whatever target is
     * current when the frame starts. */
    if (used_this_frame || depth_compare) wcl_r3d_clear_depth();
    used_this_frame = 0;
}

int wcl_r3d_used_this_frame(void) { return used_this_frame; }

void wcl_r3d_leave(void) {
    if (depth_test_on) { glDisable(GL_DEPTH_TEST); depth_test_on = 0; }
    if (cull_on) { glDisable(GL_CULL_FACE); cull_on = 0; }
    /* The 2D path never writes depth, but it inherits the mask, and a mask
     * left off would break the next frame's depth clear. */
    glDepthMask(1);
    wcl_r2d__rebind_2d_state();
}

void wcl_r3d_reset(void) {
    for (int i = 0; i < MAX_MESHES3D; i++)
        if (meshes3d[i].used) wcl_r3d_mesh_free(i);
    depth_compare = 0;
    depth_write = 0;
    cull_mode = 0;
    front_face_cw = 0;
    used_this_frame = 0;
    if (depth_test_on) { glDisable(GL_DEPTH_TEST); depth_test_on = 0; }
    if (cull_on) { glDisable(GL_CULL_FACE); cull_on = 0; }
    glDepthMask(1);
}

/* ── the draw ─────────────────────────────────────────────────────── */

/* `instances` < 1 means an ordinary draw; >= 1 selects the instanced entry
 * points. The two share everything except that one call, so they are one
 * function rather than a copy that drifts. */
static int mesh_draw_common(int handle, uint32_t tint, int alpha, int instances) {
    mesh3d_t *m = mesh_by_handle(handle);
    if (!m || !wcl_r2d_active()) return 0;

    /* A 3D mesh has no transform of its own: the cart's vertex shader turns
     * model space into clip space from its own matrix uniforms. Without a
     * cart shader bound, the default 2D program would run -- it declares
     * `in vec2 a_pos` and would read the first two floats of a 3D vertex as
     * clip coordinates, drawing a garbled 2D smear rather than failing. So
     * this is a refusal, not a fallback. */
    GLuint prog = wcl_r2d__active_program();
    if (!prog) return 0;

    wcl_r2d__flush_for_3d();

    glBindVertexArray(vao3d);
    bind_mesh_attribs(m);

    apply_depth();
    apply_cull();

    /* Blending stays on: a 3D cart may draw translucent geometry, and the
     * depth test is what resolves occlusion for the opaque case. */
    if (m->tex) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m->tex);
        wcl_r2d__invalidate_texture_binding();
    }

    /* The three engine-supplied uniforms, looked up once per program rather
     * than three times per draw. glGetUniformLocation is a string lookup and
     * a synchronous driver round trip on some implementations; at 60 fps
     * with a few dozen models that adds up for values that never move. */
    const uniform_cache_t *u = uniforms_for(prog);

    /* The current love.graphics colour, handed to the shader the same way
     * the 2D path modulates with it. A shader that ignores it is free to. */
    if (u->love_color >= 0) {
        glUniform4f(u->love_color,
                    (float)((tint >> 16) & 255) / 255.0f,
                    (float)((tint >> 8) & 255) / 255.0f,
                    (float)(tint & 255) / 255.0f,
                    (float)alpha / 255.0f);
    }
    if (u->textured >= 0) glUniform1i(u->textured, m->tex ? 1 : 0);
    if (u->tex >= 0) glUniform1i(u->tex, 0);

    if (instances >= 1) {
        /* gl_InstanceID is what the cart's vertex shader reads to place each
         * copy. There is no per-instance attribute buffer here: a cart that
         * wants per-instance data passes it as a uniform array indexed by
         * gl_InstanceID, which is what LOVE's drawInstanced gives you too. */
        if (m->index_count)
            glDrawElementsInstanced(GL_TRIANGLES, (GLsizei)m->index_count,
                                    GL_UNSIGNED_INT, (const void *)0,
                                    (GLsizei)instances);
        else
            glDrawArraysInstanced(GL_TRIANGLES, 0, (GLsizei)m->count,
                                  (GLsizei)instances);
    } else if (m->index_count) {
        glDrawElements(GL_TRIANGLES, (GLsizei)m->index_count,
                       GL_UNSIGNED_INT, (const void *)0);
    } else {
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)m->count);
    }

#ifdef WCL_R3D_TRACE
    {
        char buf[320];
        GLenum e = glGetError();
        /* Read back what the program ACTUALLY holds for the transform, so a
         * blank frame can be attributed to the matrix rather than guessed
         * at. A zero row here is the whole story. */
        int n = snprintf(buf, sizeof buf,
                         "r3d draw: prog=%u count=%d tex=%u depth=%u/%d cull=%d err=0x%x",
                         (unsigned)prog, m->count,
                         (unsigned)m->tex, (unsigned)depth_compare, depth_write,
                         cull_mode, (unsigned)e);
        if (n > 0) wc_log(buf, (unsigned)n);
    }
#endif

    used_this_frame = 1;
    wcl_r3d_leave();
    return 1;
}

int wcl_r3d_mesh_draw(int handle, uint32_t tint, int alpha) {
    return mesh_draw_common(handle, tint, alpha, 0);
}

int wcl_r3d_mesh_draw_instanced(int handle, int count, uint32_t tint, int alpha) {
    if (count < 1) return 0;
    return mesh_draw_common(handle, tint, alpha, count);
}

#endif /* WCL_ENABLE_GL2D */
