#ifndef WCL_RENDER3D_GL_H
#define WCL_RENDER3D_GL_H

#include <stdint.h>

/* ── 3D meshes (love.graphics.newMesh with a custom vertex format) ─────
 *
 * This is a SECOND pipeline beside the 2D one in render2d_gl.c, not an
 * extension of it. The two differ in every way that matters to a vertex:
 *
 *   2D                                  3D
 *   ---------------------------------   ---------------------------------
 *   Lua transforms to world pixels      the GPU transforms, from a mat4
 *   x,y                                 x,y,z (depth is the whole point)
 *   uv remapped into a shared atlas     whole-texture uv, so it can WRAP
 *   no normals                          normals, for lighting
 *   painter's order, no depth test      depth test + cull
 *
 * Trying to serve both from one vertex_t was the design mistake to avoid:
 * the 2D vertex is 9 floats chosen so a LOVE default-format vertex IS an
 * engine vertex with no repacking, and widening it to carry z and normals
 * would cost every 2D sprite in the cart 12 extra bytes per vertex forever.
 * So: separate struct, separate VAO, separate buffer, separate program.
 *
 * WHY A SEPARATE VAO. render2d_gl.c pins attribute indices 0..3 with
 * glBindAttribLocation so its one shared VAO stays valid across the default
 * program and every cart shader. A 3D program needs a different attribute
 * layout at those same indices. A VAO records attribute state per index, so
 * the two layouts cannot share one VAO -- binding a 3D program with the 2D
 * VAO bound would read positions as UVs. Each pipeline owns its VAO and
 * binds it before drawing.
 *
 * TEXTURES ARE NOT ATLASED. A 2D sprite's uv is remapped into the shared
 * 4096^2 atlas. A 3D model's uv routinely goes outside 0..1 and relies on
 * GL_REPEAT, which an atlas cannot express -- wrapping would sample a
 * neighbouring model's texels. A 3D texture therefore gets its own GL
 * texture object with GL_REPEAT and its own mip chain.
 */

#ifdef WCL_ENABLE_GL2D

/* Vertex layout, matching LOVE's 3D convention and g3d's vertexFormat:
 *     {"VertexPosition", "float", 3}
 *     {"VertexTexCoord", "float", 2}
 *     {"VertexNormal",   "float", 3}
 *     {"VertexColor",    "byte",  4}
 * Colour is stored as 4 floats here rather than 4 bytes: the cart hands us
 * Lua numbers either way, so packing to bytes would cost a conversion to
 * save 12 bytes on geometry that is uploaded once and drawn for the life of
 * the cart. */
typedef struct {
    float x, y, z;
    float u, v;
    float nx, ny, nz;
    float r, g, b, a;
} wcl_vertex3d_t;

/* Create a static-geometry mesh. `verts` is `count` wcl_vertex3d_t uploaded
 * once to a VBO; a redraw is then a bind plus a draw with no per-frame
 * upload, which is what makes a 20k-triangle model cost the same as a quad.
 * Returns a handle >= 0, or -1 (no GL context, or out of slots). */
/* ── generic vertex formats ───────────────────────────────────────────
 *
 * The fixed wcl_vertex3d_t layout above covers the common case and is what
 * g3d uses. It is not enough for a real renderer: 3DreamEngine declares
 * VertexTangent for normal mapping and VertexMaterial for its PBR terms,
 * and a fixed layout has nowhere to put them.
 *
 * So a mesh may instead declare its OWN format -- a list of named
 * attributes, each with a type and a component count, exactly as LOVE's
 * newMesh(vertexformat, ...) does. The layout then drives three things that
 * must agree or the geometry reads as noise:
 *
 *   1. the VBO's stride and per-attribute offsets,
 *   2. the attribute INDEX each name binds to, and
 *   3. the same index bound into every shader program by name at link time.
 *
 * (3) is the part that cannot be skipped. A GL linker assigns attribute
 * locations however it likes, so without an explicit binding a shader's
 * `VertexTangent` could land on the index the VBO fills with positions --
 * which renders, wrongly, with no error anywhere.
 *
 * Indices are assigned in DECLARATION ORDER, so the same format always
 * produces the same bindings and one VAO can be reused across meshes that
 * share a format. */
