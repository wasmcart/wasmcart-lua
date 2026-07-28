/*
 * runtime.c - the wasmcart Lua engine core.
 *
 * Game developers never touch this file. They take the prebuilt engine wasm
 * and write Lua in app/. The engine embeds a full Lua VM (real Lua: closures,
 * coroutines, metatables, GC, require) and runs a game written against a
 * LOVE-style API: love.load / love.update(dt) / love.draw, top-left origin,
 * 1280x720, 60 fps fixed step.
 *
 * NOT LOVE: LOVE is an independent zlib-licensed project. This is an
 * unaffiliated engine with a LOVE-style API surface on the open wasmcart
 * cartridge contract. Source-familiar, not compatibility-claiming.
 *
 * Renderer: software raster (rects/lines/circles/polygons + PNG sprites with
 * quads, rotation, scale, tint, alpha + bitfont and TTF text + canvases).
 * Audio: wc_pcm_mixer (16 voices), WAV + OGG assets, generated beeps.
 * Determinism: fixed dt, host-seeded RNG, one pinned VM -> identical frames
 * on every host.
 */
#ifdef WCL_USE_GL
#define WC_USE_GL   /* must precede wasmcart.h: gates the "gl" import block */
#endif
#include "wasmcart.h"
#include "render2d_gl.h"
#include "wc_cart.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"

#define WC_PCM_MIXER_IMPLEMENTATION
#include "wc_pcm_mixer.h"

/* stb_vorbis lives in its own translation unit (vorbis.c): it #defines a bare
 * macro `L` that collides with every `lua_State *L` parameter in lua.h. */
extern int stb_vorbis_decode_memory(const unsigned char *mem, int len, int *channels,
                                    int *sample_rate, short **output);

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

/* physics.c: Box2D v3 bound as the global `b2` */
void wcl_open_physics(lua_State *L);

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* The Lua-side API surface, embedded at build time (prelude.inc is generated
 * from prelude.lua by build.sh). An app/prelude.lua asset overrides it. */
#include "prelude.inc"

#define DEFAULT_WIDTH  1280
#define DEFAULT_HEIGHT 720
#define MAX_WIDTH  1280
#define MAX_HEIGHT 720
#define AUDIO_CAP 4096

WC_CART_BUFFERS;

/* ── GL presentation (opt-in: -DWCL_USE_GL) ───────────────────────────
 *
 * wc_gl_blit.h is the spec's standard display path: even a pure-2D cart
 * uploads its finished pixels as a texture and draws one fullscreen quad,
 * so the host always presents through GL.
 *
 * OPT-IN and default OFF on purpose. A cart IS a GL cart iff its wasm
 * imports from the "gl" module, and a host handed a GL cart with no GL
 * context must fail the load rather than stub it. Making this unconditional
 * would break every 2D-only host these carts run on today, for a change
 * that moves no pixels.
 *
 * The software rasterizer stays the reference implementation either way --
 * it produces exactly the same pixels, and this only changes how they reach
 * the screen. That is what keeps the blit/prims goldens valid here.
 */
#ifdef WCL_USE_GL
#define WC_GL_BLIT_IMPLEMENTATION
#include "wc_gl_blit.h"

/* The framebuffer is XRGB8888 (0x00RRGGBB), so in little-endian memory the
 * bytes run B,G,R,X. wc_gl_blit uploads as GL_RGBA (R first), which would
 * swap red and blue. Repack into scratch rather than changing the
 * framebuffer format, which the rasterizer and every golden depend on. */
static uint8_t gl_rgba[MAX_WIDTH * MAX_HEIGHT * 4];

static void gl_present(void) {
    const uint32_t *src = wc_framebuffer;
    uint8_t *dst = gl_rgba;
    for (int i = 0; i < DEFAULT_WIDTH * DEFAULT_HEIGHT; i++) {
        const uint32_t px = src[i];
        dst[0] = (uint8_t)(px >> 16);  /* R */
        dst[1] = (uint8_t)(px >> 8);   /* G */
        dst[2] = (uint8_t)px;          /* B */
        dst[3] = 255;                  /* X is unused: present opaque */
        dst += 4;
    }
    wc_gl_blit(gl_rgba, DEFAULT_WIDTH, DEFAULT_HEIGHT);
}
#endif

WC_DETERMINISTIC_RNG

/* named state the harness reads by name */
static uint32_t dbg_tick;
static int32_t  dbg_score;
static int32_t  dbg_aux;
static uint32_t dbg_lua_ok = 1;
static uint32_t dbg_gc_kb;
static uint32_t dbg_draw_calls;

WC_DEBUG_FIELDS(
    WC_DBG("tick_count", dbg_tick,       WC_DBG_U32),
    WC_DBG("score",      dbg_score,      WC_DBG_I32),
    WC_DBG("aux",        dbg_aux,        WC_DBG_I32),
    WC_DBG("lua_ok",     dbg_lua_ok,     WC_DBG_U32),
    WC_DBG("gc_kb",      dbg_gc_kb,      WC_DBG_U32),
    WC_DBG("draw_calls", dbg_draw_calls, WC_DBG_U32)
)

#define MARK_BOOT 1
#define MARK_LUA_ERROR 9

static lua_State *L;
static uint32_t tick_n;

/* cart "SRAM": hosts persist this (.sav next to the cart) */
#define SAVE_BYTES 4096
static uint8_t wc_save[SAVE_BYTES];

/* ── raster state ──────────────────────────────────────────────────── */

/* Current raster destination. NULL = the cart framebuffer (XRGB u32).
 * Non-NULL = a canvas's RGBA8 buffer (row 0 = top, same as the framebuffer,
 * so canvases sample identically to images). */
static uint8_t *rt_buf = NULL;
static int rt_w, rt_h;

/* graphics state: color + scissor. Transforms live in Lua (prelude keeps the
 * stack and sends world coords down), so C stays a dumb rasterizer. */
static int cur_r = 255, cur_g = 255, cur_b = 255, cur_a = 255;
static int sc_on = 0, sc_x = 0, sc_y = 0, sc_w = 0, sc_h = 0;
static int blend_add = 0;

static inline int dest_w(void) { return rt_buf ? rt_w : DEFAULT_WIDTH; }
static inline int dest_h(void) { return rt_buf ? rt_h : DEFAULT_HEIGHT; }

/* top-left origin throughout (LOVE convention) */
static inline void blend_px(int x, int y, uint32_t rgb, int a) {
    if (a <= 0 || x < 0 || x >= dest_w() || y < 0 || y >= dest_h()) return;
    if (sc_on && (x < sc_x || x >= sc_x + sc_w || y < sc_y || y >= sc_y + sc_h)) return;
    uint32_t sr = (rgb >> 16) & 0xFF, sg = (rgb >> 8) & 0xFF, sb = rgb & 0xFF;
    if (rt_buf) {
        uint8_t *p = &rt_buf[(y * rt_w + x) * 4];
        uint32_t da = p[3];
        uint32_t oa = a + da * (255 - a) / 255;
        if (oa == 0) return;
        if (blend_add) {
            int nr = p[0] + (int)(sr * a / 255); if (nr > 255) nr = 255;
            int ng = p[1] + (int)(sg * a / 255); if (ng > 255) ng = 255;
            int nb = p[2] + (int)(sb * a / 255); if (nb > 255) nb = 255;
            p[0] = (uint8_t)nr; p[1] = (uint8_t)ng; p[2] = (uint8_t)nb;
            p[3] = (uint8_t)oa;
            return;
        }
        p[0] = (uint8_t)((sr * a + p[0] * da * (255 - a) / 255) / oa);
        p[1] = (uint8_t)((sg * a + p[1] * da * (255 - a) / 255) / oa);
        p[2] = (uint8_t)((sb * a + p[2] * da * (255 - a) / 255) / oa);
        p[3] = (uint8_t)oa;
        return;
    }
    uint32_t *p = &wc_framebuffer[y * DEFAULT_WIDTH + x];
    uint32_t d = *p;
    if (blend_add) {
        int nr = (int)((d >> 16) & 0xFF) + (int)(sr * a / 255); if (nr > 255) nr = 255;
        int ng = (int)((d >> 8) & 0xFF) + (int)(sg * a / 255); if (ng > 255) ng = 255;
        int nb = (int)(d & 0xFF) + (int)(sb * a / 255); if (nb > 255) nb = 255;
        *p = ((uint32_t)nr << 16) | ((uint32_t)ng << 8) | (uint32_t)nb;
        return;
    }
    if (a >= 255) { *p = rgb; return; }
    uint32_t r = (sr * a + ((d >> 16) & 0xFF) * (255 - a)) / 255;
    uint32_t g = (sg * a + ((d >> 8) & 0xFF) * (255 - a)) / 255;
    uint32_t b = (sb * a + (d & 0xFF) * (255 - a)) / 255;
    *p = (r << 16) | (g << 8) | b;
}

