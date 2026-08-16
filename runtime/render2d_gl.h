#ifndef WCL_RENDER2D_GL_H
#define WCL_RENDER2D_GL_H

/* Largest filled polygon either layer will handle.
 *
 * Shared so the Lua prelude, the software rasterizer and the GL path all
 * agree. If they disagree the failure is silent and expensive: a polygon
 * the prelude emits but the GL path refuses drops the ENTIRE FRAME -- 3D
 * included -- to the software rasterizer for the rest of the run.
 *
 * 256 covers a full-screen circle at ~1.4 degrees per segment, which is
 * smoother than any 1080p display resolves. */
#define WCL_MAX_POLY_PTS 256

#include <stdint.h>

/* Per-frame GL diagnostics, latched at wcl_r2d_end so a host reading the
 * debug fields mid-frame sees the last completed frame. All zero in the
 * CPU build. Defined in runtime.c so both builds link one copy. */
typedef struct {
    uint32_t draws;         /* glDrawElements/glDrawArrays calls */
    uint32_t solid_flushes;
    uint32_t tex_flushes;
    uint32_t quads;         /* rect/sprite quads submitted */
    uint32_t upload_bytes;  /* glBufferData bytes this frame */
} wcl_r2d_stats_t;
extern wcl_r2d_stats_t wcl_r2d_stats;

/* love.graphics.setBlendMode's modes. Defined for BOTH backends -- the
 * Lua binding maps mode names onto these before it knows which renderer is
 * compiled in, so they cannot live behind the GL guard.
 *
 * A deferred 3D renderer needs these to be real: its post chain composites
 * with "replace" and multiplies AO and bloom into the colour buffer, so
 * collapsing them onto alpha silently destroys the composite while every
 * draw call still reports success. */
#define WCL_BLEND_ALPHA     0
#define WCL_BLEND_ADD       1
#define WCL_BLEND_SUBTRACT  2
#define WCL_BLEND_MULTIPLY  3
#define WCL_BLEND_LIGHTEN   4
#define WCL_BLEND_DARKEN    5
#define WCL_BLEND_SCREEN    6
#define WCL_BLEND_NONE      7   /* "replace" */

#ifdef WCL_ENABLE_GL2D

int  wcl_r2d_init(int width, int height);
/* returns 1 when this frame renders via GL; 0 only when there is no usable
 * GL context at all, which the shipped engine treats as fatal rather than as
 * a reason to rasterize in software. See wcl_r2d_refuse. */
int  wcl_r2d_begin(uint32_t clear_color);
/* fb: the cart framebuffer - blitted to GL when the frame was CPU-rendered.
 * Only engine-cpu.wasm (the differ's oracle) ever renders that way. */
void wcl_r2d_end(const uint32_t *fb);
void wcl_r2d_disable(void);
void wcl_r2d_disable_why(const char *why);
int  wcl_r2d_active(void);

/* Refuse ONE primitive the GL backend cannot express, without touching the
 * frame. wasmcart targets a GPU: a draw the backend will not do is a defect
 * in the cart, and silently finishing the frame on a software rasterizer
 * hides it behind a picture that still looks right while the frame time
 * triples and any bound shader stops being applied.
 *
 * So the primitive is dropped and named, and everything else in the frame
 * renders on the GPU exactly as it would have.
 *
 * Logged once per distinct reason, not once per draw: a cart that trips this
 * trips it every frame, and 60 identical lines a second bury the one that
 * matters. */
void wcl_r2d_refuse(const char *what);
int  wcl_r2d_solid(int x, int y, int w, int h, uint32_t color, int alpha);
int  wcl_r2d_line(int x0, int y0, int x1, int y1, uint32_t color, int alpha);
/* Blit an RGBA image onto an arbitrary destination parallelogram. `pixels`
 * doubles as the atlas cache key, so a cart's image_t payload maps to one
 * atlas entry for its life.
 *
 * cx/cy are the four destination corners in cart pixels, in the order
 * top-left, top-right, bottom-right, bottom-left of the SOURCE rect. Passing
 * corners rather than a rect and an angle means rotation, flips and origin
 * offsets are all just the caller's existing corner math -- draw_image
 * already computes exactly this, so nothing is recomputed or approximated. */
int  wcl_r2d_sprite(const void *pixels, int sw, int sh,
                    const double *cx, const double *cy,
                    int sx, int sy, int srcw, int srch,
                    uint32_t tint, int alpha);