#define WCL_MAX_ATTRIBS 8
#define WCL_ATTRIB_NAME_MAX 32

typedef struct {
    char name[WCL_ATTRIB_NAME_MAX];
    int components;     /* 1..4 */
    int is_byte;        /* 1 = unsigned byte, normalized to 0..1 in the shader */
} wcl_attrib_t;

/* Create a mesh with a declared format. `verts` is raw interleaved vertex
 * data already packed to the format's stride -- the caller builds it, since
 * only it knows the source values. Returns a handle >= 0 or -1. */
int  wcl_r3d_mesh_new_format(const wcl_attrib_t *attribs, int n_attribs,
                             const void *verts, int count, int stride);
/* The byte stride a format implies, so a caller can size its buffer. */
int  wcl_r3d_format_stride(const wcl_attrib_t *attribs, int n_attribs);
/* Bind every attribute name a program might declare, in the order the
 * format lists them. Called by render2d_gl.c before linking a 3D shader. */
void wcl_r3d_bind_format_attribs(unsigned int program);

/* Replace a declared-format mesh's raw interleaved bytes. */
int  wcl_r3d_mesh_set_bytes(int handle, const void *data, int len);

int  wcl_r3d_mesh_new(const wcl_vertex3d_t *verts, int count);
/* Replace a mesh's geometry in place; count must not exceed the original. */
int  wcl_r3d_mesh_update(int handle, const wcl_vertex3d_t *verts, int count);
/* Index buffer, so a cube is 8 vertices and 36 indices rather than 36
 * vertices. NULL/0 clears it and the mesh draws as a plain array. */
int  wcl_r3d_mesh_set_indices(int handle, const uint32_t *idx, int n);
/* Bind an RGBA image as this mesh's texture. `pixels` is the same payload
 * pointer the 2D path uses as its cache key, so an image uploaded for 3D is
 * found again on a later draw. w/h describe it. NULL detaches. */
int  wcl_r3d_mesh_set_texture(int handle, const void *pixels, int w, int h);
void wcl_r3d_mesh_free(int handle);

/* Draw with the currently bound shader (love.graphics.setShader). A 3D draw
 * REQUIRES a cart shader: the transform lives in the cart's vertex shader
 * (that is how g3d and every other LOVE 3D library work), so there is no
 * sensible default program to fall back to. Returns 0 if no shader is bound,
 * so the prelude can say exactly that rather than drawing nothing.
 *
 * `tint` is the current love.graphics colour, modulating vertex colour the
 * same way the 2D mesh path does. */
int  wcl_r3d_mesh_draw(int handle, uint32_t tint, int alpha);

/* ── depth and face culling ───────────────────────────────────────────
 *
 * love.graphics.setDepthMode(comparemode, write). `compare` is a GL compare
 * func (GL_LEQUAL and friends); 0 disables the test entirely, which is
 * LOVE's "always" + no-write resting state and the 2D default. */
void wcl_r3d_depth_mode(uint32_t compare, int write);
void wcl_r3d_get_depth_mode(uint32_t *compare, int *write);
/* 0 none, 1 back, 2 front. */
void wcl_r3d_cull_mode(int mode);
int  wcl_r3d_get_cull_mode(void);
/* 0 counter-clockwise (LOVE's default), 1 clockwise. */
void wcl_r3d_front_face(int cw);
int  wcl_r3d_get_front_face(void);

/* Clear the depth buffer. Called at frame start whenever a depth mode is
 * active: without it, frame N+1 tests against frame N's depths and geometry
 * vanishes wherever the previous frame happened to be nearer. */
void wcl_r3d_clear_depth(void);
/* Drop every 3D resource and reset depth/cull state. Called from
 * wcl_r2d_disable so a fallback to the software path does not leave the
 * depth test enabled for 2D draws. */