/* Exact division by 255 for v <= 65535, which is the standard
 * multiply-shift identity. Our alpha blends compute (src*a + dst*(255-a)),
 * whose maximum over the whole 8-bit domain is 255*255 = 65025, so this is
 * exact everywhere it is used here -- verified exhaustively. It diverges
 * from v/255 at v = 65790, so do NOT reuse it on wider sums. */
static inline uint32_t div255(uint32_t v) { return (v * 257u + 257u) >> 16; }

/* Horizontal-span writer.
 *
 * blend_px re-reads five globals (rt_buf, sc_on, blend_add, and the two
 * dest dimensions) and re-tests bounds and scissor for EVERY pixel. None of
 * those change while a primitive is drawing, so for any run of pixels on
 * one row the decisions can be made once. Measured: opaque rect fill (which
 * already had a fast path) was 0.45 ms while the SAME pixel count through
 * blend_px cost 7.88 ms -- a 17x cliff that every other primitive was
 * paying.
 *
 * The arithmetic below is copied verbatim from blend_px, so results are
 * bit-identical; only the redundant per-pixel decisions are lifted out.
 * Callers must pass a run that is already clipped in y; x is clipped here.
 */
static void blend_span(int x0, int x1, int y, uint32_t rgb, int a) {
    if (a <= 0) return;
    const int dw_ = dest_w(), dh_ = dest_h();
    if (y < 0 || y >= dh_) return;

    if (x0 < 0) x0 = 0;
    if (x1 > dw_) x1 = dw_;
    if (sc_on) {
        if (y < sc_y || y >= sc_y + sc_h) return;
        if (x0 < sc_x) x0 = sc_x;
        if (x1 > sc_x + sc_w) x1 = sc_x + sc_w;
    }
    if (x0 >= x1) return;

    const uint32_t sr = (rgb >> 16) & 0xFF, sg = (rgb >> 8) & 0xFF, sb = rgb & 0xFF;

    if (rt_buf) {
        uint8_t *p = &rt_buf[(y * rt_w + x0) * 4];
        if (blend_add) {                       /* branch hoisted out of the loop */
            for (int x = x0; x < x1; x++, p += 4) {
                const uint32_t da = p[3];
                const uint32_t oa = a + da * (255 - a) / 255;
                if (oa == 0) continue;
                int nr = p[0] + (int)(sr * a / 255); if (nr > 255) nr = 255;
                int ng = p[1] + (int)(sg * a / 255); if (ng > 255) ng = 255;
                int nb = p[2] + (int)(sb * a / 255); if (nb > 255) nb = 255;
                p[0] = (uint8_t)nr; p[1] = (uint8_t)ng; p[2] = (uint8_t)nb;
                p[3] = (uint8_t)oa;
            }
            return;
        }
        if (a >= 255) {                        /* fully opaque source: no read */
            for (int x = x0; x < x1; x++, p += 4) {
                p[0] = (uint8_t)sr; p[1] = (uint8_t)sg; p[2] = (uint8_t)sb;
                p[3] = 255;
            }
            return;
        }
        const uint32_t ia = 255 - a;
        for (int x = x0; x < x1; x++, p += 4) {
            const uint32_t da = p[3];
            if (da == 255) {
                /* opaque destination -- the general formula reduces exactly
                 * to the same lerp the framebuffer path uses (oa == 255), so
                 * this skips three divisions without changing the result */
                p[0] = (uint8_t)div255(sr * a + p[0] * ia);
                p[1] = (uint8_t)div255(sg * a + p[1] * ia);
                p[2] = (uint8_t)div255(sb * a + p[2] * ia);
                continue;
            }
            const uint32_t oa = a + da * ia / 255;
            if (oa == 0) continue;
            p[0] = (uint8_t)((sr * a + p[0] * da * ia / 255) / oa);
            p[1] = (uint8_t)((sg * a + p[1] * da * ia / 255) / oa);
            p[2] = (uint8_t)((sb * a + p[2] * da * ia / 255) / oa);
            p[3] = (uint8_t)oa;
        }
        return;
    }

    uint32_t *p = &wc_framebuffer[y * DEFAULT_WIDTH + x0];
    if (blend_add) {
        for (int x = x0; x < x1; x++, p++) {
            const uint32_t d = *p;
            int nr = (int)((d >> 16) & 0xFF) + (int)(sr * a / 255); if (nr > 255) nr = 255;
            int ng = (int)((d >> 8) & 0xFF) + (int)(sg * a / 255); if (ng > 255) ng = 255;
            int nb = (int)(d & 0xFF) + (int)(sb * a / 255); if (nb > 255) nb = 255;
            *p = ((uint32_t)nr << 16) | ((uint32_t)ng << 8) | (uint32_t)nb;
        }
        return;
    }
    if (a >= 255) {
        for (int x = x0; x < x1; x++, p++) *p = rgb;
        return;
    }
    const uint32_t ia = 255 - a;
    const uint32_t sra = sr * a, sga = sg * a, sba = sb * a;
    for (int x = x0; x < x1; x++, p++) {
        const uint32_t d = *p;
        const uint32_t r = div255(sra + ((d >> 16) & 0xFF) * ia);
        const uint32_t g = div255(sga + ((d >> 8) & 0xFF) * ia);
        const uint32_t b = div255(sba + (d & 0xFF) * ia);
        *p = (r << 16) | (g << 8) | b;
    }
}

static inline uint32_t cur_rgb(void) {
    return ((uint32_t)cur_r << 16) | ((uint32_t)cur_g << 8) | (uint32_t)cur_b;
}

static void fill_rect(int x, int y, int w, int h, uint32_t c, int a) {
    if (a <= 0 || w <= 0 || h <= 0) return;
    /* GL only owns the screen; a canvas target, scissor or additive blending
     * is not modelled here, so those keep the software path (and have
     * already forced cpu_mode via wcl_r2d_disable). */
    if (wcl_r2d_solid(x, y, w, h, c, a)) return;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > dest_w() ? dest_w() : x + w;
    int y1 = y + h > dest_h() ? dest_h() : y + h;
    for (int yy = y0; yy < y1; yy++) blend_span(x0, x1, yy, c, a);
}