/* Scissor rect in cart pixels; w<0 disables. */
void wcl_r2d_scissor(int x, int y, int w, int h);

/* ── stencil (love.graphics.stencil / setStencilTest) ─────────────────
 *
 * Masking to a NON-RECTANGULAR region, which scissor cannot express: a
 * circular spotlight, a curved health bar, a torn page edge.
 *
 * wcl_r2d_stencil_begin masks off colour writes and points the stencil op
 * at `value`, so whatever the caller draws next lands only in the stencil
 * buffer. wcl_r2d_stencil_end restores colour writes. wcl_r2d_stencil_test
 * then keeps or rejects later fragments by comparing against it.
 *
 * Costs nothing when unused: the stencil renderbuffer is allocated lazily
 * on a canvas's FIRST stencil call, GL_STENCIL_TEST is only enabled while
 * a test is live, and no branch was added to the batched draw path.
 *
 * begin() returns 0 when no stencil buffer is obtainable, so the caller
 * can refuse loudly instead of drawing an unmasked frame.
 *
 * action:  0 replace, 1 increment, 2 decrement, 3 invert,
 *          4 incrementwrap, 5 decrementwrap
 * compare: 0 off, 1 equal, 2 notequal, 3 less, 4 lequal, 5 greater,
 *          6 gequal */
int  wcl_r2d_stencil_begin(int action, int value);
void wcl_r2d_stencil_end(void);
void wcl_r2d_stencil_test(int compare, int value);

/* Fill a CONVEX polygon as a triangle fan. Returns 0 for a concave one --
 * a fan would cover area outside it -- so the caller falls back. */
int  wcl_r2d_poly(const double *xs, const double *ys, int n,
                  uint32_t color, int alpha);

/* Filled circle as a triangle fan. Returns 0 for small radii, where the
 * shape is mostly boundary and the edge-coverage difference stops being a
 * rounding detail. */
int  wcl_r2d_circle(int cx, int cy, int r, uint32_t color, int alpha);

/* Batches are flushed on a change because blending is pipeline state, not
 * per-vertex. `premultiplied` is LOVE's alphamode: 1 when the source colour
 * already carries its alpha. */
void wcl_r2d_blend_mode(int mode, int premultiplied);

/* Additive on/off. Retained for callers that only ever needed the toggle. */
void wcl_r2d_blend_add(int on);

/* Render targets. A canvas is both a destination and, later, a source, so it
 * lives in its own texture with an FBO attached rather than in the shared
 * atlas. `key` is the canvas's RGBA payload pointer, the same identity
 * wcl_r2d_sprite uses, so drawing a canvas afterwards finds this texture.
 *
 * wcl_r2d_target(NULL, 0, 0) restores the screen. Returns 0 if the target
 * could not be set up, in which case the caller must use the software path. */
int  wcl_r2d_target(const void *key, int w, int h);

/* THE HOST'S VIEWPORT RECT. A host that letterboxes -- a phone whose screen
 * is not the cart's aspect -- calls this once, and the engine restores this
 * rect whenever it returns to the screen instead of resetting to the cart's
 * own size. Without it the first setCanvas() of a frame silently destroys
 * the letterbox: on a 2244x1008 device running a 1920x1080 cart that cut a
 * whole row off a match-three board and broke touch mapping with it.
 * Zero width means "not set". */
void wcl_r2d_set_host_viewport(int x, int y, int w, int h);
/* Clear the current target (screen or canvas) to an RGBA colour. */
void wcl_r2d_clear(uint32_t color, int alpha);
/* Forget a canvas's GPU texture, e.g. when the image slot is reused. */
void wcl_r2d_forget(const void *key);

/* ── custom shaders (love.graphics.newShader / setShader) ─────────────
 *
 * A shader is a whole second PROGRAM, not a patch to the default one. It is
 * built from the same vertex layout and keeps the same u_textured modes, so
 * every existing draw path (solid, sprite, glyph, circle) works unchanged
 * while a custom shader is bound.
 *
 * wcl_r2d_shader_new returns a handle >= 0, or -1 on compile/link failure
 * (the GL info log is already sent to wc_log by then). `pixel_src` is the
 * LOVE-shaped body containing effect(); `vertex_src` may be NULL.
 *
 * `is_3d` selects the 3D prologue pair: a vec3 VertexPosition, a
 * VertexNormal attribute, and no glyph/circle dispatch in the fragment
 * stage. A 3D shader MUST supply a vertex stage, since that is where the
 * transform lives; the call is refused otherwise. See render3d_gl.h. */
