#ifndef WCL_RENDER2D_GL_H
#define WCL_RENDER2D_GL_H

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

#ifdef WCL_ENABLE_GL2D

int  wcl_r2d_init(int width, int height);
/* returns 1 when this frame renders via GL; 0 when the caller must clear
 * and CPU-rasterize the framebuffer (sticky cpu_mode, see render2d_gl.c) */
int  wcl_r2d_begin(uint32_t clear_color);
/* fb: the cart framebuffer - blitted to GL when the frame was CPU-rendered */
void wcl_r2d_end(const uint32_t *fb);
void wcl_r2d_disable(void);
int  wcl_r2d_active(void);
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

/* Fill a CONVEX polygon as a triangle fan. Returns 0 for a concave one --
 * a fan would cover area outside it -- so the caller falls back. */
int  wcl_r2d_poly(const double *xs, const double *ys, int n,
                  uint32_t color, int alpha);

/* Filled circle as a triangle fan. Returns 0 for small radii, where the
 * shape is mostly boundary and the edge-coverage difference stops being a
 * rounding detail. */
int  wcl_r2d_circle(int cx, int cy, int r, uint32_t color, int alpha);

/* Additive blending on/off. Batches are flushed on a change because the
 * blend mode is pipeline state, not per-vertex. */
void wcl_r2d_blend_add(int on);

/* Render targets. A canvas is both a destination and, later, a source, so it
 * lives in its own texture with an FBO attached rather than in the shared
 * atlas. `key` is the canvas's RGBA payload pointer, the same identity
 * wcl_r2d_sprite uses, so drawing a canvas afterwards finds this texture.
 *
 * wcl_r2d_target(NULL, 0, 0) restores the screen. Returns 0 if the target
 * could not be set up, in which case the caller must use the software path. */
int  wcl_r2d_target(const void *key, int w, int h);
/* Clear the current target (screen or canvas) to an RGBA colour. */
void wcl_r2d_clear(uint32_t color, int alpha);
/* Forget a canvas's GPU texture, e.g. when the image slot is reused. */
void wcl_r2d_forget(const void *key);

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
static inline int wcl_r2d_active(void) { return 0; }
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
static inline void wcl_r2d_blend_add(int on) { (void)on; }
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

#endif /* WCL_ENABLE_GL2D */
#endif /* WCL_RENDER2D_GL_H */