static void raster_line(int x0, int y0, int x1, int y1, uint32_t c, int a) {
    if (a <= 0) return;
    if (wcl_r2d_line(x0, y0, x1, y1, c, a)) return;
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    /* Bresenham touches one pixel per step, so it cannot use blend_span --
     * but the destination, scissor and blend-mode decisions are still
     * constant for the whole line. Hoist them, then use a plain store for
     * the common opaque-to-framebuffer case. Arithmetic is copied from
     * blend_px, so results are unchanged. */
    const int dw_ = dest_w(), dh_ = dest_h();
    const int simple = (!rt_buf && !sc_on && !blend_add);
    const uint32_t sr = (c >> 16) & 0xFF, sg = (c >> 8) & 0xFF, sb = c & 0xFF;
    const uint32_t ia = 255 - a;
    const uint32_t sra = sr * a, sga = sg * a, sba = sb * a;

    for (;;) {
        if (simple && x0 >= 0 && x0 < dw_ && y0 >= 0 && y0 < dh_) {
            uint32_t *p = &wc_framebuffer[y0 * DEFAULT_WIDTH + x0];
            if (a >= 255) {
                *p = c;
            } else {
                const uint32_t d = *p;
                *p = (div255(sra + ((d >> 16) & 0xFF) * ia) << 16)
                   | (div255(sga + ((d >> 8) & 0xFF) * ia) << 8)
                   |  div255(sba + (d & 0xFF) * ia);
            }
        } else if (!simple) {
            blend_px(x0, y0, c, a);
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void raster_circle(int cx, int cy, int r, uint32_t c, int a, int filled) {
    if (r <= 0) return;
    /* A GL triangle fan does NOT reproduce the software span fill: the
     * midpoint/sqrt spans and polygon coverage disagree on the boundary, and
     * a circle is nearly all boundary at small radii. Circles therefore
     * still drop the frame to the CPU backend, which is correct rather than
     * approximate. Cavern hits this, which is why it stays on software. */
    wcl_r2d_disable();
    if (filled) {
        for (int yy = -r; yy <= r; yy++) {
            int span = (int)(sqrt((double)(r * r - yy * yy)) + 0.5);
            blend_span(cx - span, cx + span + 1, cy + yy, c, a);
        }
        return;
    }
    /* midpoint circle. Each step writes 8 single pixels, so like the line
     * this cannot use blend_span; it uses one-pixel spans instead, which
     * still hoists the destination and scissor decisions per call. */
    int x = r, y = 0, err = 1 - x;
    while (x >= y) {
        blend_span(cx + x, cx + x + 1, cy + y, c, a);
        blend_span(cx + y, cx + y + 1, cy + x, c, a);
        blend_span(cx - y, cx - y + 1, cy + x, c, a);
        blend_span(cx - x, cx - x + 1, cy + y, c, a);
        blend_span(cx - x, cx - x + 1, cy - y, c, a);
        blend_span(cx - y, cx - y + 1, cy - x, c, a);
        blend_span(cx + y, cx + y + 1, cy - x, c, a);
        blend_span(cx + x, cx + x + 1, cy - y, c, a);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

/* convex/concave polygon scanline fill (even-odd) */
#define MAX_POLY_PTS 64
static void raster_polygon(const double *xs, const double *ys, int n,
                           uint32_t c, int a, int filled) {
    if (n < 3) return;
    /* Filled convex polygons go to GL as a triangle fan; outlines are just
     * lines, which are already on the GL path. A concave fill has no correct
     * fan, so it keeps the scanline fill and drops the frame to software. */
    if (filled) {
        if (wcl_r2d_poly(xs, ys, n, c, a)) return;
        wcl_r2d_disable();
    }
    if (!filled) {
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            raster_line((int)xs[i], (int)ys[i], (int)xs[j], (int)ys[j], c, a);
        }
        return;
    }
    double miny = ys[0], maxy = ys[0];
    for (int i = 1; i < n; i++) { if (ys[i] < miny) miny = ys[i]; if (ys[i] > maxy) maxy = ys[i]; }
    int y0 = (int)floor(miny), y1 = (int)ceil(maxy);
    if (y0 < 0) y0 = 0;
    if (y1 > dest_h()) y1 = dest_h();
    double xint[MAX_POLY_PTS];
    for (int yy = y0; yy < y1; yy++) {
        int cnt = 0;
        double py = yy + 0.5;
        for (int i = 0; i < n && cnt < MAX_POLY_PTS; i++) {
            int j = (i + 1) % n;
            double ya = ys[i], yb = ys[j];
            if ((ya <= py && yb > py) || (yb <= py && ya > py)) {
                double t = (py - ya) / (yb - ya);
                xint[cnt++] = xs[i] + t * (xs[j] - xs[i]);
            }
        }
        /* insertion sort the crossings */
        for (int i = 1; i < cnt; i++) {
            double v = xint[i]; int k = i - 1;
            while (k >= 0 && xint[k] > v) { xint[k + 1] = xint[k]; k--; }
            xint[k + 1] = v;
        }
        for (int i = 0; i + 1 < cnt; i += 2) {
            int xa = (int)ceil(xint[i] - 0.5), xb = (int)ceil(xint[i + 1] - 0.5);
            blend_span(xa, xb, yy, c, a);
        }
    }
}

/* ── image cache ───────────────────────────────────────────────────── */

#define MAX_IMAGES 128
typedef struct {
    char path[160];
    uint8_t *rgba;   /* R,G,B,A byte order, row 0 = top */
    int w, h;
    int active;
    int is_canvas;
} image_t;
static image_t images[MAX_IMAGES];

static image_t *image_by_id(int id) {
    if (id < 0 || id >= MAX_IMAGES || !images[id].active) return NULL;
    return &images[id];
}

static int image_load(const char *path) {
    for (int i = 0; i < MAX_IMAGES; i++)
        if (images[i].active && !images[i].is_canvas && strcmp(images[i].path, path) == 0)
            return i;
    int slot = -1;
    for (int i = 0; i < MAX_IMAGES; i++) if (!images[i].active) { slot = i; break; }
    if (slot < 0) return -1;
    int size = wc_asset_size(path, (unsigned int)strlen(path));
    if (size <= 0) {
        char line[200];
        snprintf(line, sizeof line, "image asset not found: %s", path);
        wc_log(line, (unsigned int)strlen(line));
        return -1;
    }
    unsigned char *buf = (unsigned char *)malloc((size_t)size);
    if (!buf) return -1;
    wc_load_asset(path, (unsigned int)strlen(path), (char *)buf, (unsigned int)size);
    int w, h, comp;
    uint8_t *pixels = stbi_load_from_memory(buf, size, &w, &h, &comp, 4);
    free(buf);
    if (!pixels) {
        char line[200];
        snprintf(line, sizeof line, "image decode failed: %s (%s)", path, stbi_failure_reason());
        wc_log(line, (unsigned int)strlen(line));
        return -1;
    }
    image_t *im = &images[slot];
    snprintf(im->path, sizeof im->path, "%s", path);
    im->rgba = pixels; im->w = w; im->h = h; im->active = 1; im->is_canvas = 0;
    return slot;
}

static int canvas_new(int w, int h) {
    int slot = -1;
    for (int i = 0; i < MAX_IMAGES; i++) if (!images[i].active) { slot = i; break; }
    if (slot < 0) return -1;
    if (w < 1) w = DEFAULT_WIDTH;
    if (h < 1) h = DEFAULT_HEIGHT;
    if (w > 2048) w = 2048;
    if (h > 2048) h = 2048;
    image_t *im = &images[slot];
    im->rgba = (uint8_t *)calloc((size_t)w * h * 4, 1);
    if (!im->rgba) return -1;
    im->w = w; im->h = h; im->active = 1; im->is_canvas = 1;
    wcl_r2d_forget(im->rgba);   /* a recycled slot must not reuse a stale texture */
    snprintf(im->path, sizeof im->path, "@canvas:%d", slot);
    return slot;
}

/* Draw an image with a source quad, dest position, rotation (radians, CW to
 * match LOVE's screen space), scale, and origin offset. Tint = current color. */
static void draw_image(image_t *im, double x, double y, double rot,
                       double sx, double sy, double ox, double oy,
                       int qx, int qy, int qw, int qh) {
    if (!im || !im->rgba) return;
    if (qw <= 0) { qx = 0; qw = im->w; }
    if (qh <= 0) { qy = 0; qh = im->h; }
    double dw = qw * sx, dh = qh * sy;
    if (dw == 0 || dh == 0) return;

    double cs = cos(rot), sn = sin(rot);
    /* corners of the dest parallelogram, relative to (x,y) after origin shift */
    double cxs[4], cys[4];
    double lx[4] = { -ox * sx, dw - ox * sx, dw - ox * sx, -ox * sx };
    double ly[4] = { -oy * sy, -oy * sy, dh - oy * sy, dh - oy * sy };
    double minx = 1e18, maxx = -1e18, miny = 1e18, maxy = -1e18;
    for (int i = 0; i < 4; i++) {
        cxs[i] = x + lx[i] * cs - ly[i] * sn;
        cys[i] = y + lx[i] * sn + ly[i] * cs;
        if (cxs[i] < minx) minx = cxs[i];
        if (cxs[i] > maxx) maxx = cxs[i];
        if (cys[i] < miny) miny = cys[i];
        if (cys[i] > maxy) maxy = cys[i];
    }
    int x0 = (int)floor(minx), x1 = (int)ceil(maxx);
    int y0 = (int)floor(miny), y1 = (int)ceil(maxy);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > dest_w()) x1 = dest_w();
    if (y1 > dest_h()) y1 = dest_h();

    int tr = cur_r, tg = cur_g, tb = cur_b, ta = cur_a;

    /* GL sprite path, to the screen with ordinary alpha blending. Rotation
     * and flips come free: cxs/cys above are already the transformed
     * destination corners, so GL gets the same geometry the software path
     * scans, with no second implementation of the transform. Canvas targets
     * and additive still fall back (and have tripped cpu_mode). */
    {
        if (wcl_r2d_sprite(im->rgba, im->w, im->h, cxs, cys,
                           qx, qy, qw, qh,
                           (uint32_t)((tr << 16) | (tg << 8) | tb), ta)) {
            return;
        }
    }

    /* Loop invariants hoisted out. The per-pixel EXPRESSION is unchanged:
     * this is deliberately NOT strength-reduced.
     *
     * The obvious optimization -- replace `/dw` with `* (1.0/dw)` -- is
     * WRONG here and cost a day to pin down. When dw is not a power of two,
     * 1.0/dw is inexact, and x*(1/dw) rounds to a DIFFERENT source texel
     * than x/dw. Cavern's sprites land exactly on such sizes (dw = 67.5,
     * 54.75, 672, ...), which shifted 5.5% of the screen's pixels. Same
     * reason incremental u,v stepping is unsafe: accumulated error moves
     * the texel. Rendering here is bit-exact by contract, so the divisions
     * stay and the speed comes from everything around them. */
    const double bu = ox * sx, by_ = oy * sy;
    const int iw = im->w, ih = im->h;
    const uint8_t *const base = im->rgba;
    const int untinted = (tr == 255 && tg == 255 && tb == 255);
    const int opaque_tint = (ta == 255);

    /* blend_px re-reads five globals and re-tests bounds, scissor and blend
     * mode for EVERY pixel, none of which change during a blit. Decide once
     * here whether the simple path applies -- straight to the framebuffer,
     * no scissor, no additive blending -- and inline it. That is the common
     * case for tiles and sprites. Anything else falls through to blend_px
     * unchanged, so behaviour is identical either way. */
    const int fast_dst  = (!rt_buf && !sc_on && !blend_add);
    const int fast_cnv  = ( rt_buf && !sc_on && !blend_add);

    /* Row-constant halves of the inverse transform: py never varies across
     * a row, so py*sn and py*cs are computed once per row instead of once
     * per pixel. This is exact -- same operands, same order. */
    /* Unrotated blits (sn == 0, cs == 1) are 84% of a real game's draws, and
     * for them the whole v axis is ROW-CONSTANT: uy reduces to py, so fy and
     * iy can be computed once per row instead of once per pixel. Verified
     * exact over every dw the port produces -- it is the same expression,
     * just evaluated fewer times. That removes one of the two per-pixel
     * divisions, which is the only remaining cost that cannot be made
     * cheaper without changing results. */
    const int axis_aligned = (sn == 0.0 && cs == 1.0);

    for (int yy = y0; yy < y1; yy++) {
        const double py = yy + 0.5 - y;
        const double py_sn = py * sn, py_cs = py * cs;
        uint32_t *const row  = fast_dst ? &wc_framebuffer[yy * DEFAULT_WIDTH] : NULL;
        uint8_t  *const crow = fast_cnv ? &rt_buf[yy * rt_w * 4] : NULL;

        int row_iy = 0;
        if (axis_aligned) {
            const double fy = (py + by_) / dh;
            if (fy < 0 || fy >= 1) continue;         /* whole row is outside */
            row_iy = qy + (int)(fy * qh);
            if (row_iy < 0 || row_iy >= ih) continue;
        }

        for (int xx = x0; xx < x1; xx++) {
            const double px = xx + 0.5 - x;
            const double ux =  px * cs + py_sn;
            const double fx = (ux + bu) / dw;        /* division kept: exact */
            if (fx < 0 || fx >= 1) continue;
            const int ix = qx + (int)(fx * qw);
            if (ix < 0 || ix >= iw) continue;

            int iy;
            if (axis_aligned) {
                iy = row_iy;
            } else {
                const double uy = -px * sn + py_cs;
                const double fy = (uy + by_) / dh;
                if (fy < 0 || fy >= 1) continue;
                iy = qy + (int)(fy * qh);
                if (iy < 0 || iy >= ih) continue;
            }

            const uint8_t *const p = &base[(iy * iw + ix) * 4];
            /* Most pixels of a sprite sheet are fully transparent; testing
             * the raw alpha first skips the multiply and the colour work.
             * (p[3]*ta)/255 <= 0 exactly when p[3]==0 or ta==0, so this is
             * the same predicate, just cheaper. */
            const int sa = p[3];
            if (sa == 0) continue;
            const int a = opaque_tint ? sa : (sa * ta) / 255;
            if (a <= 0) continue;

            /* x*255/255 == x for all 0..255, so the untinted path is exact */
            const uint32_t rgb = untinted
                ? (((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2])
                : ((((uint32_t)(p[0] * tr / 255)) << 16)
                 | (((uint32_t)(p[1] * tg / 255)) << 8)
                 |  ((uint32_t)(p[2] * tb / 255)));
            if (row) {
                /* identical arithmetic to blend_px's framebuffer path */
                uint32_t *d = &row[xx];
                if (a >= 255) {
                    *d = rgb;
                } else {
                    const uint32_t o = *d;
                    const uint32_t sr = (rgb >> 16) & 0xFF;
                    const uint32_t sg = (rgb >> 8) & 0xFF;
                    const uint32_t sb = rgb & 0xFF;
                    const uint32_t ia = 255 - a;
                    const uint32_t rr = div255(sr * a + ((o >> 16) & 0xFF) * ia);
                    const uint32_t gg = div255(sg * a + ((o >> 8) & 0xFF) * ia);
                    const uint32_t bb = div255(sb * a + (o & 0xFF) * ia);
                    *d = (rr << 16) | (gg << 8) | bb;
                }
            } else if (crow) {
                /* canvas destination, decisions hoisted the same way.
                 * Arithmetic copied verbatim from blend_px's rt_buf path. */
                uint8_t *cp = &crow[xx * 4];
                const uint32_t da = cp[3];
                const uint32_t oa = a + da * (255 - a) / 255;
                if (oa != 0) {
                    const uint32_t sr = (rgb >> 16) & 0xFF;
                    const uint32_t sg = (rgb >> 8) & 0xFF;
                    const uint32_t sb = rgb & 0xFF;
                    cp[0] = (uint8_t)((sr * a + cp[0] * da * (255 - a) / 255) / oa);
                    cp[1] = (uint8_t)((sg * a + cp[1] * da * (255 - a) / 255) / oa);
                    cp[2] = (uint8_t)((sb * a + cp[2] * da * (255 - a) / 255) / oa);
                    cp[3] = (uint8_t)oa;
                }
            } else {
                blend_px(xx, yy, rgb, a);
            }
        }
    }
}

/* ── 5x7 bitfont (default font) ────────────────────────────────────── */
static const uint8_t FONT[85][7] = {
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E},{0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},{0x07,0x02,0x02,0x02,0x02,0x12,0x0C},
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11},{0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},{0x11,0x19,0x15,0x13,0x11,0x11,0x11},
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},{0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},{0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11},{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
    /* 26: digits */
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F},{0x0E,0x11,0x01,0x06,0x01,0x11,0x0E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},{0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    /* 36: space - . ! : > */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C},{0x04,0x04,0x04,0x04,0x04,0x00,0x04},
    {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00},{0x00,0x04,0x02,0x1F,0x02,0x04,0x00},
    /* 42: lowercase a-z */
    {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F},{0x10,0x10,0x1E,0x11,0x11,0x11,0x1E},
    {0x00,0x00,0x0F,0x10,0x10,0x10,0x0F},{0x01,0x01,0x0F,0x11,0x11,0x11,0x0F},
    {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E},{0x06,0x09,0x08,0x1C,0x08,0x08,0x08},
    {0x00,0x0F,0x11,0x11,0x0F,0x01,0x0E},{0x10,0x10,0x1E,0x11,0x11,0x11,0x11},
    {0x04,0x00,0x0C,0x04,0x04,0x04,0x0E},{0x02,0x00,0x06,0x02,0x02,0x12,0x0C},
    {0x10,0x10,0x12,0x14,0x18,0x14,0x12},{0x0C,0x04,0x04,0x04,0x04,0x04,0x0E},
    {0x00,0x00,0x1A,0x15,0x15,0x15,0x15},{0x00,0x00,0x1E,0x11,0x11,0x11,0x11},
    {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E},{0x00,0x00,0x1E,0x11,0x1E,0x10,0x10},
    {0x00,0x00,0x0F,0x11,0x0F,0x01,0x01},{0x00,0x00,0x16,0x19,0x10,0x10,0x10},
    {0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E},{0x08,0x08,0x1C,0x08,0x08,0x09,0x06},
    {0x00,0x00,0x11,0x11,0x11,0x13,0x0D},{0x00,0x00,0x11,0x11,0x11,0x0A,0x04},
    {0x00,0x00,0x11,0x11,0x15,0x15,0x0A},{0x00,0x00,0x11,0x0A,0x04,0x0A,0x11},
    {0x00,0x00,0x11,0x11,0x0F,0x01,0x0E},{0x00,0x00,0x1F,0x02,0x04,0x08,0x1F},
    /* 68: + = / ( ) , ? % * < # _ ' " ; [ ] */
    {0x00,0x04,0x04,0x1F,0x04,0x04,0x00},{0x00,0x00,0x1F,0x00,0x1F,0x00,0x00},
    {0x01,0x02,0x02,0x04,0x08,0x08,0x10},{0x02,0x04,0x08,0x08,0x08,0x04,0x02},
    {0x08,0x04,0x02,0x02,0x02,0x04,0x08},{0x00,0x00,0x00,0x00,0x0C,0x04,0x08},
    {0x0E,0x11,0x01,0x06,0x04,0x00,0x04},{0x19,0x1A,0x02,0x04,0x08,0x0B,0x13},
    {0x00,0x11,0x0A,0x1F,0x0A,0x11,0x00},{0x02,0x04,0x08,0x10,0x08,0x04,0x02},
    {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A},{0x00,0x00,0x00,0x00,0x00,0x00,0x1F},
    {0x04,0x04,0x08,0x00,0x00,0x00,0x00},{0x0A,0x0A,0x14,0x00,0x00,0x00,0x00},
    {0x00,0x0C,0x0C,0x00,0x0C,0x04,0x08},{0x0E,0x08,0x08,0x08,0x08,0x08,0x0E},
    {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E},
};

static int glyph_index(char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return 42 + (ch - 'a');
    if (ch >= '0' && ch <= '9') return 26 + (ch - '0');
    switch (ch) {
        case ' ': return 36; case '-': return 37; case '.': return 38;
        case '!': return 39; case ':': return 40; case '>': return 41;
        case '+': return 68; case '=': return 69; case '/': return 70;
        case '(': return 71; case ')': return 72; case ',': return 73;
        case '?': return 74; case '%': return 75; case '*': return 76;
        case '<': return 77; case '#': return 78; case '_': return 79;
        case '\'': return 80; case '"': return 81; case ';': return 82;
        case '[': return 83; case ']': return 84;
    }
    return 36; /* unmapped codepoints render as a space */
}

/* y is the TOP of the text box (LOVE convention) */
static void draw_bitfont(int x, int y_top, const char *s, int scale, uint32_t c, int a) {
    /* Every glyph pixel is a fill_rect, which already routes to the GL solid
     * batch, so bitfont text needs nothing special -- a whole string lands in
     * one draw call. (This used to disable GL because the TTF path next to it
     * was CPU-only; that is no longer true.) */
    if (scale < 1) scale = 1;
    int cx = x;
    for (; *s; s++) {
        if (*s == '\n') { cx = x; y_top += 8 * scale; continue; }
        const uint8_t *g = FONT[glyph_index(*s)];
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++)
                if (g[row] & (0x10 >> col))
                    fill_rect(cx + col * scale, y_top + row * scale, scale, scale, c, a);
        cx += 6 * scale;
    }
}