void wcl_r3d_reset(void);
/* Restore the 2D pipeline's GL state after a 3D draw: the 2D VAO rebound,
 * depth test and culling off. The 2D batcher assumes both are off. */
void wcl_r3d_leave(void);

/* Did any 3D draw happen this frame? The frame loop uses this to decide
 * whether the depth buffer needs clearing next frame. */
int  wcl_r3d_used_this_frame(void);
void wcl_r3d_frame_begin(void);

/* ── GPU render targets ───────────────────────────────────────────────
 *
 * A SECOND kind of canvas, beside the CPU-backed one in runtime.c. The 2D
 * canvas is an RGBA8 buffer in linear memory that the software rasterizer
 * can write and the GL path mirrors into a texture; that design is what
 * makes the fallback whole-frame and exact, and it is kept.
 *
 * It cannot express what a deferred renderer needs. `rgba16f` has no 8-bit
 * CPU form, a depth texture has no colour at all, and a cubemap is six
 * faces rather than one buffer. So these targets are GPU-ONLY: no CPU
 * mirror, no software path, and a cart that asks for one on a host with no
 * GL is told so rather than handed something that silently differs.
 *
 * Formats. Not an open set -- each is a (internalformat, format, type)
 * triple the backend knows how to allocate, and an unknown name is refused
 * by name rather than guessed at. */
typedef enum {
    WCL_FMT_RGBA8 = 0,
    WCL_FMT_R8,
    WCL_FMT_RG8,
    WCL_FMT_R16F,
    WCL_FMT_RG16F,
    WCL_FMT_RGBA16F,
    WCL_FMT_R32F,
    WCL_FMT_RGBA32F,
    WCL_FMT_DEPTH16,
    WCL_FMT_DEPTH24,
    WCL_FMT_DEPTH32F,
    WCL_FMT_DEPTH24_STENCIL8,
    WCL_FMT__COUNT
} wcl_format_t;

/* Texture shape. `2d` is the ordinary case; the others exist because a
 * renderer needs to BOTH sample them as one unit and render into a single
 * face/layer of them. */
typedef enum {
    WCL_TEX_2D = 0,
    WCL_TEX_CUBE,      /* 6 faces, sampled as samplerCube */
    WCL_TEX_ARRAY,     /* N independent layers, sampler2DArray */
    WCL_TEX_VOLUME     /* N filtered slices, sampler3D */
} wcl_textype_t;

/* Create a GPU target. `layers` is the face/layer/slice count (ignored for
 * 2D, forced to 6 for cube). `mipmaps` allocates a full mip chain.
 * Returns a handle >= 0, or -1 with a reason logged. */
int  wcl_r3d_target_new(int w, int h, wcl_format_t fmt, wcl_textype_t type,
                        int layers, int mipmaps, int msaa);
void wcl_r3d_target_free(int handle);
int  wcl_r3d_target_size(int handle, int *w, int *h, int *layers);
/* Is this format actually renderable on THIS driver? Float colour buffers
 * need EXT_color_buffer_float on WebGL2, so the answer is a runtime
 * question, not a compile-time one. */
int  wcl_r3d_format_supported(wcl_format_t fmt);
/* Regenerate the mip chain after rendering into level 0. */
void wcl_r3d_target_generate_mipmaps(int handle);

/* Bind up to `n` colour targets plus an optional depth target, which is the
 * multiple-render-target draw a deferred renderer's geometry pass needs.
 *
 * `layer[i]` selects the cube face or array layer of colour target i (0 for
 * a plain 2D target). `depth` may be -1 for none. Passing n == 0 and
 * depth == -1 restores the screen.
 *
 * Returns 0 if the framebuffer could not be completed -- a format the
 * driver will not render to, or more targets than it has attachments --
 * which the prelude reports rather than drawing into nothing. */
int  wcl_r3d_target_bind(const int *handles, const int *layer, int n,
                         int depth, int depth_layer);
/* The currently bound colour target count, so a caller can tell whether it
 * is drawing to the screen. */
int  wcl_r3d_target_bound(void);