int  wcl_r2d_shader_new(const char *pixel_src, const char *vertex_src, int is_3d,
                        int mrt_outputs);
/* Bind a shader handle, or -1 for the engine's default program. Flushes any
 * open batch first: the program is pipeline state, not per-vertex. */
void wcl_r2d_shader_use(int handle);
/* Uniform setters. `n` is the component count 1..4. Return 0 when the
 * uniform is not present in the program, so the caller can say so. */
int  wcl_r2d_shader_send_float(int handle, const char *name, const float *v, int n);
int  wcl_r2d_shader_send_int(int handle, const char *name, const int *v, int n);
/* Bind an image's RGBA payload (the atlas cache key) to a sampler uniform.
 * Returns 0 if the uniform is missing or the image has no texture yet. */
int  wcl_r2d_shader_send_image(int handle, const char *name,
                               const void *pixels, int w, int h);
/* Read-only: is `name` a live uniform in the linked program? Read-only
 * matters -- probing by writing a value would clobber the uniform. */
int  wcl_r2d_shader_has_uniform(int handle, const char *name);
/* Is a custom shader currently bound? Used to keep draws that GL cannot
 * express from silently rendering unshaded on the CPU path. */
int  wcl_r2d_shader_active(void);

/* ── meshes (love.graphics.newMesh) ───────────────────────────────────
 *
 * A mesh is ARBITRARY triangles, so it cannot ride the batcher: that path is
 * hardwired to quads (it draws (count/4)*6 indices from a static quad index
 * buffer). A mesh draw therefore flushes and issues its own glDrawArrays,
 * the same shape wcl_r2d_poly already uses -- but textured, and with no
 * 64-triangle cap.
 *
 * The vertex a cart supplies is LOVE's default format, which is already this
 * engine's vertex_t minus `rad`:
 *     VertexPosition vec2   -> x, y
 *     VertexTexCoord vec2   -> u, v
 *     VertexColor    vec4   -> r, g, b, a
 * so there is no repacking, only a coordinate-space conversion (cart pixels
 * to clip space) and, for a textured mesh, the atlas remap below.
 *
 * `verts` is 8 floats per vertex in that order, x/y already world-transformed
 * by Lua and u/v in LOVE's 0..1 texture space. `count` is the number of
 * vertices, which must be a multiple of 3 (the caller expands fan/strip into
 * triangles, so this entry point only ever sees a triangle list).
 *
 * `pixels` is the texture's RGBA payload (the same atlas cache key sprites
 * use) or NULL for an untextured mesh. THE UV REMAP IS THE WHOLE TRAP HERE:
 * sprites live in a shared 2048^2 atlas, so a cart's 0..1 uv addresses the
 * whole atlas rather than its own image. This function maps 0..1 onto the
 * image's atlas sub-rect. A Canvas is its own texture and needs no remap,
 * only the V flip an FBO's bottom-left origin requires.
 *
 * Returns 0 when the mesh could not be drawn (no GL, no atlas room, the
 * texture is the current render target), so the caller can report it. */
int  wcl_r2d_mesh(const float *verts, int count,
                  const void *pixels, int tw, int th,
                  uint32_t tint, int alpha);

/* Draw one baked TTF glyph. `atlas` is stb_truetype's 8-bit coverage bitmap
 * and doubles as the cache key, so a font uploads once and every glyph after
 * that is a quad in the same batch. Coverage modulates alpha, exactly as the
 * software path's blend_px(cov * a / 255) does. */
int  wcl_r2d_glyph(const unsigned char *atlas, int aw, int ah,
                   int dx, int dy, int dw, int dh,
                   int sx, int sy, int srcw, int srch,
                   uint32_t color, int alpha);

#else

static inline int wcl_r2d_init(int width, int height) {
    (void)width; (void)height; return 0;
}
static inline int wcl_r2d_begin(uint32_t clear_color) { (void)clear_color; return 0; }
static inline void wcl_r2d_end(const uint32_t *fb) { (void)fb; }
static inline void wcl_r2d_disable(void) {}
static inline void wcl_r2d_disable_why(const char *why) { (void)why; }
static inline int wcl_r2d_active(void) { return 0; }
/* In the CPU comparator there is no GPU path to keep, so a refusal is a
 * no-op: the scanline rasterizer below draws the primitive itself. That
 * asymmetry is the point -- engine-cpu.wasm is the oracle the GL build is
 * diffed against, and it has to be able to draw what GL refuses. */