/* ── TTF fonts: baked-atlas cache per (path, pixel height) ─────────── */

#define MAX_FONTS 8
#define TTF_FIRST_CHAR 32
#define TTF_CHAR_COUNT 95
typedef struct {
    char path[160];
    int px;
    unsigned char *atlas;
    int aw, ah;
    stbtt_bakedchar chars[TTF_CHAR_COUNT];
    int active;
} font_t;
static font_t fonts[MAX_FONTS];
static int font_rr;

static font_t *font_by_id(int id) {
    if (id < 0 || id >= MAX_FONTS || !fonts[id].active) return NULL;
    return &fonts[id];
}

static int font_load(const char *path, int px) {
    if (px < 6) px = 6;
    if (px > 256) px = 256;
    for (int i = 0; i < MAX_FONTS; i++)
        if (fonts[i].active && fonts[i].px == px && strcmp(fonts[i].path, path) == 0)
            return i;
    int size = wc_asset_size(path, (unsigned int)strlen(path));
    if (size <= 0) {
        char line[200];
        snprintf(line, sizeof line, "font asset not found: %s", path);
        wc_log(line, (unsigned int)strlen(line));
        return -1;
    }
    unsigned char *buf = (unsigned char *)malloc((size_t)size);
    if (!buf) return -1;
    wc_load_asset(path, (unsigned int)strlen(path), (char *)buf, (unsigned int)size);

    int slot = -1;
    for (int i = 0; i < MAX_FONTS; i++) if (!fonts[i].active) { slot = i; break; }
    if (slot < 0) { slot = font_rr; font_rr = (font_rr + 1) % MAX_FONTS; free(fonts[slot].atlas); fonts[slot].atlas = NULL; }
    font_t *f = &fonts[slot];

    int aw = 256;
    while (aw < px * 12 && aw < 2048) aw *= 2;
    int ah = aw;
    f->atlas = (unsigned char *)malloc((size_t)aw * ah);
    if (!f->atlas) { free(buf); return -1; }
    int res = stbtt_BakeFontBitmap(buf, 0, (float)px, f->atlas, aw, ah,
                                   TTF_FIRST_CHAR, TTF_CHAR_COUNT, f->chars);
    if (res <= 0 && res != -TTF_CHAR_COUNT && aw < 2048) {
        free(f->atlas);
        aw = ah = aw * 2;
        f->atlas = (unsigned char *)malloc((size_t)aw * ah);
        if (!f->atlas) { free(buf); return -1; }
        stbtt_BakeFontBitmap(buf, 0, (float)px, f->atlas, aw, ah,
                             TTF_FIRST_CHAR, TTF_CHAR_COUNT, f->chars);
    }
    free(buf);
    f->aw = aw; f->ah = ah; f->px = px;
    snprintf(f->path, sizeof f->path, "%s", path);
    f->active = 1;
    return slot;
}