/* Bind a target's texture to a sampler uniform in the bound shader. This is
 * how a lighting pass reads the geometry pass's output, and it is separate
 * from the 2D image sampler path because the texture may be a cube, an
 * array, a volume, or a depth texture -- none of which the 2D path has a
 * uniform type for. Returns 0 if the uniform is not live. */
int  wcl_r3d_target_send(int shader_handle, const char *name, int target,
                         int unit);

/* Colour write mask, LOVE's setColorMask. */
void wcl_r3d_color_mask(int r, int g, int b, int a);
void wcl_r3d_get_color_mask(int *r, int *g, int *b, int *a);

/* Instanced draw: the same mesh `count` times, with gl_InstanceID varying.
 * Returns 0 on the same terms as wcl_r3d_mesh_draw. */
int  wcl_r3d_mesh_draw_instanced(int handle, int count, uint32_t tint, int alpha);

/* Driver limits, for love.graphics.getSystemLimits(). */
int  wcl_r3d_limit(int which);   /* 0 multicanvas, 1 texturesize, 2 cubetexturesize,
                                    3 volumetexturesize, 4 texturelayers, 5 msaa */

/* ── cube / array / volume IMAGES ─────────────────────────────────────
 *
 * The read-only twin of a render target: same texture shapes, but filled
 * from cart assets instead of rendered into. love.graphics.newCubeImage and
 * friends.
 *
 * These reuse the target table, so an image and a target are the same kind
 * of handle and both can be sent to a sampler by wcl_r3d_target_send. The
 * only difference is who writes the pixels.
 *
 * Create empty, then upload each face/layer separately: a cart's six cube
 * faces are six PNGs decoded one at a time, and requiring them all in
 * memory at once would double the peak for no reason. */
int  wcl_r3d_image_new(int w, int h, wcl_textype_t type, int layers, int mipmaps);
/* Upload RGBA8 pixels into one face/layer. `layer` is 0-based. */
int  wcl_r3d_image_upload(int handle, int layer, const void *rgba, int w, int h);
/* Finish: generate mips if the image asked for them. */
void wcl_r3d_image_finish(int handle);
/* Wrap + filter, for Image:setWrap / setFilter on these types. */
void wcl_r3d_image_wrap(int handle, int repeat_s, int repeat_t, int repeat_r);
void wcl_r3d_image_filter(int handle, int linear);

#else  /* no GL: 3D is refused up front, exactly as meshes and shaders are */

typedef struct { float x, y, z, u, v, nx, ny, nz, r, g, b, a; } wcl_vertex3d_t;
static inline int wcl_r3d_mesh_new(const wcl_vertex3d_t *v, int c) {
    (void)v; (void)c; return -1;
}
static inline int wcl_r3d_mesh_update(int h, const wcl_vertex3d_t *v, int c) {
    (void)h; (void)v; (void)c; return 0;
}
static inline int wcl_r3d_mesh_set_indices(int h, const uint32_t *i, int n) {
    (void)h; (void)i; (void)n; return 0;
}
static inline int wcl_r3d_mesh_set_texture(int h, const void *p, int w, int ht) {
    (void)h; (void)p; (void)w; (void)ht; return 0;
}
static inline void wcl_r3d_mesh_free(int h) { (void)h; }
static inline int wcl_r3d_mesh_draw(int h, uint32_t t, int a) {
    (void)h; (void)t; (void)a; return 0;
}
static inline void wcl_r3d_depth_mode(uint32_t c, int w) { (void)c; (void)w; }
static inline void wcl_r3d_get_depth_mode(uint32_t *c, int *w) {
    if (c) *c = 0; if (w) *w = 0;
}
static inline void wcl_r3d_cull_mode(int m) { (void)m; }
static inline int  wcl_r3d_get_cull_mode(void) { return 0; }
static inline void wcl_r3d_front_face(int cw) { (void)cw; }
static inline int  wcl_r3d_get_front_face(void) { return 0; }
static inline void wcl_r3d_clear_depth(void) {}
static inline void wcl_r3d_reset(void) {}
static inline void wcl_r3d_leave(void) {}
static inline int  wcl_r3d_used_this_frame(void) { return 0; }
static inline void wcl_r3d_frame_begin(void) {}