static inline void wcl_r2d_refuse(const char *what) { (void)what; }
static inline int wcl_r2d_solid(int x, int y, int w, int h, uint32_t color, int alpha) {
    (void)x; (void)y; (void)w; (void)h; (void)color; (void)alpha; return 0;
}
static inline int wcl_r2d_line(int x0, int y0, int x1, int y1, uint32_t color, int alpha) {
    (void)x0; (void)y0; (void)x1; (void)y1; (void)color; (void)alpha; return 0;
}
static inline int wcl_r2d_sprite(const void *pixels, int sw, int sh,
                                 const double *cx, const double *cy,
                                 int sx, int sy, int srcw, int srch,
                                 uint32_t tint, int alpha) {
    (void)pixels; (void)sw; (void)sh; (void)cx; (void)cy;
    (void)sx; (void)sy; (void)srcw; (void)srch; (void)tint; (void)alpha; return 0;
}
static inline void wcl_r2d_scissor(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
}
static inline int wcl_r2d_stencil_begin(int action, int value) {
    (void)action; (void)value; return 0;
}
static inline void wcl_r2d_stencil_end(void) {}
static inline void wcl_r2d_stencil_test(int compare, int value) {
    (void)compare; (void)value;
}
static inline void wcl_r2d_blend_add(int on) { (void)on; }
static inline void wcl_r2d_blend_mode(int mode, int premultiplied) {
    (void)mode; (void)premultiplied;
}
static inline void wcl_r2d_set_host_viewport(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
}
static inline int wcl_r2d_circle(int cx, int cy, int r, uint32_t color, int alpha) {
    (void)cx; (void)cy; (void)r; (void)color; (void)alpha; return 0;
}
static inline int wcl_r2d_poly(const double *xs, const double *ys, int n,
                               uint32_t color, int alpha) {
    (void)xs; (void)ys; (void)n; (void)color; (void)alpha; return 0;
}
static inline int wcl_r2d_target(const void *key, int w, int h) {
    (void)key; (void)w; (void)h; return 0;
}
static inline void wcl_r2d_clear(uint32_t color, int alpha) { (void)color; (void)alpha; }
static inline void wcl_r2d_forget(const void *key) { (void)key; }
static inline int wcl_r2d_glyph(const unsigned char *atlas, int aw, int ah,
                                int dx, int dy, int dw, int dh,
                                int sx, int sy, int srcw, int srch,
                                uint32_t color, int alpha) {
    (void)atlas; (void)aw; (void)ah; (void)dx; (void)dy; (void)dw; (void)dh;
    (void)sx; (void)sy; (void)srcw; (void)srch; (void)color; (void)alpha; return 0;
}
static inline int wcl_r2d_shader_new(const char *p, const char *v, int is_3d, int mrt) {
    (void)p; (void)v; (void)is_3d; (void)mrt; return -1;
}
static inline void wcl_r2d_shader_use(int handle) { (void)handle; }
static inline int wcl_r2d_shader_send_float(int h, const char *n, const float *v, int c) {
    (void)h; (void)n; (void)v; (void)c; return 0;
}
static inline int wcl_r2d_shader_send_int(int h, const char *n, const int *v, int c) {
    (void)h; (void)n; (void)v; (void)c; return 0;
}
static inline int wcl_r2d_shader_send_image(int h, const char *n, const void *p, int w, int ht) {
    (void)h; (void)n; (void)p; (void)w; (void)ht; return 0;
}
static inline int wcl_r2d_shader_has_uniform(int h, const char *n) {
    (void)h; (void)n; return 0;
}
static inline int wcl_r2d_shader_active(void) { return 0; }
static inline int wcl_r2d_mesh(const float *v, int c, const void *p, int tw, int th,
                               uint32_t tint, int a) {
    (void)v; (void)c; (void)p; (void)tw; (void)th; (void)tint; (void)a; return 0;
}

#endif /* WCL_ENABLE_GL2D */
#endif /* WCL_RENDER2D_GL_H */