/* y_top = top of the text box; baseline sits px*0.8 below (top-left origin) */
static void draw_ttf(font_t *f, int x, int y_top, const char *s, uint32_t c, int a) {
    /* GL path: stb_truetype already baked every glyph into f->atlas, so the
     * font uploads once as a coverage texture and each glyph is a quad in
     * the shared textured batch. Only the screen target is handled here;
     * canvases and additive fall through to the CPU rasterizer below. */
    const int gl_text = (!rt_buf && wcl_r2d_active());
    float pen_x = (float)x;
    float baseline = (float)y_top + f->px * 0.8f;
    for (; *s; s++) {
        if (*s == '\n') { pen_x = (float)x; baseline += f->px * 1.2f; continue; }
        int ch = (unsigned char)*s;
        if (ch < TTF_FIRST_CHAR || ch >= TTF_FIRST_CHAR + TTF_CHAR_COUNT) { pen_x += f->px * 0.4f; continue; }
        stbtt_bakedchar *b = &f->chars[ch - TTF_FIRST_CHAR];
        int gw = b->x1 - b->x0, gh = b->y1 - b->y0;
        int gx = (int)(pen_x + b->xoff);
        int gy = (int)(baseline + b->yoff);
        if (gl_text) {
            /* A space has no glyph box at all, which is not a failure --
             * skip it and keep the pen advance below. Treating a zero-sized
             * glyph as an error dropped every string containing a space onto
             * the CPU backend, which is most of them. */
            if (gw > 0 && gh > 0 &&
                !wcl_r2d_glyph(f->atlas, f->aw, f->ah, gx, gy, gw, gh,
                               b->x0, b->y0, gw, gh, c, a)) {
                /* out of glyph textures: give up on GL for the run rather
                 * than draw half the string on each backend */
                wcl_r2d_disable();
                return;
            }
        } else {
            for (int row = 0; row < gh; row++)
                for (int col = 0; col < gw; col++) {
                    int cov = f->atlas[(b->y0 + row) * f->aw + (b->x0 + col)];
                    if (cov) blend_px(gx + col, gy + row, c, cov * a / 255);
                }
        }
        pen_x += b->xadvance;
    }
}

static int measure_ttf(font_t *f, const char *s) {
    float w = 0, best = 0;
    for (; *s; s++) {
        if (*s == '\n') { if (w > best) best = w; w = 0; continue; }
        int ch = (unsigned char)*s;
        if (ch < TTF_FIRST_CHAR || ch >= TTF_FIRST_CHAR + TTF_CHAR_COUNT) { w += f->px * 0.4f; continue; }
        w += f->chars[ch - TTF_FIRST_CHAR].xadvance;
    }
    if (w > best) best = w;
    return (int)(best + 0.5f);
}

/* ── sound cache ───────────────────────────────────────────────────── */

#define MAX_SOUND_PATHS 64
typedef struct { char path[160]; int id; } sound_entry_t;
static sound_entry_t sound_paths[MAX_SOUND_PATHS];
static int sound_path_count;

