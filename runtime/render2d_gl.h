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

#endif /* WCL_ENABLE_GL2D */
#endif /* WCL_RENDER2D_GL_H */