typedef enum {
    WCL_FMT_RGBA8 = 0, WCL_FMT_R8, WCL_FMT_RG8, WCL_FMT_R16F, WCL_FMT_RG16F,
    WCL_FMT_RGBA16F, WCL_FMT_R32F, WCL_FMT_RGBA32F, WCL_FMT_DEPTH16,
    WCL_FMT_DEPTH24, WCL_FMT_DEPTH32F, WCL_FMT_DEPTH24_STENCIL8, WCL_FMT__COUNT
} wcl_format_t;
typedef enum {
    WCL_TEX_2D = 0, WCL_TEX_CUBE, WCL_TEX_ARRAY, WCL_TEX_VOLUME
} wcl_textype_t;

static inline int wcl_r3d_target_new(int w, int h, wcl_format_t f, wcl_textype_t t,
                                     int l, int m, int s) {
    (void)w; (void)h; (void)f; (void)t; (void)l; (void)m; (void)s; return -1;
}
static inline void wcl_r3d_target_free(int h) { (void)h; }
static inline int wcl_r3d_target_size(int h, int *w, int *ht, int *l) {
    (void)h; (void)w; (void)ht; (void)l; return 0;
}
static inline int wcl_r3d_format_supported(wcl_format_t f) { (void)f; return 0; }
static inline void wcl_r3d_target_generate_mipmaps(int h) { (void)h; }
static inline int wcl_r3d_target_bind(const int *h, const int *l, int n,
                                      int d, int dl) {
    (void)h; (void)l; (void)n; (void)d; (void)dl; return 0;
}
static inline int wcl_r3d_target_bound(void) { return 0; }
static inline int wcl_r3d_target_send(int s, const char *n, int t, int u) {
    (void)s; (void)n; (void)t; (void)u; return 0;
}
static inline void wcl_r3d_color_mask(int r, int g, int b, int a) {
    (void)r; (void)g; (void)b; (void)a;
}
static inline void wcl_r3d_get_color_mask(int *r, int *g, int *b, int *a) {
    if (r) *r = 1; if (g) *g = 1; if (b) *b = 1; if (a) *a = 1;
}
static inline int wcl_r3d_mesh_draw_instanced(int h, int c, uint32_t t, int a) {
    (void)h; (void)c; (void)t; (void)a; return 0;
}
static inline int wcl_r3d_limit(int which) { (void)which; return 0; }
#define WCL_MAX_ATTRIBS 8
#define WCL_ATTRIB_NAME_MAX 32
typedef struct {
    char name[WCL_ATTRIB_NAME_MAX];
    int components;
    int is_byte;
} wcl_attrib_t;
static inline int wcl_r3d_mesh_new_format(const wcl_attrib_t *a, int n,
                                          const void *v, int c, int s) {
    (void)a; (void)n; (void)v; (void)c; (void)s; return -1;
}
static inline int wcl_r3d_format_stride(const wcl_attrib_t *a, int n) {
    (void)a; (void)n; return 0;
}
static inline void wcl_r3d_bind_format_attribs(unsigned int p) { (void)p; }
static inline int wcl_r3d_mesh_set_bytes(int h, const void *d, int l) {
    (void)h; (void)d; (void)l; return 0;
}

static inline int wcl_r3d_image_new(int w, int h, wcl_textype_t t, int l, int m) {
    (void)w; (void)h; (void)t; (void)l; (void)m; return -1;
}
static inline int wcl_r3d_image_upload(int h, int l, const void *p, int w, int ht) {
    (void)h; (void)l; (void)p; (void)w; (void)ht; return 0;
}
static inline void wcl_r3d_image_finish(int h) { (void)h; }
static inline void wcl_r3d_image_wrap(int h, int s, int t, int r) {
    (void)h; (void)s; (void)t; (void)r;
}
static inline void wcl_r3d_image_filter(int h, int l) { (void)h; (void)l; }

#endif /* WCL_ENABLE_GL2D */
#endif /* WCL_RENDER3D_GL_H */