static int sound_load(const char *path) {
    for (int i = 0; i < sound_path_count; i++)
        if (strcmp(sound_paths[i].path, path) == 0) return sound_paths[i].id;
    if (sound_path_count >= MAX_SOUND_PATHS) return -1;

    /* Resolve the asset, allowing an extension fallback: a cart may ship
     * WAV where the game's source asks for OGG (or vice versa). Codec
     * choice is a packaging decision, not a gameplay one, so honoring the
     * sibling file beats failing and leaving the game silent. */
    char resolved[192];
    snprintf(resolved, sizeof resolved, "%s", path);
    int size = wc_asset_size(resolved, (unsigned int)strlen(resolved));
    if (size <= 0) {
        size_t rl = strlen(resolved);
        if (rl > 4) {
            const char *alt = NULL;
            if (strcmp(resolved + rl - 4, ".ogg") == 0) alt = ".wav";
            else if (strcmp(resolved + rl - 4, ".wav") == 0) alt = ".ogg";
            if (alt) {
                memcpy(resolved + rl - 4, alt, 5);
                size = wc_asset_size(resolved, (unsigned int)strlen(resolved));
            }
        }
    }
    if (size <= 0) {
        char line[200];
        snprintf(line, sizeof line, "sound asset not found: %s", path);
        wc_log(line, (unsigned int)strlen(line));
        return -1;
    }
    path = resolved;
    unsigned char *buf = (unsigned char *)malloc((size_t)size);
    if (!buf) return -1;
    wc_load_asset(path, (unsigned int)strlen(path), (char *)buf, (unsigned int)size);
    int id;
    size_t plen = strlen(path);
    if (plen > 4 && strcmp(path + plen - 4, ".ogg") == 0) {
        int chans = 0, rate = 0;
        short *pcm = NULL;
        int frames = stb_vorbis_decode_memory(buf, size, &chans, &rate, &pcm);
        if (frames > 0 && pcm) {
            id = wc_mixer_load_raw(pcm, frames, chans > 1 ? 2 : 1, rate);
            free(pcm);
        } else {
            char line[200];
            snprintf(line, sizeof line, "ogg decode failed: %s", path);
            wc_log(line, (unsigned int)strlen(line));
            id = -1;
        }
    } else {
        id = wc_mixer_load_wav(buf, size);
    }
    free(buf);
    sound_entry_t *e = &sound_paths[sound_path_count++];
    snprintf(e->path, sizeof e->path, "%s", path);
    e->id = id;
    return id;
}

#define MAX_BEEPS 16
static struct { int freq; int id; } beeps[MAX_BEEPS];
static int beep_count;

static int beep_get(int freq) {
    for (int i = 0; i < beep_count; i++) if (beeps[i].freq == freq) return beeps[i].id;
    if (beep_count >= MAX_BEEPS) return beeps[0].id;
    int frames = 48000 / 5; /* 200ms decaying square */
    int16_t *pcm = (int16_t *)malloc(sizeof(int16_t) * frames);
    if (!pcm) return -1;
    float phase = 0;
    for (int i = 0; i < frames; i++) {
        float env = 1.0f - (float)i / frames;
        pcm[i] = (int16_t)((phase < 0.5f ? 6000 : -6000) * env);
        phase += (float)freq / 48000.0f;
        if (phase >= 1.0f) phase -= 1.0f;
    }
    int id = wc_mixer_load_raw(pcm, frames, 1, 48000);
    free(pcm);
    beeps[beep_count].freq = freq;
    beeps[beep_count].id = id;
    beep_count++;
    return id;
}

/* ── Lua -> C bridges (module `wc`) ────────────────────────────────── */

#define ARGI(n)  ((int)luaL_checkinteger(L, (n)))
#define ARGD(n)  ((double)luaL_checknumber(L, (n)))
#define OPTI(n,d) ((int)luaL_optinteger(L, (n), (d)))
#define OPTD(n,d) ((double)luaL_optnumber(L, (n), (d)))

static int l_set_color(lua_State *S) {
    cur_r = ARGI(1); cur_g = ARGI(2); cur_b = ARGI(3); cur_a = OPTI(4, 255);
    if (cur_r < 0) cur_r = 0; if (cur_r > 255) cur_r = 255;
    if (cur_g < 0) cur_g = 0; if (cur_g > 255) cur_g = 255;
    if (cur_b < 0) cur_b = 0; if (cur_b > 255) cur_b = 255;
    if (cur_a < 0) cur_a = 0; if (cur_a > 255) cur_a = 255;
    return 0;
}

static uint32_t frame_clear_color;   /* last clear, for the GL path's glClear */

static int l_clear(lua_State *S) {
    int r = ARGI(1), g = ARGI(2), b = ARGI(3);
    uint32_t c = (((uint32_t)r & 0xFF) << 16) | (((uint32_t)g & 0xFF) << 8) | ((uint32_t)b & 0xFF);
    if (!rt_buf) {
        frame_clear_color = c;
        /* On the GL path wcl_r2d_begin already issued glClear for this
         * colour; writing the framebuffer too would just be wasted work. */
        if (wcl_r2d_active()) return 0;
    } else if (wcl_r2d_active()) {
        /* clearing a canvas clears its FBO */
        wcl_r2d_clear(c, 255);
        return 0;
    }
    if (rt_buf) {
        for (int i = 0; i < rt_w * rt_h; i++) {
            rt_buf[i * 4 + 0] = (uint8_t)r; rt_buf[i * 4 + 1] = (uint8_t)g;
            rt_buf[i * 4 + 2] = (uint8_t)b; rt_buf[i * 4 + 3] = 255;
        }
    } else {
        for (int i = 0; i < DEFAULT_WIDTH * DEFAULT_HEIGHT; i++) wc_framebuffer[i] = c;
    }
    return 0;
}

static int l_rect(lua_State *S) {
    int filled = ARGI(1);
    double x = ARGD(2), y = ARGD(3), w = ARGD(4), h = ARGD(5);
    dbg_draw_calls++;
    if (filled) fill_rect((int)x, (int)y, (int)w, (int)h, cur_rgb(), cur_a);
    else {
        int xi = (int)x, yi = (int)y, wi = (int)w, hi = (int)h;
        if (wi <= 0 || hi <= 0) return 0;
        raster_line(xi, yi, xi + wi - 1, yi, cur_rgb(), cur_a);
        raster_line(xi, yi + hi - 1, xi + wi - 1, yi + hi - 1, cur_rgb(), cur_a);
        raster_line(xi, yi, xi, yi + hi - 1, cur_rgb(), cur_a);
        raster_line(xi + wi - 1, yi, xi + wi - 1, yi + hi - 1, cur_rgb(), cur_a);
    }
    return 0;
}

static int l_circle(lua_State *S) {
    int filled = ARGI(1);
    dbg_draw_calls++;
    raster_circle((int)ARGD(2), (int)ARGD(3), (int)ARGD(4), cur_rgb(), cur_a, filled);
    return 0;
}

static int l_line(lua_State *S) {
    dbg_draw_calls++;
    raster_line((int)ARGD(1), (int)ARGD(2), (int)ARGD(3), (int)ARGD(4), cur_rgb(), cur_a);
    return 0;
}

static int l_point(lua_State *S) {
    blend_px((int)ARGD(1), (int)ARGD(2), cur_rgb(), cur_a);
    return 0;
}

/* polygon(filled, {x1,y1,x2,y2,...}) - already world-transformed by Lua */
static int l_polygon(lua_State *S) {
    int filled = ARGI(1);
    luaL_checktype(S, 2, LUA_TTABLE);
    int n = (int)lua_rawlen(S, 2) / 2;
    if (n < 3) return 0;
    if (n > MAX_POLY_PTS) n = MAX_POLY_PTS;
    double xs[MAX_POLY_PTS], ys[MAX_POLY_PTS];
    for (int i = 0; i < n; i++) {
        lua_rawgeti(S, 2, i * 2 + 1); xs[i] = lua_tonumber(S, -1); lua_pop(S, 1);
        lua_rawgeti(S, 2, i * 2 + 2); ys[i] = lua_tonumber(S, -1); lua_pop(S, 1);
    }
    dbg_draw_calls++;
    raster_polygon(xs, ys, n, cur_rgb(), cur_a, filled);
    return 0;
}

static int l_image_load(lua_State *S) {
    const char *path = luaL_checkstring(S, 1);
    int id = image_load(path);
    if (id < 0) { lua_pushnil(S); return 1; }
    lua_pushinteger(S, id);
    lua_pushinteger(S, images[id].w);
    lua_pushinteger(S, images[id].h);
    return 3;
}

static int l_canvas_new(lua_State *S) {
    int id = canvas_new(ARGI(1), ARGI(2));
    if (id < 0) { lua_pushnil(S); return 1; }
    lua_pushinteger(S, id);
    return 1;
}

static int l_image_draw(lua_State *S) {
    image_t *im = image_by_id(ARGI(1));
    if (!im) return 0;
    dbg_draw_calls++;
    draw_image(im, ARGD(2), ARGD(3), OPTD(4, 0), OPTD(5, 1), OPTD(6, 1),
               OPTD(7, 0), OPTD(8, 0), OPTI(9, 0), OPTI(10, 0), OPTI(11, 0), OPTI(12, 0));
    return 0;
}

static int l_set_canvas(lua_State *S) {
    if (lua_isnoneornil(S, 1)) {
        rt_buf = NULL;
        wcl_r2d_target(NULL, 0, 0);          /* back to the screen */
        return 0;
    }
    image_t *im = image_by_id(ARGI(1));
    if (!im || !im->is_canvas) {
        rt_buf = NULL;
        wcl_r2d_target(NULL, 0, 0);
        return 0;
    }
    rt_buf = im->rgba; rt_w = im->w; rt_h = im->h;
    /* An FBO keyed on the same pointer sprites use, so drawing this canvas
     * later samples the texture that was just rendered into. If the backend
     * cannot provide one, drop to software for the rest of the run rather
     * than let GL and CPU each hold half the canvas. */
    if (!wcl_r2d_target(im->rgba, im->w, im->h)) wcl_r2d_disable();
    return 0;
}

static int l_set_scissor(lua_State *S) {
    if (lua_isnoneornil(S, 1)) {
        sc_on = 0;
        wcl_r2d_scissor(0, 0, -1, -1);
        return 0;
    }
    sc_on = 1; sc_x = ARGI(1); sc_y = ARGI(2); sc_w = ARGI(3); sc_h = ARGI(4);
    /* glScissor clips the same half-open rect the software path does, so both
     * backends agree on the edges without either changing. */
    wcl_r2d_scissor(sc_x, sc_y, sc_w, sc_h);
    return 0;
}

static int l_set_blend(lua_State *S) {
    blend_add = ARGI(1);
    wcl_r2d_blend_add(blend_add);
    return 0;
}

static int l_font_load(lua_State *S) {
    const char *path = luaL_checkstring(S, 1);
    int id = font_load(path, ARGI(2));
    if (id < 0) { lua_pushnil(S); return 1; }
    lua_pushinteger(S, id);
    lua_pushinteger(S, fonts[id].px);
    return 2;
}

/* print(text, x, y, font_id_or_-1, scale) */
static int l_print(lua_State *S) {
    const char *s = luaL_checkstring(S, 1);
    int x = (int)ARGD(2), y = (int)ARGD(3);
    int fid = OPTI(4, -1);
    int scale = OPTI(5, 1);
    dbg_draw_calls++;
    font_t *f = font_by_id(fid);
    if (f) draw_ttf(f, x, y, s, cur_rgb(), cur_a);
    else draw_bitfont(x, y, s, scale, cur_rgb(), cur_a);
    return 0;
}

static int l_text_size(lua_State *S) {
    const char *s = luaL_checkstring(S, 1);
    int fid = OPTI(2, -1);
    int scale = OPTI(3, 1);
    font_t *f = font_by_id(fid);
    if (f) {
        lua_pushinteger(S, measure_ttf(f, s));
        lua_pushinteger(S, f->px);
        return 2;
    }
    int len = 0, best = 0, lines = 1;
    for (const char *p = s; *p; p++) {
        if (*p == '\n') { if (len > best) best = len; len = 0; lines++; }
        else len++;
    }
    if (len > best) best = len;
    lua_pushinteger(S, best > 0 ? (6 * best - 1) * scale : 0);
    lua_pushinteger(S, 8 * scale * lines);
    return 2;
}

static int l_sound_load(lua_State *S) {
    int id = sound_load(luaL_checkstring(S, 1));
    if (id < 0) { lua_pushnil(S); return 1; }
    lua_pushinteger(S, id);
    return 1;
}

static int l_sound_play(lua_State *S) {
    int id = ARGI(1);
    lua_pushinteger(S, wc_mixer_play(id, (float)OPTD(2, 1.0), OPTI(3, 0)));
    return 1;
}

static int l_sound_stop(lua_State *S)     { wc_mixer_stop(ARGI(1)); return 0; }
static int l_sound_gain(lua_State *S)     { wc_mixer_set_volume(ARGI(1), (float)ARGD(2)); return 0; }
static int l_sound_pitch(lua_State *S)    { wc_mixer_set_pitch(ARGI(1), (float)ARGD(2)); return 0; }
static int l_sound_paused(lua_State *S)   { wc_mixer_set_paused(ARGI(1), ARGI(2)); return 0; }
static int l_sound_seek(lua_State *S)     { wc_mixer_seek(ARGI(1), (float)ARGD(2)); return 0; }
static int l_sound_playtime(lua_State *S) { lua_pushnumber(S, wc_mixer_playtime(ARGI(1))); return 1; }
static int l_sound_playing(lua_State *S)  { lua_pushboolean(S, wc_mixer_is_playing(ARGI(1))); return 1; }

static int l_beep(lua_State *S) {
    int id = beep_get(ARGI(1));
    if (id >= 0) wc_mixer_play(id, (float)OPTD(2, 0.8), 0);
    return 0;
}

static int l_log(lua_State *S) {
    size_t n; const char *s = luaL_checklstring(S, 1, &n);
    wc_log(s, (unsigned int)n);
    return 0;
}

static int l_mark(lua_State *S) { wc_debug_mark((uint32_t)ARGI(1)); return 0; }

static int l_debug_set(lua_State *S) {
    int slot = ARGI(1), v = ARGI(2);
    if (slot == 0) dbg_score = v;
    else if (slot == 1) dbg_aux = v;
    return 0;
}

static int l_rand(lua_State *S) {
    /* host-seeded xorshift -> [0,1); the ONLY entropy source */
    lua_pushnumber(S, (double)(wc_rand() >> 8) / 16777216.0);
    return 1;
}

static int l_save_write(lua_State *S) {
    size_t n; const char *s = luaL_checklstring(S, 1, &n);
    if (n > SAVE_BYTES - 4) n = SAVE_BYTES - 4;
    wc_save[0] = (uint8_t)(n & 0xFF);
    wc_save[1] = (uint8_t)((n >> 8) & 0xFF);
    wc_save[2] = (uint8_t)((n >> 16) & 0xFF);
    wc_save[3] = (uint8_t)((n >> 24) & 0xFF);
    memcpy(wc_save + 4, s, n);
    lua_pushboolean(S, 1);
    return 1;
}

static int l_save_read(lua_State *S) {
    uint32_t n = (uint32_t)wc_save[0] | ((uint32_t)wc_save[1] << 8)
               | ((uint32_t)wc_save[2] << 16) | ((uint32_t)wc_save[3] << 24);
    if (n == 0 || n > SAVE_BYTES - 4) { lua_pushnil(S); return 1; }
    lua_pushlstring(S, (const char *)(wc_save + 4), n);
    return 1;
}

/* read a cart asset as a string (love.filesystem.read + the require bridge) */
static int l_asset_read(lua_State *S) {
    const char *path = luaL_checkstring(S, 1);
    int size = wc_asset_size(path, (unsigned int)strlen(path));
    if (size < 0) { lua_pushnil(S); return 1; }
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) { lua_pushnil(S); return 1; }
    int got = wc_load_asset(path, (unsigned int)strlen(path), buf, (unsigned int)size);
    if (got < 0) { free(buf); lua_pushnil(S); return 1; }
    buf[got] = 0;
    lua_pushlstring(S, buf, (size_t)got);
    free(buf);
    return 1;
}

/* pointer(index) -> x, y, buttons, active  (wasmcart unified mouse/touch) */
static int l_pointer(lua_State *S) {
    int i = OPTI(1, 0);
    if (i < 0 || i > 9) i = 0;
    lua_pushinteger(S, wc_pointers[i].x);
    lua_pushinteger(S, wc_pointers[i].y);
    lua_pushinteger(S, wc_pointers[i].buttons);
    lua_pushboolean(S, wc_pointers[i].active);
    return 4;
}

static int l_asset_exists(lua_State *S) {
    const char *path = luaL_checkstring(S, 1);
    lua_pushboolean(S, wc_asset_size(path, (unsigned int)strlen(path)) >= 0);
    return 1;
}

static const luaL_Reg wc_lib[] = {
    {"set_color",   l_set_color},
    {"clear",       l_clear},
    {"rect",        l_rect},
    {"circle",      l_circle},
    {"line",        l_line},
    {"point",       l_point},
    {"polygon",     l_polygon},
    {"image_load",  l_image_load},
    {"image_draw",  l_image_draw},
    {"canvas_new",  l_canvas_new},
    {"set_canvas",  l_set_canvas},
    {"set_scissor", l_set_scissor},
    {"set_blend",   l_set_blend},
    {"font_load",   l_font_load},
    {"print",       l_print},
    {"text_size",   l_text_size},
    {"sound_load",  l_sound_load},
    {"sound_play",  l_sound_play},
    {"sound_stop",  l_sound_stop},
    {"sound_gain",  l_sound_gain},
    {"sound_pitch", l_sound_pitch},
    {"sound_paused", l_sound_paused},
    {"sound_seek",  l_sound_seek},
    {"sound_playtime", l_sound_playtime},
    {"sound_playing",  l_sound_playing},
    {"beep",        l_beep},
    {"log",         l_log},
    {"mark",        l_mark},
    {"debug_set",   l_debug_set},
    {"rand",        l_rand},
    {"save_write",  l_save_write},
    {"save_read",   l_save_read},
    {"asset_read",  l_asset_read},
    {"asset_exists", l_asset_exists},
    {"pointer",     l_pointer},
    {NULL, NULL}
};

/* ── boot ──────────────────────────────────────────────────────────── */

/* Open exactly the libraries a cart may have.
 *
 * This REPLACES Lua's linit.c on purpose. io/os/package are not "opened then
 * deleted" - they are never linked in at all, which is what keeps the engine
 * free of WASI imports (fd_write/fd_seek/clock_time_get) and makes the cart
 * runnable on hosts that provide nothing but the wasmcart env module. A cart
 * has no filesystem, no clock but wc_time, and no dynamic loading, by design.
 */
static void open_cart_libs(lua_State *S) {
    static const luaL_Reg libs[] = {
        {LUA_GNAME,       luaopen_base},
        {LUA_TABLIBNAME,  luaopen_table},
        {LUA_STRLIBNAME,  luaopen_string},
        {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8},
        {LUA_COLIBNAME,   luaopen_coroutine},
        {LUA_DBLIBNAME,   luaopen_debug},
        {NULL, NULL}
    };
    for (const luaL_Reg *lib = libs; lib->func; lib++) {
        luaL_requiref(S, lib->name, lib->func, 1);
        lua_pop(S, 1);
    }
    /* base's file-facing entries have no meaning without io/package */
    lua_pushnil(S); lua_setglobal(S, "dofile");
    lua_pushnil(S); lua_setglobal(S, "loadfile");
}

static int lua_guard(const char *what, int status) {
    if (status == LUA_OK) return 0;
    const char *msg = lua_tostring(L, -1);
    char line[512];
    snprintf(line, sizeof line, "lua error in %s: %s", what, msg ? msg : "(unknown)");
    wc_log(line, (unsigned int)strlen(line));
    wc_debug_mark(MARK_LUA_ERROR);
    lua_pop(L, 1);
    dbg_lua_ok = 0;
    return 1;
}

static char *load_asset_text(const char *name) {
    int size = wc_asset_size(name, (unsigned int)strlen(name));
    if (size < 0) return NULL;
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) return NULL;
    int got = wc_load_asset(name, (unsigned int)strlen(name), buf, (unsigned int)size);
    if (got < 0) { free(buf); return NULL; }
    buf[got] = 0;
    return buf;
}

static int run_source(const char *name) {
    char *src = load_asset_text(name);
    if (!src) {
        char line[200];
        snprintf(line, sizeof line, "missing asset: %s", name);
        wc_log(line, (unsigned int)strlen(line));
        dbg_lua_ok = 0;
        return -1;
    }
    int st = luaL_loadbuffer(L, src, strlen(src), name);
    free(src);
    if (st != LUA_OK) return lua_guard(name, st) ? -1 : 0;
    st = lua_pcall(L, 0, 0, 0);
    return lua_guard(name, st) ? -1 : 0;
}

WC_EXPORT wc_info_t *wc_get_info(void) {
    WC_FILL_INFO(WC_FLAG_DEBUG | WC_FLAG_DETERMINISTIC | WC_FLAG_POINTER);
    wc_info.audio_sample_rate = 48000;
#ifdef WCL_USE_GL
    wc_info.gpu_api = 1;  /* WebGL2/GLES3: we present via wc_gl_blit */
#endif
    wc_info.save_ptr  = (uint32_t)(uintptr_t)wc_save;
    wc_info.save_size = sizeof(wc_save);
    return &wc_info;
}

WC_EXPORT_INIT void wc_init(void) {
    WC_LOG("wasmcart-lua: boot");
    wc_debug_mark(MARK_BOOT);
    wc_mixer_init();

    L = luaL_newstate();
    if (!L) { WC_LOG("luaL_newstate failed"); dbg_lua_ok = 0; return; }
    open_cart_libs(L);

    /* the C bridge */
    luaL_newlib(L, wc_lib);
    lua_setglobal(L, "wc");

    /* Box2D v3 as the global `b2`; prelude.lua builds love.physics and the
     * windfield-shaped collider API on top of it */
    wcl_open_physics(L);

    /* screen dims for the prelude */
    lua_pushinteger(L, DEFAULT_WIDTH);  lua_setglobal(L, "__WC_WIDTH");
    lua_pushinteger(L, DEFAULT_HEIGHT); lua_setglobal(L, "__WC_HEIGHT");

    int prelude_ok;
    if (wc_asset_size("prelude.lua", 11) > 0) {
        prelude_ok = run_source("prelude.lua") == 0;
    } else {
        int st = luaL_loadbuffer(L, (const char *)WCL_PRELUDE, WCL_PRELUDE_LEN, "prelude");
        if (st == LUA_OK) st = lua_pcall(L, 0, 0, 0);
        prelude_ok = !lua_guard("prelude(embedded)", st);
    }
    if (!prelude_ok) return;

    if (run_source("main.lua") != 0) return;

    /* love.load() */
    lua_getglobal(L, "__wasmcart_load");
    if (lua_isfunction(L, -1)) lua_guard("love.load", lua_pcall(L, 0, 0, 0));
    else lua_pop(L, 1);
}

WC_EXPORT_RENDER void wc_render(void) {
    dbg_draw_calls = 0;

#ifdef WCL_ENABLE_GL2D
    static int gl2d_started;
    if (!gl2d_started) { gl2d_started = 1; wcl_r2d_init(DEFAULT_WIDTH, DEFAULT_HEIGHT); }
    /* Returns 0 in sticky cpu_mode, in which case every draw below takes the
     * software path and wcl_r2d_end blits the finished framebuffer. */
    if (wcl_r2d_begin(frame_clear_color)) {
        /* the prelude resets sc_on per frame; keep GL's state in step */
        if (!sc_on) wcl_r2d_scissor(0, 0, -1, -1);
        else wcl_r2d_scissor(sc_x, sc_y, sc_w, sc_h);
    }
#endif

    if (L && dbg_lua_ok) {
        lua_getglobal(L, "__wasmcart_frame");
        if (lua_isfunction(L, -1)) {
            /* all four pads: buttons + both sticks + triggers */
            for (int p = 0; p < 4; p++) {
                wc_pad_t *pd = &wc_pads[p];
                lua_pushinteger(L, (lua_Integer)pd->buttons);
                lua_pushinteger(L, (lua_Integer)pd->left_x);
                lua_pushinteger(L, (lua_Integer)pd->left_y);
                lua_pushinteger(L, (lua_Integer)pd->right_x);
                lua_pushinteger(L, (lua_Integer)pd->right_y);
            }
            lua_guard("frame", lua_pcall(L, 20, 0, 0));
            rt_buf = NULL; /* never leave a frame aimed at a canvas */
            sc_on = 0;
            blend_add = 0;
        } else {
            lua_pop(L, 1);
        }

        /* GC at the frame boundary only: a budgeted incremental step keeps
         * collection off the critical path instead of spiking mid-frame. */
        lua_gc(L, LUA_GCSTEP, 4);
        dbg_gc_kb = (uint32_t)lua_gc(L, LUA_GCCOUNT);
    }

    if (!dbg_lua_ok) {
        /* CPU-drawn, so the frame has to be presented via the blit path. */
        wcl_r2d_disable();
        /* LOVE-style error screen: blue, readable, keeps the cart alive */
        for (int i = 0; i < DEFAULT_WIDTH * DEFAULT_HEIGHT; i++)
            wc_framebuffer[i] = 0x00201F6E;
        cur_r = 255; cur_g = 255; cur_b = 255; cur_a = 255;
        draw_bitfont(48, 48, "LUA ERROR - SEE LOG", 4, 0x00FFFFFF, 255);
        draw_bitfont(48, 110, "the cart kept running so you can read this.", 2, 0x00C8C8FF, 255);
    }

    double delta = wc_time.delta_ms;
    if (delta <= 0 || delta > 100) delta = 1000.0 / 60.0;
    int frames = (int)(48000.0 * delta / 1000.0);
    wc_mixer_mix_f32(wc_audio_ring, AUDIO_CAP, &wc_audio_write_cursor, frames);

    tick_n++;
    dbg_tick = tick_n;

#ifdef WCL_ENABLE_GL2D
    /* Flushes the batches on the GL path, or blits the CPU framebuffer when
     * this frame fell back. Either way the frame is on screen after this. */
    wcl_r2d_end(wc_framebuffer);
#elif defined(WCL_USE_GL)
    gl_present();
#endif
}
