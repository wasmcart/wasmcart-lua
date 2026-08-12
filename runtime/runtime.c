/*
 * runtime.c - the wasmcart Lua engine core.
 *
 * Game developers never touch this file. They take the prebuilt engine wasm
 * and write Lua in app/. The engine embeds a full Lua VM (real Lua: closures,
 * coroutines, metatables, GC, require) and runs a game written against a
 * LOVE-style API: love.load / love.update(dt) / love.draw, top-left origin,
 * 1280x720 by default -- a cart's conf.lua can pick up to 1920x1080.
 * 60 fps fixed step.
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
#include "render3d_gl.h"
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
/* physics3d.c: Box3D bound as the global `b3` */
void wcl_open_physics3d(lua_State *L);

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* The Lua-side API surface, embedded at build time (prelude.inc is generated
 * from prelude.lua by build.sh). An app/prelude.lua asset overrides it. */
#include "prelude.inc"

#define DEFAULT_WIDTH  1280
#define DEFAULT_HEIGHT 720
#define MAX_WIDTH  1920
#define MAX_HEIGHT 1080
#define AUDIO_CAP 4096

WC_CART_BUFFERS;


/* The cart's ACTUAL resolution, chosen once at boot. Defaults hold unless the
 * cart ships a conf.lua whose love.conf(t) sets t.window.width/height (the
 * LOVE idiom), clamped to MAX_*. The buffers above are MAX-sized, so this is
 * only ever a stride/extent -- never a reallocation. Everything below must
 * index the framebuffer with scr_w, not DEFAULT_WIDTH: the host reads the
 * frame as scr_w * scr_h contiguous pixels. */
static int scr_w = DEFAULT_WIDTH;
static int scr_h = DEFAULT_HEIGHT;

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
    for (int i = 0; i < scr_w * scr_h; i++) {
        const uint32_t px = src[i];
        dst[0] = (uint8_t)(px >> 16);  /* R */
        dst[1] = (uint8_t)(px >> 8);   /* G */
        dst[2] = (uint8_t)px;          /* B */
        dst[3] = 255;                  /* X is unused: present opaque */
        dst += 4;
    }
    wc_gl_blit(gl_rgba, scr_w, scr_h);
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
static uint32_t dbg_gpu2d = 1;

WC_DEBUG_FIELDS(
    WC_DBG("tick_count", dbg_tick,       WC_DBG_U32),
    WC_DBG("score",      dbg_score,      WC_DBG_I32),
    WC_DBG("aux",        dbg_aux,        WC_DBG_I32),
    WC_DBG("lua_ok",     dbg_lua_ok,     WC_DBG_U32),
    WC_DBG("gc_kb",      dbg_gc_kb,      WC_DBG_U32),
    WC_DBG("draw_calls", dbg_draw_calls, WC_DBG_U32),
    /* 1 while the GPU 2D path is live, 0 once anything has dropped the
     * frame to the software rasterizer. A DROP IS A FAILURE, not a
     * fallback: it is whole-frame, sticky for the rest of the run, and
     * invisible except that everything gets slower. Exposed so a test can
     * assert on it rather than a human noticing the frame rate. */
    WC_DBG("gpu2d", dbg_gpu2d, WC_DBG_U32)
)

#define MARK_BOOT 1
#define MARK_LUA_ERROR 9

static lua_State *L;
static uint32_t tick_n;

/* cart "SRAM": hosts persist this (.sav next to the cart) */
#define SAVE_BYTES 4096
static uint8_t wc_save[SAVE_BYTES];

/* ── native-host region access ────────────────────────────────────────
 *
 * wc_info_t hands every shared region to the host as a uint32_t OFFSET into
 * wasm linear memory. That is exact under wasm (pointers are 32-bit) and
 * lossy everywhere else, so a 64-bit native host cannot use those fields.
 *
 * The regions themselves are `static` (WC_CART_BUFFERS), i.e. not linkable
 * by name. A native host is linked into this same address space, so the
 * honest fix is to hand it the real addresses once. The wasm ABI struct is
 * untouched — wasm hosts see exactly what they always saw.
 */
#ifdef WC_NATIVE_HOST
#include "wc_native.h"
static const wc_native_regions_t wcl_native_regions = {
    .framebuffer        = wc_framebuffer,
    .audio_ring         = wc_audio_ring,
    .audio_write_cursor = &wc_audio_write_cursor,
    .audio_cap          = AUDIO_CAP,
    .pads               = wc_pads,
    .pointers           = wc_pointers,
    .keys               = wc_keys,
    .time               = &wc_time,
    .host_info          = &wc_host_info,
    .info               = &wc_info,
    .save               = wc_save,
    .save_size          = SAVE_BYTES,
};
const wc_native_regions_t *wc_native_regions(void) { return &wcl_native_regions; }
#endif

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

static inline int dest_w(void) { return rt_buf ? rt_w : scr_w; }
static inline int dest_h(void) { return rt_buf ? rt_h : scr_h; }

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
    uint32_t *p = &wc_framebuffer[y * scr_w + x];
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

    uint32_t *p = &wc_framebuffer[y * scr_w + x0];
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
            uint32_t *p = &wc_framebuffer[y0 * scr_w + x0];
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
    /* Filled circles are evaluated in the fragment shader with the same span
     * rule used below, so they are bit-exact at every radius -- no fan, no
     * minimum radius, no fallback. */
    if (filled) {
        if (wcl_r2d_circle(cx, cy, r, c, a)) return;
        wcl_r2d_disable_why("raster_circle: filled circle rejected by wcl_r2d_circle");
    }
    /* The OUTLINE writes single pixels. blend_span goes straight to the
     * framebuffer and would be INVISIBLE on a GL frame, so on the GL path
     * each pixel goes through fill_rect (a 1x1 quad) and joins the solid
     * batch instead. Same pixels, one draw call for the whole circle. */
    const int gl_outline = (!filled && !rt_buf && wcl_r2d_active());
#define CIRC_PX(px, py) do { \
    if (gl_outline) fill_rect((px), (py), 1, 1, c, a); \
    else blend_span((px), (px) + 1, (py), c, a); \
} while (0)
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
        CIRC_PX(cx + x, cy + y);
        CIRC_PX(cx + y, cy + x);
        CIRC_PX(cx - y, cy + x);
        CIRC_PX(cx - x, cy + y);
        CIRC_PX(cx - x, cy - y);
        CIRC_PX(cx - y, cy - x);
        CIRC_PX(cx + y, cy - x);
        CIRC_PX(cx + x, cy - y);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
#undef CIRC_PX
}

/* convex/concave polygon scanline fill (even-odd) */
/* One shared cap (render2d_gl.h). It was 64 here and 64 there, separately,
 * which is exactly how the two silently disagree. */
#define MAX_POLY_PTS WCL_MAX_POLY_PTS
static void raster_polygon(const double *xs, const double *ys, int n,
                           uint32_t c, int a, int filled) {
    if (n < 3) return;
    /* Filled convex polygons go to GL as a triangle fan; outlines are just
     * lines, which are already on the GL path. A concave fill has no correct
     * fan, so it keeps the scanline fill and drops the frame to software. */
    if (filled) {
        if (wcl_r2d_poly(xs, ys, n, c, a)) return;
        wcl_r2d_disable_why("raster_polygon: filled polygon rejected by wcl_r2d_poly (concave/self-intersecting or >cap)");
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
    if (w < 1) w = scr_w;
    if (h < 1) h = scr_h;
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
        uint32_t *const row  = fast_dst ? &wc_framebuffer[yy * scr_w] : NULL;
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
                wcl_r2d_disable_why("draw_text: out of glyph textures");
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
        for (int i = 0; i < scr_w * scr_h; i++) wc_framebuffer[i] = c;
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

/* image_pixel(id, x, y[, r, g, b, a]) - read or write one pixel.
 *
 * ImageData in LOVE is CPU-side pixels a cart can inspect and edit before
 * they become a texture. The engine already keeps every image's RGBA bytes
 * in linear memory (image_t.rgba), so this is a direct accessor rather than
 * a second representation.
 *
 * Writing marks the image so the GL backend re-uploads it: a texture cached
 * from the old pixels would otherwise keep drawing them, which looks like
 * setPixel doing nothing. */
static int l_image_pixel(lua_State *S) {
    image_t *im = image_by_id(ARGI(1));
    int x = ARGI(2), y = ARGI(3);
    if (!im || !im->rgba || x < 0 || y < 0 || x >= im->w || y >= im->h) {
        if (lua_gettop(S) <= 3) { lua_pushnil(S); return 1; }
        return 0;
    }
    uint8_t *p = &im->rgba[((size_t)y * im->w + x) * 4];
    if (lua_gettop(S) <= 3) {              /* read */
        lua_pushnumber(S, p[0] / 255.0);
        lua_pushnumber(S, p[1] / 255.0);
        lua_pushnumber(S, p[2] / 255.0);
        lua_pushnumber(S, p[3] / 255.0);
        return 4;
    }
    double r = ARGD(4), g = ARGD(5), b = ARGD(6);
    double a = lua_isnoneornil(S, 7) ? 1.0 : ARGD(7);
    #define CLAMP01(v) ((v) < 0 ? 0 : ((v) > 1 ? 1 : (v)))
    p[0] = (uint8_t)(CLAMP01(r) * 255.0 + 0.5);
    p[1] = (uint8_t)(CLAMP01(g) * 255.0 + 0.5);
    p[2] = (uint8_t)(CLAMP01(b) * 255.0 + 0.5);
    p[3] = (uint8_t)(CLAMP01(a) * 255.0 + 0.5);
    #undef CLAMP01
    /* The GL side caches by payload POINTER, so the contents changing under
     * it is invisible. Drop the cached texture and let it re-upload. */
    wcl_r2d_forget(im->rgba);
    return 0;
}

/* image_blank(w, h) -> id - an all-zero RGBA image a cart can write into. */
static int l_image_blank(lua_State *S) {
    int id = canvas_new(ARGI(1), ARGI(2));
    if (id < 0) { lua_pushnil(S); return 1; }
    /* Not a render target -- just CPU pixels. Clearing is_canvas keeps
     * set_canvas from accepting it as one. */
    images[id].is_canvas = 0;
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
    if (!wcl_r2d_target(im->rgba, im->w, im->h)) wcl_r2d_disable_why("setCanvas: wcl_r2d_target could not provide an FBO");
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

/* wc.gpu2d() -> true while the GPU 2D path is live.
 *
 * A cart normally has no business asking, and the boundary exists so it
 * cannot. This is here for CONFORMANCE TESTS: a drop to the software
 * rasterizer is whole-frame, sticky, and otherwise invisible, so a test
 * that draws every primitive needs a way to assert none of them caused one. */
static int l_gpu2d(lua_State *S) {
    lua_pushboolean(S, wcl_r2d_active());
    return 1;
}

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

/* Rumble. pad_id is 0-based at the ABI boundary; the prelude does the
 * 1-based -> 0-based conversion so love.pad numbering stays uniform. */
static int l_pad_has_rumble(lua_State *S) {
    lua_pushboolean(S, wc_pad_has_rumble((unsigned int)ARGI(1)) != 0);
    return 1;
}

static int l_pad_rumble(lua_State *S) {
    wc_pad_rumble((unsigned int)ARGI(1), (float)ARGD(2), (float)ARGD(3),
                  (unsigned int)ARGI(4));
    return 0;
}

static int l_pad_rumble_stop(lua_State *S) {
    wc_pad_rumble_stop((unsigned int)ARGI(1));
    return 0;
}

/* ── networking (wc_peer_*) ────────────────────────────────────────────
 *
 * One primitive: a connection to a peer. The transport underneath is the
 * host's business and the cart cannot tell what it is, so this layer adds
 * nothing but the Lua marshalling.
 *
 * Payloads are BINARY. Lua strings hold arbitrary bytes including NUL, so
 * they are the carrier, and every crossing here uses explicit lengths -
 * a strlen anywhere on this path would silently truncate a message at its
 * first zero byte, which for packed binary game state is most of them.
 *
 * Peer ids are host-assigned integers, not indices, and there is no
 * conversion at this boundary: unlike the pads, what Lua holds is exactly
 * what the ABI uses. Handing an id back verbatim is what makes it usable
 * as a stable key for a player table.
 */

/* Names arrive from a remote machine, so this is the length the engine is
 * willing to receive - not a protocol limit. Anything longer is truncated
 * rather than trusted to fit. */
#define PEER_NAME_MAX 128

static int l_peer_open(lua_State *S) {
    size_t len = 0;
    const char *addr = luaL_checklstring(S, 1, &len);
    int id = wc_peer_open(addr, (unsigned int)len);
    if (id < 0) { lua_pushnil(S); return 1; }
    lua_pushinteger(S, id);
    return 1;
}

static int l_peer_close(lua_State *S) {
    wc_peer_close(ARGI(1));
    return 0;
}

static int l_peer_send(lua_State *S) {
    int id = ARGI(1);
    size_t len = 0;
    const char *data = luaL_checklstring(S, 2, &len);
    lua_pushinteger(S, wc_peer_send(id, data, (unsigned int)len));
    return 1;
}

static int l_peer_broadcast(lua_State *S) {
    size_t len = 0;
    const char *data = luaL_checklstring(S, 1, &len);
    lua_pushinteger(S, wc_peer_broadcast(data, (unsigned int)len));
    return 1;
}

static int l_peer_state(lua_State *S) {
    lua_pushinteger(S, wc_peer_state(ARGI(1)));
    return 1;
}

static int l_peer_count(lua_State *S) {
    lua_pushinteger(S, wc_peer_count());
    return 1;
}

/* index is 0-based here, matching the ABI; the prelude builds the 1-based
 * Lua list from it. */
static int l_peer_id(lua_State *S) {
    int id = wc_peer_id((unsigned int)ARGI(1));
    if (id < 0) { lua_pushnil(S); return 1; }
    lua_pushinteger(S, id);
    return 1;
}

static int l_peer_name(lua_State *S) {
    char buf[PEER_NAME_MAX];
    int n = wc_peer_name(ARGI(1), buf, (unsigned int)sizeof buf);
    if (n <= 0) { lua_pushnil(S); return 1; }
    /* The host returns bytes written INCLUDING the NUL terminator, and the
     * name itself is untrusted text that may hold anything. Trust the count,
     * not the bytes: scan for the terminator inside the reported span and
     * fall back to the full span if the host did not write one. */
    size_t span = (size_t)n <= sizeof buf ? (size_t)n : sizeof buf;
    size_t text = span;
    for (size_t i = 0; i < span; i++) {
        if (buf[i] == 0) { text = i; break; }
    }
    lua_pushlstring(S, buf, text);
    return 1;
}

static int l_peer_transport(lua_State *S) {
    lua_pushinteger(S, wc_peer_transport(ARGI(1)));
    return 1;
}

/* Host-called peer callbacks.
 *
 * These are queued rather than dispatched into Lua on the spot. The host is
 * free to deliver them at any point it likes, including while a frame's Lua
 * is on the stack; re-entering the VM from there would run a cart's handler
 * inside its own update. Queueing keeps every cart callback in one place -
 * the top of the frame, before love.update - so ordering is the cart's to
 * reason about and a handler that errors takes down a frame, not the host's
 * callback.
 *
 * The queue is bounded. A peer that floods faster than the cart drains must
 * cost the cart frames, not memory: past the cap the oldest events are
 * dropped and the cart is told how many, so it can resynchronize rather than
 * silently play on a partial stream.
 */
#define PEER_EVQ_CAP     256
#define PEER_MSG_MAX     8192

typedef struct {
    int      kind;      /* 0 connect, 1 message, 2 disconnect, 3 error */
    int      peer_id;
    uint32_t len;
    char     data[PEER_MSG_MAX];
} peer_event_t;

static peer_event_t peer_evq[PEER_EVQ_CAP];
static int peer_evq_head = 0, peer_evq_len = 0;
static uint32_t peer_dropped = 0;

static peer_event_t *peer_evq_push(int kind, int peer_id) {
    if (peer_evq_len == PEER_EVQ_CAP) {
        peer_evq_head = (peer_evq_head + 1) % PEER_EVQ_CAP;
        peer_evq_len--;
        peer_dropped++;
    }
    int slot = (peer_evq_head + peer_evq_len) % PEER_EVQ_CAP;
    peer_evq_len++;
    peer_event_t *ev = &peer_evq[slot];
    ev->kind = kind;
    ev->peer_id = peer_id;
    ev->len = 0;
    return ev;
}

static void peer_evq_copy(peer_event_t *ev, const void *src, unsigned int len) {
    if (len > PEER_MSG_MAX) len = PEER_MSG_MAX;
    if (len && src) memcpy(ev->data, src, len);
    ev->len = len;
}

__attribute__((export_name("wc_peer_on_connect")))
void wc_peer_on_connect(int peer_id, const char *name, unsigned int name_len) {
    peer_evq_copy(peer_evq_push(0, peer_id), name, name_len);
}

__attribute__((export_name("wc_peer_on_message")))
void wc_peer_on_message(int peer_id, const void *data, unsigned int len) {
    peer_evq_copy(peer_evq_push(1, peer_id), data, len);
}

__attribute__((export_name("wc_peer_on_disconnect")))
void wc_peer_on_disconnect(int peer_id) {
    peer_evq_push(2, peer_id);
}

__attribute__((export_name("wc_peer_on_error")))
void wc_peer_on_error(int peer_id) {
    peer_evq_push(3, peer_id);
}

/* peer_poll() -> kind, peer_id, payload  (nil when the queue is empty).
 * The prelude drains this at the top of each frame and turns each event into
 * the matching love.net callback. */
static int l_peer_poll(lua_State *S) {
    if (peer_evq_len == 0) { lua_pushnil(S); return 1; }
    peer_event_t *ev = &peer_evq[peer_evq_head];
    peer_evq_head = (peer_evq_head + 1) % PEER_EVQ_CAP;
    peer_evq_len--;
    lua_pushinteger(S, ev->kind);
    lua_pushinteger(S, ev->peer_id);
    lua_pushlstring(S, ev->data, (size_t)ev->len);
    return 3;
}

/* Number of events dropped since the last call, and reset. Reported rather
 * than hidden: a cart that cares about a complete stream needs to know its
 * view has a hole in it. */
static int l_peer_dropped(lua_State *S) {
    lua_pushinteger(S, (lua_Integer)peer_dropped);
    peer_dropped = 0;
    return 1;
}

/* ── custom shaders ────────────────────────────────────────────────────
 *
 * The whole feature is GL-only by construction: a shader IS a GPU program,
 * and there is no software rasterizer path that could run one. So rather
 * than let a cart draw silently unshaded, shader_new refuses on a host with
 * no GL and the prelude turns that into a clear Lua error.
 *
 * shader_use additionally refuses while the engine is in the sticky
 * cpu_mode fallback: at that point every draw is CPU-rasterized and a bound
 * shader would have no effect at all, which is the exact "looks fine, wrong
 * pixels" failure this engine spends its comments avoiding.
 */
static int l_shader_new(lua_State *S) {
    const char *pixel = lua_isnoneornil(S, 1) ? NULL : luaL_checkstring(S, 1);
    const char *vertex = lua_isnoneornil(S, 2) ? NULL : luaL_checkstring(S, 2);
    /* The prelude passes is_3d when the cart declared a 3D vertex format, so
     * the shader gets the vec3-position prologue rather than the 2D one. */
    int is_3d = lua_toboolean(S, 3);
    /* >0 selects the multi-output fragment scaffold (effect2). */
    int mrt = OPTI(4, 0);
    int h = wcl_r2d_shader_new(pixel, vertex, is_3d, mrt);
    if (h < 0) { lua_pushnil(S); return 1; }
    lua_pushinteger(S, h);
    return 1;
}

static int l_shader_use(lua_State *S) {
    int h = lua_isnoneornil(S, 1) ? -1 : ARGI(1);
    if (h >= 0 && !wcl_r2d_active()) {
        /* GL is off for this run (no context, or the sticky software
         * fallback tripped). Say so instead of drawing unshaded. */
        lua_pushboolean(S, 0);
        return 1;
    }
    wcl_r2d_shader_use(h);
    lua_pushboolean(S, 1);
    return 1;
}

/* send(handle, name, ...) - numbers, or a table of up to 4 numbers, or an
 * image id. Returns false when the uniform is not in the linked program,
 * which is the normal way a name typo shows up. */
static int l_shader_send(lua_State *S) {
    int h = ARGI(1);
    const char *name = luaL_checkstring(S, 2);
    if (lua_isnumber(S, 3) && lua_gettop(S) <= 6) {
        float v[4];
        int n = lua_gettop(S) - 2;
        if (n > 4) n = 4;
        for (int i = 0; i < n; i++) v[i] = (float)lua_tonumber(S, 3 + i);
        lua_pushboolean(S, wcl_r2d_shader_send_float(h, name, v, n));
        return 1;
    }
    if (lua_istable(S, 3)) {
        /* A flat table is a vector; a table of tables is a matrix. Only 4x4
         * is accepted, which is the one LOVE shaders actually send. */
        lua_rawgeti(S, 3, 1);
        int nested = lua_istable(S, -1);
        lua_pop(S, 1);
        int len = (int)lua_rawlen(S, 3);
        if (nested) {
            if (len != 4) {
                lua_pushboolean(S, 0);
                return 1;
            }
            float m[16];
            for (int r = 0; r < 4; r++) {
                lua_rawgeti(S, 3, r + 1);
                for (int c = 0; c < 4; c++) {
                    lua_rawgeti(S, -1, c + 1);
                    m[r * 4 + c] = (float)lua_tonumber(S, -1);
                    lua_pop(S, 1);
                }
                lua_pop(S, 1);
            }
            lua_pushboolean(S, wcl_r2d_shader_send_float(h, name, m, 16));
            return 1;
        }
        /* A FLAT 16-number table is also a mat4, and it is the shape every
         * LOVE 3D library actually sends: g3d's matrices are plain arrays of
         * 16 (see its matrices.lua), never tables of rows. Rejecting this
         * meant a cart's projection matrix silently failed to upload and the
         * model rendered at the identity transform -- geometry on screen,
         * flat and enormous, with no error anywhere. */
        if (len == 16) {
            float m[16];
            for (int i = 0; i < 16; i++) {
                lua_rawgeti(S, 3, i + 1);
                m[i] = (float)lua_tonumber(S, -1);
                lua_pop(S, 1);
            }
            lua_pushboolean(S, wcl_r2d_shader_send_float(h, name, m, 16));
            return 1;
        }
        if (len < 1 || len > 4) { lua_pushboolean(S, 0); return 1; }
        float v[4];
        for (int i = 0; i < len; i++) {
            lua_rawgeti(S, 3, i + 1);
            v[i] = (float)lua_tonumber(S, -1);
            lua_pop(S, 1);
        }
        lua_pushboolean(S, wcl_r2d_shader_send_float(h, name, v, len));
        return 1;
    }
    lua_pushboolean(S, 0);
    return 1;
}

/* send_image(handle, name, image_id) */
static int l_shader_send_image(lua_State *S) {
    int h = ARGI(1);
    const char *name = luaL_checkstring(S, 2);
    image_t *im = image_by_id(ARGI(3));
    if (!im || !im->rgba) { lua_pushboolean(S, 0); return 1; }
    lua_pushboolean(S, wcl_r2d_shader_send_image(h, name, im->rgba, im->w, im->h));
    return 1;
}

/* has_uniform(handle, name) - a read-only probe.
 *
 * NOT implementable as "try sending 0 and see if it took": that WRITES the
 * uniform, so asking whether a uniform exists would silently zero it. Games
 * call hasUniform to guard an optional uniform, immediately before setting
 * it, so the clobber would usually be invisible and occasionally not. */
static int l_shader_has_uniform(lua_State *S) {
    lua_pushboolean(S, wcl_r2d_shader_has_uniform(ARGI(1), luaL_checkstring(S, 2)));
    return 1;
}

static int l_shader_send_bool(lua_State *S) {
    int h = ARGI(1);
    const char *name = luaL_checkstring(S, 2);
    int v[4];
    int n = lua_gettop(S) - 2;
    if (n < 1) { lua_pushboolean(S, 0); return 1; }
    if (n > 4) n = 4;
    for (int i = 0; i < n; i++) v[i] = lua_toboolean(S, 3 + i) ? 1 : 0;
    lua_pushboolean(S, wcl_r2d_shader_send_int(h, name, v, n));
    return 1;
}

/* ── meshes ────────────────────────────────────────────────────────────
 *
 * The vertex data lives HERE, in C, not in a Lua table.
 *
 * A mesh is drawn every frame and its vertices rarely change, so marshalling
 * a Lua table across the boundary per frame would dominate the feature: a
 * 3000-vertex mesh is 24000 lua_rawgeti calls a frame to produce geometry
 * that did not move. Keeping the floats in a C array means a draw is one
 * call, and setVertex/setVertices write into that array directly. This is
 * also what LOVE does (a Mesh owns a GPU buffer), so the API shape follows
 * from it rather than being worked around.
 *
 * Layout is LOVE's default vertex format, 8 floats:
 *     x, y, u, v, r, g, b, a
 * which is the GL backend's vertex_t minus `rad`, so wcl_r2d_mesh does no
 * repacking at all.
 *
 * The draw expands the mesh's draw MODE (fan/strip/triangles) and its vertex
 * map into a triangle list here rather than in Lua, for the same reason.
 */
#define MAX_MESHES 32
/* Per-mesh vertex cap. The expansion buffer below is the real cost: a fan of
 * N vertices becomes 3*(N-2) triangle vertices, so the scratch is ~3x this
 * at 32 bytes each. 4096 keeps that at 1.5 MB, which is nothing against the
 * 64 MB heap and is far more geometry than a 2D cart draws in one mesh. */
#define MESH_MAX_VERTS 4096
typedef struct {
    float *verts;        /* 8 floats per vertex */
    int n;               /* vertex count */
    uint16_t *map;       /* vertex map (index buffer), or NULL */
    int map_n;
    int mode;            /* 0 fan, 1 strip, 2 triangles, 3 points */
    int tex;             /* image id, or -1 */
    int range_start, range_count;   /* -1 = the whole thing */
    int active;
} mesh_t;
static mesh_t meshes[MAX_MESHES];

/* Expanded triangle list. Sized for the worst case a single draw can ask
 * for: a fan or strip of MESH_MAX_VERTS makes (n-2) triangles. */
static float mesh_tris[MESH_MAX_VERTS * 3 * 8];

static mesh_t *mesh_by_id(int id) {
    if (id < 0 || id >= MAX_MESHES || !meshes[id].active) return NULL;
    return &meshes[id];
}

static int l_mesh_new(lua_State *S) {
    int n = ARGI(1);
    int mode = ARGI(2);
    if (n < 1 || n > MESH_MAX_VERTS) { lua_pushnil(S); return 1; }
    /* Meshes are GL-only, and a cart must find that out at newMesh rather
     * than by getting an empty screen. Same rule newShader follows: refusing
     * up front beats rendering nothing and reporting success. Reported as a
     * distinct return so the prelude can name the actual reason. */
    if (!wcl_r2d_active()) {
        lua_pushnil(S);
        lua_pushstring(S, "nogl");
        return 2;
    }
    int slot = -1;
    for (int i = 0; i < MAX_MESHES; i++) if (!meshes[i].active) { slot = i; break; }
    if (slot < 0) { lua_pushnil(S); return 1; }
    mesh_t *m = &meshes[slot];
    m->verts = (float *)calloc((size_t)n * 8, sizeof(float));
    if (!m->verts) { lua_pushnil(S); return 1; }
    /* LOVE's default vertex is white and opaque, and a cart that supplies
     * only x,y,u,v relies on that. calloc would give transparent black, so a
     * mesh built from positions alone would render nothing at all. */
    for (int i = 0; i < n; i++) {
        m->verts[i * 8 + 4] = 1.0f; m->verts[i * 8 + 5] = 1.0f;
        m->verts[i * 8 + 6] = 1.0f; m->verts[i * 8 + 7] = 1.0f;
    }
    m->n = n; m->mode = mode; m->tex = -1; m->active = 1;
    m->map = NULL; m->map_n = 0;
    m->range_start = -1; m->range_count = -1;
    lua_pushinteger(S, slot);
    return 1;
}

/* set_vertex(id, index0, x, y, u, v, r, g, b, a) - index is 0-BASED here;
 * the prelude does the 1-based conversion, once, where LOVE's API is. */
static int l_mesh_set_vertex(lua_State *S) {
    mesh_t *m = mesh_by_id(ARGI(1));
    int i = ARGI(2);
    if (!m || i < 0 || i >= m->n) return 0;
    float *v = &m->verts[i * 8];
    for (int k = 0; k < 8; k++) v[k] = (float)ARGD(3 + k);
    return 0;
}

static int l_mesh_get_vertex(lua_State *S) {
    mesh_t *m = mesh_by_id(ARGI(1));
    int i = ARGI(2);
    if (!m || i < 0 || i >= m->n) return 0;
    const float *v = &m->verts[i * 8];
    for (int k = 0; k < 8; k++) lua_pushnumber(S, v[k]);
    return 8;
}

static int l_mesh_set_texture(lua_State *S) {
    mesh_t *m = mesh_by_id(ARGI(1));
    if (!m) return 0;
    m->tex = lua_isnoneornil(S, 2) ? -1 : ARGI(2);
    return 0;
}

/* set_map(id, {i0, i1, ...}) with 0-based indices, or nil to clear. */
static int l_mesh_set_map(lua_State *S) {
    mesh_t *m = mesh_by_id(ARGI(1));
    if (!m) return 0;
    free(m->map); m->map = NULL; m->map_n = 0;
    if (lua_isnoneornil(S, 2)) return 0;
    luaL_checktype(S, 2, LUA_TTABLE);
    int n = (int)lua_rawlen(S, 2);
    if (n < 1) return 0;
    if (n > MESH_MAX_VERTS * 3) n = MESH_MAX_VERTS * 3;
    m->map = (uint16_t *)malloc((size_t)n * sizeof(uint16_t));
    if (!m->map) return 0;
    for (int i = 0; i < n; i++) {
        lua_rawgeti(S, 2, i + 1);
        int v = (int)lua_tointeger(S, -1);
        lua_pop(S, 1);
        if (v < 0) v = 0;
        if (v >= m->n) v = m->n - 1;
        m->map[i] = (uint16_t)v;
    }
    m->map_n = n;
    return 0;
}

static int l_mesh_get_map(lua_State *S) {
    mesh_t *m = mesh_by_id(ARGI(1));
    if (!m || !m->map) { lua_pushnil(S); return 1; }
    lua_createtable(S, m->map_n, 0);
    for (int i = 0; i < m->map_n; i++) {
        lua_pushinteger(S, m->map[i]);
        lua_rawseti(S, -2, i + 1);
    }
    return 1;
}

static int l_mesh_set_range(lua_State *S) {
    mesh_t *m = mesh_by_id(ARGI(1));
    if (!m) return 0;
    if (lua_isnoneornil(S, 2)) { m->range_start = -1; m->range_count = -1; return 0; }
    m->range_start = ARGI(2);
    m->range_count = ARGI(3);
    return 0;
}

static int l_mesh_get_range(lua_State *S) {
    mesh_t *m = mesh_by_id(ARGI(1));
    if (!m || m->range_start < 0) { lua_pushnil(S); return 1; }
    lua_pushinteger(S, m->range_start);
    lua_pushinteger(S, m->range_count);
    return 2;
}

static int l_mesh_release(lua_State *S) {
    mesh_t *m = mesh_by_id(ARGI(1));
    if (!m) return 0;
    free(m->verts); free(m->map);
    m->verts = NULL; m->map = NULL; m->active = 0;
    return 0;
}

/* mesh_draw(id, x, y, rot, sx, sy, ox, oy)
 *
 * The transform is applied HERE rather than in Lua: the whole point of
 * keeping the vertices in C is not to walk them from Lua every frame, and
 * transforming them in Lua would walk them twice. The parameters arrive
 * already composed with the transform stack by the prelude, exactly as
 * image_draw's do.
 *
 * Returns false when the mesh could not be rendered, so the prelude can say
 * why rather than leaving an empty screen.
 */
static int l_mesh_draw(lua_State *S) {
    mesh_t *m = mesh_by_id(ARGI(1));
    if (!m || m->n < 1) { lua_pushboolean(S, 0); return 1; }
    const double x = ARGD(2), y = ARGD(3), rot = ARGD(4);
    const double sx = ARGD(5), sy = ARGD(6), ox = ARGD(7), oy = ARGD(8);

    /* Which vertices, in which order. A vertex map replaces the implicit
     * 0,1,2,... sequence; the draw range then selects a window of THAT
     * sequence, which is LOVE's rule (the range indexes the map when there
     * is one, not the vertex array). */
    const int seq_n = m->map ? m->map_n : m->n;
    int start = 0, cnt = seq_n;
    if (m->range_start >= 0) {
        start = m->range_start;
        cnt = m->range_count;
        if (start < 0) start = 0;
        if (start > seq_n) start = seq_n;
        if (cnt < 0 || start + cnt > seq_n) cnt = seq_n - start;
    }
    if (cnt < 3) { lua_pushboolean(S, 1); return 1; }   /* nothing to draw is
                                                           not a failure */

#define MESH_SEQ(k) (m->map ? (int)m->map[start + (k)] : (start + (k)))

    /* Expand the draw mode into a triangle list. "points" is refused in the
     * prelude, so only these three arrive. */
    int tri_n = 0;
    int idx[3];
    const int cap = MESH_MAX_VERTS * 3;
    float *out = mesh_tris;

    const double c = cos(rot), s = sin(rot);
    /* Emit one vertex: apply origin, scale, rotation and translation, in
     * LOVE's order, then copy uv and colour through unchanged. */
    #define MESH_EMIT(vi) do {                                           \
        if (tri_n + 1 <= cap) {                                          \
            const float *src = &m->verts[(vi) * 8];                      \
            double px = ((double)src[0] - ox) * sx;                      \
            double py = ((double)src[1] - oy) * sy;                      \
            double rx = px * c - py * s;                                 \
            double ry = px * s + py * c;                                 \
            float *d = &out[tri_n * 8];                                  \
            d[0] = (float)(rx + x); d[1] = (float)(ry + y);              \
            for (int k = 2; k < 8; k++) d[k] = src[k];                   \
            tri_n++;                                                     \
        }                                                                \
    } while (0)

    if (m->mode == 0) {                     /* fan */
        for (int i = 1; i + 1 < cnt; i++) {
            idx[0] = MESH_SEQ(0); idx[1] = MESH_SEQ(i); idx[2] = MESH_SEQ(i + 1);
            for (int k = 0; k < 3; k++) MESH_EMIT(idx[k]);
        }
    } else if (m->mode == 1) {              /* strip */
        for (int i = 0; i + 2 < cnt; i++) {
            /* Winding alternates so every triangle faces the same way. There
             * is no backface culling here, so this does not change what is
             * drawn -- but it keeps the geometry identical to LOVE's, which
             * matters the moment a cart's own shader reads gl_FrontFacing. */
            if (i & 1) {
                idx[0] = MESH_SEQ(i + 1); idx[1] = MESH_SEQ(i); idx[2] = MESH_SEQ(i + 2);
            } else {
                idx[0] = MESH_SEQ(i); idx[1] = MESH_SEQ(i + 1); idx[2] = MESH_SEQ(i + 2);
            }
            for (int k = 0; k < 3; k++) MESH_EMIT(idx[k]);
        }
    } else {                                /* triangles */
        for (int i = 0; i + 2 < cnt; i += 3) {
            idx[0] = MESH_SEQ(i); idx[1] = MESH_SEQ(i + 1); idx[2] = MESH_SEQ(i + 2);
            for (int k = 0; k < 3; k++) MESH_EMIT(idx[k]);
        }
    }
#undef MESH_EMIT
#undef MESH_SEQ

    if (tri_n < 3) { lua_pushboolean(S, 1); return 1; }
    dbg_draw_calls++;

    image_t *im = (m->tex >= 0) ? image_by_id(m->tex) : NULL;
    lua_pushboolean(S, wcl_r2d_mesh(mesh_tris, tri_n,
                                    im ? im->rgba : NULL,
                                    im ? im->w : 0, im ? im->h : 0,
                                    cur_rgb(), cur_a));
    return 1;
}

/* ── 3D meshes ─────────────────────────────────────────────────────────
 *
 * The 2D bridge above marshals vertices into C once and then re-walks them
 * every frame to apply the transform. The 3D path does neither: the vertices
 * go straight into a GPU buffer at creation and the transform is the cart's
 * own matrices in its own vertex shader. So a 3D draw is a handle and a
 * colour, and no geometry crosses the Lua/C boundary after load.
 *
 * That is the difference that makes a real model viable. A 20k-triangle
 * model through the 2D path would be 160k lua_rawgeti calls and a 2.5 MB
 * upload EVERY FRAME; here it is one upload, ever.
 */
#define MESH3D_MAX_VERTS 200000
static wcl_vertex3d_t *mesh3d_scratch;
static int mesh3d_scratch_cap;

/* Grow the marshalling scratch to hold `n` vertices. One buffer reused
 * across every newMesh call: they are not reentrant and the buffer is dead
 * the moment the data reaches the GPU. */
static wcl_vertex3d_t *mesh3d_scratch_for(int n) {
    if (n <= mesh3d_scratch_cap) return mesh3d_scratch;
    wcl_vertex3d_t *p = (wcl_vertex3d_t *)realloc(
        mesh3d_scratch, (size_t)n * sizeof(wcl_vertex3d_t));
    if (!p) return NULL;
    mesh3d_scratch = p;
    mesh3d_scratch_cap = n;
    return p;
}

/* Read one vertex from a Lua table at stack index `t`.
 *
 * The layout is LOVE's 3D convention, the one g3d's vertexFormat declares:
 *     {x, y, z, u, v, nx, ny, nz, r, g, b, a}
 * Everything past z is optional, defaulting the way LOVE does: uv 0, normal
 * 0, colour opaque white. A model with positions alone must render as white
 * geometry, not as nothing -- calloc's transparent black would make a
 * correctly-loaded .obj look like a failed load. */
static void mesh3d_read_vertex(lua_State *S, int t, wcl_vertex3d_t *v) {
    double c[12] = {0,0,0, 0,0, 0,0,0, 1,1,1,1};
    for (int i = 0; i < 12; i++) {
        lua_rawgeti(S, t, i + 1);
        if (!lua_isnil(S, -1)) c[i] = lua_tonumber(S, -1);
        lua_pop(S, 1);
    }
    v->x = (float)c[0];  v->y = (float)c[1];  v->z = (float)c[2];
    v->u = (float)c[3];  v->v = (float)c[4];
    v->nx = (float)c[5]; v->ny = (float)c[6]; v->nz = (float)c[7];
    v->r = (float)c[8];  v->g = (float)c[9];  v->b = (float)c[10];
    v->a = (float)c[11];
}

/* mesh3d_new(verts) -> handle | nil, reason
 * `verts` is an array of vertex tables. */
static int l_mesh3d_new(lua_State *S) {
    luaL_checktype(S, 1, LUA_TTABLE);
    int n = (int)lua_rawlen(S, 1);
    if (n < 1) { lua_pushnil(S); lua_pushstring(S, "empty"); return 2; }
    if (n > MESH3D_MAX_VERTS) {
        lua_pushnil(S); lua_pushstring(S, "toobig"); return 2;
    }
    if (!wcl_r2d_active()) {
        lua_pushnil(S); lua_pushstring(S, "nogl"); return 2;
    }
    wcl_vertex3d_t *buf = mesh3d_scratch_for(n);
    if (!buf) { lua_pushnil(S); lua_pushstring(S, "oom"); return 2; }
    for (int i = 0; i < n; i++) {
        lua_rawgeti(S, 1, i + 1);
        if (lua_istable(S, -1)) mesh3d_read_vertex(S, lua_gettop(S), &buf[i]);
        else memset(&buf[i], 0, sizeof buf[i]);
        lua_pop(S, 1);
    }
    int h = wcl_r3d_mesh_new(buf, n);
    if (h < 0) { lua_pushnil(S); lua_pushstring(S, "slots"); return 2; }
    lua_pushinteger(S, h);
    return 1;
}

/* mesh3d_new_format(format, verts) -> handle | nil, reason
 *
 * `format` is an array of {name, "float"|"byte", components} and `verts` an
 * array of per-vertex tables holding the components IN FORMAT ORDER,
 * flattened. The packing happens here rather than in Lua because the target
 * is raw interleaved bytes: doing it Lua-side would mean building a string
 * per vertex.
 */
static int l_mesh3d_new_format(lua_State *S) {
    luaL_checktype(S, 1, LUA_TTABLE);
    luaL_checktype(S, 2, LUA_TTABLE);

    wcl_attrib_t attribs[WCL_MAX_ATTRIBS];
    int n_attribs = (int)lua_rawlen(S, 1);
    if (n_attribs < 1 || n_attribs > WCL_MAX_ATTRIBS) {
        lua_pushnil(S); lua_pushstring(S, "attribs"); return 2;
    }
    for (int i = 0; i < n_attribs; i++) {
        lua_rawgeti(S, 1, i + 1);
        lua_rawgeti(S, -1, 1);
        const char *nm = lua_tostring(S, -1);
        snprintf(attribs[i].name, WCL_ATTRIB_NAME_MAX, "%s", nm ? nm : "");
        lua_pop(S, 1);
        lua_rawgeti(S, -1, 2);
        const char *ty = lua_tostring(S, -1);
        attribs[i].is_byte = (ty && !strcmp(ty, "byte"));
        lua_pop(S, 1);
        lua_rawgeti(S, -1, 3);
        int comps = (int)lua_tointeger(S, -1);
        lua_pop(S, 1);
        if (comps < 1) comps = 1;
        if (comps > 4) comps = 4;
        /* A byte attribute is always 4 components in LOVE, which is what
         * makes it exactly one 32-bit word per vertex. */
        attribs[i].components = attribs[i].is_byte ? 4 : comps;
        lua_pop(S, 1);
    }

    const int stride = wcl_r3d_format_stride(attribs, n_attribs);
    const int count = (int)lua_rawlen(S, 2);
    if (count < 1) { lua_pushnil(S); lua_pushstring(S, "empty"); return 2; }
    if (!wcl_r2d_active()) { lua_pushnil(S); lua_pushstring(S, "nogl"); return 2; }

    uint8_t *buf = (uint8_t *)calloc((size_t)count * stride, 1);
    if (!buf) { lua_pushnil(S); lua_pushstring(S, "oom"); return 2; }

    for (int v = 0; v < count; v++) {
        lua_rawgeti(S, 2, v + 1);
        if (!lua_istable(S, -1)) { lua_pop(S, 1); continue; }
        uint8_t *dst = buf + (size_t)v * stride;
        int src_i = 1;                     /* 1-based index into the vertex */
        for (int a = 0; a < n_attribs; a++) {
            const wcl_attrib_t *at = &attribs[a];
            if (at->is_byte) {
                for (int c = 0; c < 4; c++) {
                    lua_rawgeti(S, -1, src_i++);
                    double d = lua_tonumber(S, -1);
                    lua_pop(S, 1);
                    /* LOVE's byte attributes arrive already in 0..255 from
                     * the packing code that produced them. */
                    if (d < 0) d = 0;
                    if (d > 255) d = 255;
                    dst[c] = (uint8_t)(d + 0.5);
                }
                dst += 4;
            } else {
                for (int c = 0; c < at->components; c++) {
                    lua_rawgeti(S, -1, src_i++);
                    float f = (float)lua_tonumber(S, -1);
                    lua_pop(S, 1);
                    memcpy(dst, &f, 4);
                    dst += 4;
                }
            }
        }
        lua_pop(S, 1);
    }

    int h = wcl_r3d_mesh_new_format(attribs, n_attribs, buf, count, stride);
    free(buf);
    if (h < 0) { lua_pushnil(S); lua_pushstring(S, "create"); return 2; }
    lua_pushinteger(S, h);
    return 1;
}

/* mesh3d_new_format_empty(format, count) - allocate the buffer, fill later
 * through mesh3d_set_bytes. */
static int l_mesh3d_new_format_empty(lua_State *S) {
    luaL_checktype(S, 1, LUA_TTABLE);
    wcl_attrib_t attribs[WCL_MAX_ATTRIBS];
    int n_attribs = (int)lua_rawlen(S, 1);
    if (n_attribs < 1 || n_attribs > WCL_MAX_ATTRIBS) {
        lua_pushnil(S); lua_pushstring(S, "attribs"); return 2;
    }
    for (int i = 0; i < n_attribs; i++) {
        lua_rawgeti(S, 1, i + 1);
        lua_rawgeti(S, -1, 1);
        const char *nm = lua_tostring(S, -1);
        snprintf(attribs[i].name, WCL_ATTRIB_NAME_MAX, "%s", nm ? nm : "");
        lua_pop(S, 1);
        lua_rawgeti(S, -1, 2);
        const char *ty = lua_tostring(S, -1);
        attribs[i].is_byte = (ty && !strcmp(ty, "byte"));
        lua_pop(S, 1);
        lua_rawgeti(S, -1, 3);
        int comps = (int)lua_tointeger(S, -1);
        lua_pop(S, 1);
        if (comps < 1) comps = 1;
        if (comps > 4) comps = 4;
        attribs[i].components = attribs[i].is_byte ? 4 : comps;
        lua_pop(S, 1);
    }
    int count = ARGI(2);
    if (count < 1) { lua_pushnil(S); lua_pushstring(S, "empty"); return 2; }
    if (!wcl_r2d_active()) { lua_pushnil(S); lua_pushstring(S, "nogl"); return 2; }

    const int stride = wcl_r3d_format_stride(attribs, n_attribs);
    uint8_t *buf = (uint8_t *)calloc((size_t)count * stride, 1);
    if (!buf) { lua_pushnil(S); lua_pushstring(S, "oom"); return 2; }
    int h = wcl_r3d_mesh_new_format(attribs, n_attribs, buf, count, stride);
    free(buf);
    if (h < 0) { lua_pushnil(S); lua_pushstring(S, "create"); return 2; }
    lua_pushinteger(S, h);
    return 1;
}

/* mesh3d_set_bytes(handle, str) - upload already-interleaved vertex bytes.
 * No marshalling: the cart packed the buffer, so it goes straight across. */
static int l_mesh3d_set_bytes(lua_State *S) {
    int handle = ARGI(1);
    size_t len = 0;
    const char *data = luaL_checklstring(S, 2, &len);
    lua_pushboolean(S, wcl_r3d_mesh_set_bytes(handle, data, (int)len));
    return 1;
}

/* mesh3d_set_vertices_format(handle, format, verts) - repack a
 * declared-format mesh's vertices in place, to ITS stride. */
static int l_mesh3d_set_vertices_format(lua_State *S) {
    int handle = ARGI(1);
    luaL_checktype(S, 2, LUA_TTABLE);
    luaL_checktype(S, 3, LUA_TTABLE);

    wcl_attrib_t attribs[WCL_MAX_ATTRIBS];
    int n_attribs = (int)lua_rawlen(S, 2);
    if (n_attribs < 1 || n_attribs > WCL_MAX_ATTRIBS) {
        lua_pushboolean(S, 0); return 1;
    }
    for (int i = 0; i < n_attribs; i++) {
        lua_rawgeti(S, 2, i + 1);
        lua_rawgeti(S, -1, 1);
        const char *nm = lua_tostring(S, -1);
        snprintf(attribs[i].name, WCL_ATTRIB_NAME_MAX, "%s", nm ? nm : "");
        lua_pop(S, 1);
        lua_rawgeti(S, -1, 2);
        const char *ty = lua_tostring(S, -1);
        attribs[i].is_byte = (ty && !strcmp(ty, "byte"));
        lua_pop(S, 1);
        lua_rawgeti(S, -1, 3);
        int comps = (int)lua_tointeger(S, -1);
        lua_pop(S, 1);
        if (comps < 1) comps = 1;
        if (comps > 4) comps = 4;
        attribs[i].components = attribs[i].is_byte ? 4 : comps;
        lua_pop(S, 1);
    }
    const int stride = wcl_r3d_format_stride(attribs, n_attribs);
    const int count = (int)lua_rawlen(S, 3);
    if (count < 1) { lua_pushboolean(S, 0); return 1; }

    uint8_t *buf = (uint8_t *)calloc((size_t)count * stride, 1);
    if (!buf) { lua_pushboolean(S, 0); return 1; }
    for (int v = 0; v < count; v++) {
        lua_rawgeti(S, 3, v + 1);
        if (!lua_istable(S, -1)) { lua_pop(S, 1); continue; }
        uint8_t *dst = buf + (size_t)v * stride;
        int src_i = 1;
        for (int a = 0; a < n_attribs; a++) {
            const wcl_attrib_t *at = &attribs[a];
            if (at->is_byte) {
                for (int c = 0; c < 4; c++) {
                    lua_rawgeti(S, -1, src_i++);
                    double d = lua_tonumber(S, -1);
                    lua_pop(S, 1);
                    if (d < 0) d = 0;
                    if (d > 255) d = 255;
                    dst[c] = (uint8_t)(d + 0.5);
                }
                dst += 4;
            } else {
                for (int c = 0; c < at->components; c++) {
                    lua_rawgeti(S, -1, src_i++);
                    float f = (float)lua_tonumber(S, -1);
                    lua_pop(S, 1);
                    memcpy(dst, &f, 4);
                    dst += 4;
                }
            }
        }
        lua_pop(S, 1);
    }
    int ok = wcl_r3d_mesh_set_bytes(handle, buf, count * stride);
    free(buf);
    lua_pushboolean(S, ok);
    return 1;
}

/* mesh3d_set_map_bytes(handle, str, width) - an index buffer that is
 * already packed. `width` is 2 or 4 bytes per index; GL wants 32-bit here,
 * so 16-bit input is widened rather than reinterpreted. */
static int l_mesh3d_set_map_bytes(lua_State *S) {
    int handle = ARGI(1);
    size_t len = 0;
    const char *data = luaL_checklstring(S, 2, &len);
    int width = OPTI(3, 4);
    if (width != 2 && width != 4) { lua_pushboolean(S, 0); return 1; }
    int n = (int)(len / (size_t)width);
    if (n < 1) { lua_pushboolean(S, 0); return 1; }
    uint32_t *idx = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
    if (!idx) { lua_pushboolean(S, 0); return 1; }
    for (int i = 0; i < n; i++) {
        if (width == 2) {
            uint16_t v;
            memcpy(&v, data + (size_t)i * 2, 2);
            idx[i] = v;
        } else {
            uint32_t v;
            memcpy(&v, data + (size_t)i * 4, 4);
            idx[i] = v;
        }
    }
    /* NOT 1-based: these indices were written by the cart against its own
     * 0-based buffer (through ffi), unlike the Lua table form which follows
     * LOVE's 1-based convention. Subtracting one here would drop the first
     * vertex of every primitive. */
    lua_pushboolean(S, wcl_r3d_mesh_set_indices(handle, idx, n));
    free(idx);
    return 1;
}

/* mesh3d_set_vertices(handle, verts) - replace geometry in place. */
static int l_mesh3d_set_vertices(lua_State *S) {
    int h = ARGI(1);
    luaL_checktype(S, 2, LUA_TTABLE);
    int n = (int)lua_rawlen(S, 2);
    if (n < 1 || n > MESH3D_MAX_VERTS) { lua_pushboolean(S, 0); return 1; }
    wcl_vertex3d_t *buf = mesh3d_scratch_for(n);
    if (!buf) { lua_pushboolean(S, 0); return 1; }
    for (int i = 0; i < n; i++) {
        lua_rawgeti(S, 2, i + 1);
        if (lua_istable(S, -1)) mesh3d_read_vertex(S, lua_gettop(S), &buf[i]);
        else memset(&buf[i], 0, sizeof buf[i]);
        lua_pop(S, 1);
    }
    lua_pushboolean(S, wcl_r3d_mesh_update(h, buf, n));
    return 1;
}

/* mesh3d_set_map(handle, indices) - an index buffer, or nil to clear. */
static int l_mesh3d_set_map(lua_State *S) {
    int h = ARGI(1);
    if (lua_isnoneornil(S, 2)) {
        lua_pushboolean(S, wcl_r3d_mesh_set_indices(h, NULL, 0));
        return 1;
    }
    luaL_checktype(S, 2, LUA_TTABLE);
    int n = (int)lua_rawlen(S, 2);
    if (n < 1) { lua_pushboolean(S, 0); return 1; }
    uint32_t *idx = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
    if (!idx) { lua_pushboolean(S, 0); return 1; }
    for (int i = 0; i < n; i++) {
        lua_rawgeti(S, 2, i + 1);
        /* LOVE's vertex maps are 1-based; GL's are 0-based. */
        lua_Integer u = lua_tointeger(S, -1) - 1;
        idx[i] = (uint32_t)(u < 0 ? 0 : u);
        lua_pop(S, 1);
    }
    lua_pushboolean(S, wcl_r3d_mesh_set_indices(h, idx, n));
    free(idx);
    return 1;
}

static int l_mesh3d_set_texture(lua_State *S) {
    int h = ARGI(1);
    if (lua_isnoneornil(S, 2)) {
        lua_pushboolean(S, wcl_r3d_mesh_set_texture(h, NULL, 0, 0));
        return 1;
    }
    image_t *im = image_by_id(ARGI(2));
    if (!im || !im->rgba) { lua_pushboolean(S, 0); return 1; }
    lua_pushboolean(S, wcl_r3d_mesh_set_texture(h, im->rgba, im->w, im->h));
    return 1;
}

static int l_mesh3d_draw(lua_State *S) {
    int ok = wcl_r3d_mesh_draw(ARGI(1), cur_rgb(), cur_a);
    if (ok) dbg_draw_calls++;
    lua_pushboolean(S, ok);
    return 1;
}

static int l_mesh3d_release(lua_State *S) {
    wcl_r3d_mesh_free(ARGI(1));
    return 0;
}

/* ── GPU render targets ────────────────────────────────────────────────
 *
 * The prelude passes formats and texture types as STRINGS, exactly as LOVE
 * spells them, and they are mapped to the enum here rather than in Lua so
 * the name->enum table lives beside the table that implements them.
 */
static const char *FMT_NAMES[] = {
    "rgba8", "r8", "rg8", "r16f", "rg16f", "rgba16f", "r32f", "rgba32f",
    "depth16", "depth24", "depth32f", "depth24stencil8", NULL
};

static int fmt_from_name(const char *s) {
    if (!s) return WCL_FMT_RGBA8;
    /* LOVE spells the default "normal"; several of its aliases mean rgba8. */
    if (!strcmp(s, "normal") || !strcmp(s, "rgba8") || !strcmp(s, "srgba8"))
        return WCL_FMT_RGBA8;
    if (!strcmp(s, "depth24stencil8") || !strcmp(s, "depth24_stencil8"))
        return WCL_FMT_DEPTH24_STENCIL8;
    for (int i = 0; FMT_NAMES[i]; i++)
        if (!strcmp(s, FMT_NAMES[i])) return i;
    return -1;
}

static int textype_from_name(const char *s) {
    if (!s || !strcmp(s, "2d")) return WCL_TEX_2D;
    if (!strcmp(s, "cube"))   return WCL_TEX_CUBE;
    if (!strcmp(s, "array"))  return WCL_TEX_ARRAY;
    if (!strcmp(s, "volume")) return WCL_TEX_VOLUME;
    return -1;
}

/* target_new(w, h, format, type, layers, mipmaps, msaa) -> handle | nil, why */
static int l_target_new(lua_State *S) {
    int w = ARGI(1), h = ARGI(2);
    const char *fname = lua_isnoneornil(S, 3) ? NULL : luaL_checkstring(S, 3);
    const char *tname = lua_isnoneornil(S, 4) ? NULL : luaL_checkstring(S, 4);
    int fmt = fmt_from_name(fname);
    int type = textype_from_name(tname);
    if (fmt < 0) { lua_pushnil(S); lua_pushstring(S, "format"); return 2; }
    if (type < 0) { lua_pushnil(S); lua_pushstring(S, "type"); return 2; }
    if (!wcl_r2d_active()) { lua_pushnil(S); lua_pushstring(S, "nogl"); return 2; }
    int h2 = wcl_r3d_target_new(w, h, (wcl_format_t)fmt, (wcl_textype_t)type,
                                OPTI(5, 1), lua_toboolean(S, 6), OPTI(7, 0));
    if (h2 < 0) { lua_pushnil(S); lua_pushstring(S, "create"); return 2; }
    lua_pushinteger(S, h2);
    return 1;
}

static int l_target_free(lua_State *S) {
    wcl_r3d_target_free(ARGI(1));
    return 0;
}

static int l_target_supported(lua_State *S) {
    int fmt = fmt_from_name(luaL_checkstring(S, 1));
    lua_pushboolean(S, fmt >= 0 && wcl_r3d_format_supported((wcl_format_t)fmt));
    return 1;
}

static int l_target_mipmaps(lua_State *S) {
    wcl_r3d_target_generate_mipmaps(ARGI(1));
    return 0;
}

/* target_bind(handles, layers, depth, depth_layer)
 * `handles` is an array of target handles (or nil/empty for the screen). */
static int l_target_bind(lua_State *S) {
    int handles[8], layers[8];
    int n = 0;
    if (lua_istable(S, 1)) {
        n = (int)lua_rawlen(S, 1);
        if (n > 8) n = 8;
        for (int i = 0; i < n; i++) {
            lua_rawgeti(S, 1, i + 1);
            handles[i] = (int)lua_tointeger(S, -1);
            lua_pop(S, 1);
            layers[i] = 0;
            if (lua_istable(S, 2)) {
                lua_rawgeti(S, 2, i + 1);
                layers[i] = (int)lua_tointeger(S, -1);
                lua_pop(S, 1);
            }
        }
    }
    int depth = lua_isnoneornil(S, 3) ? -1 : (int)lua_tointeger(S, 3);
    int dlayer = OPTI(4, 0);
    lua_pushboolean(S, wcl_r3d_target_bind(handles, layers, n, depth, dlayer));
    return 1;
}

static int l_target_send(lua_State *S) {
    lua_pushboolean(S, wcl_r3d_target_send(ARGI(1), luaL_checkstring(S, 2),
                                           ARGI(3), ARGI(4)));
    return 1;
}

/* image3d_new(w, h, type, layers, mipmaps) -> handle | nil */
static int l_image3d_new(lua_State *S) {
    int type = textype_from_name(lua_isnoneornil(S, 3) ? NULL : luaL_checkstring(S, 3));
    if (type < 0) { lua_pushnil(S); lua_pushstring(S, "type"); return 2; }
    if (!wcl_r2d_active()) { lua_pushnil(S); lua_pushstring(S, "nogl"); return 2; }
    int h = wcl_r3d_image_new(ARGI(1), ARGI(2), (wcl_textype_t)type,
                              OPTI(4, 1), lua_toboolean(S, 5));
    if (h < 0) { lua_pushnil(S); lua_pushstring(S, "create"); return 2; }
    lua_pushinteger(S, h);
    return 1;
}

/* image3d_upload_from(handle, layer, imageId) - copies an already-decoded
 * 2D image's pixels into one face/layer. Going through the existing image
 * loader means cube faces get the same PNG decoder, the same asset lookup
 * and the same error reporting as every other texture in the cart. */
static int l_image3d_upload_from(lua_State *S) {
    int handle = ARGI(1), layer = ARGI(2);
    image_t *im = image_by_id(ARGI(3));
    if (!im || !im->rgba) { lua_pushboolean(S, 0); return 1; }
    lua_pushboolean(S, wcl_r3d_image_upload(handle, layer, im->rgba, im->w, im->h));
    return 1;
}

static int l_image3d_finish(lua_State *S) {
    wcl_r3d_image_finish(ARGI(1));
    return 0;
}

static int l_image3d_wrap(lua_State *S) {
    wcl_r3d_image_wrap(ARGI(1), lua_toboolean(S, 2), lua_toboolean(S, 3),
                       lua_toboolean(S, 4));
    return 0;
}

static int l_image3d_filter(lua_State *S) {
    wcl_r3d_image_filter(ARGI(1), lua_toboolean(S, 2));
    return 0;
}

static int l_color_mask(lua_State *S) {
    wcl_r3d_color_mask(lua_toboolean(S, 1), lua_toboolean(S, 2),
                       lua_toboolean(S, 3), lua_toboolean(S, 4));
    return 0;
}

static int l_get_color_mask(lua_State *S) {
    int r, g, b, a;
    wcl_r3d_get_color_mask(&r, &g, &b, &a);
    lua_pushboolean(S, r); lua_pushboolean(S, g);
    lua_pushboolean(S, b); lua_pushboolean(S, a);
    return 4;
}

static int l_gl_limit(lua_State *S) {
    lua_pushinteger(S, wcl_r3d_limit(ARGI(1)));
    return 1;
}

static int l_mesh3d_draw_instanced(lua_State *S) {
    int ok = wcl_r3d_mesh_draw_instanced(ARGI(1), ARGI(2), cur_rgb(), cur_a);
    if (ok) dbg_draw_calls++;
    lua_pushboolean(S, ok);
    return 1;
}

/* depth_mode(compare, write) - compare is a GL enum, 0 to disable. */
static int l_depth_mode(lua_State *S) {
    wcl_r3d_depth_mode((uint32_t)luaL_checkinteger(S, 1), lua_toboolean(S, 2));
    return 0;
}

static int l_get_depth_mode(lua_State *S) {
    uint32_t c = 0; int w = 0;
    wcl_r3d_get_depth_mode(&c, &w);
    lua_pushinteger(S, (lua_Integer)c);
    lua_pushboolean(S, w);
    return 2;
}

static int l_cull_mode(lua_State *S) {
    wcl_r3d_cull_mode(ARGI(1));
    return 0;
}
static int l_get_cull_mode(lua_State *S) {
    lua_pushinteger(S, wcl_r3d_get_cull_mode());
    return 1;
}
static int l_front_face(lua_State *S) {
    wcl_r3d_front_face(lua_toboolean(S, 1));
    return 0;
}
static int l_get_front_face(lua_State *S) {
    lua_pushboolean(S, wcl_r3d_get_front_face());
    return 1;
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
    {"image_pixel", l_image_pixel},
    {"image_blank", l_image_blank},
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
    {"gpu2d",       l_gpu2d},
    {"debug_set",   l_debug_set},
    {"rand",        l_rand},
    {"save_write",  l_save_write},
    {"save_read",   l_save_read},
    {"asset_read",  l_asset_read},
    {"asset_exists", l_asset_exists},
    {"pointer",     l_pointer},
    {"shader_new",  l_shader_new},
    {"shader_use",  l_shader_use},
    {"shader_send", l_shader_send},
    {"shader_send_image", l_shader_send_image},
    {"shader_send_bool",  l_shader_send_bool},
    {"mesh_new",        l_mesh_new},
    {"mesh_set_vertex", l_mesh_set_vertex},
    {"mesh_get_vertex", l_mesh_get_vertex},
    {"mesh_set_texture", l_mesh_set_texture},
    {"mesh_set_map",    l_mesh_set_map},
    {"mesh_get_map",    l_mesh_get_map},
    {"mesh_set_range",  l_mesh_set_range},
    {"mesh_get_range",  l_mesh_get_range},
    {"mesh_draw",       l_mesh_draw},
    {"mesh_release",    l_mesh_release},
    {"mesh3d_new",          l_mesh3d_new},
    {"mesh3d_new_format",   l_mesh3d_new_format},
    {"mesh3d_new_format_empty", l_mesh3d_new_format_empty},
    {"mesh3d_set_bytes",    l_mesh3d_set_bytes},
    {"mesh3d_set_map_bytes", l_mesh3d_set_map_bytes},
    {"mesh3d_set_vertices_format", l_mesh3d_set_vertices_format},
    {"mesh3d_set_vertices", l_mesh3d_set_vertices},
    {"mesh3d_set_map",      l_mesh3d_set_map},
    {"mesh3d_set_texture",  l_mesh3d_set_texture},
    {"mesh3d_draw",         l_mesh3d_draw},
    {"mesh3d_release",      l_mesh3d_release},
    {"mesh3d_draw_instanced", l_mesh3d_draw_instanced},
    {"target_new",      l_target_new},
    {"target_free",     l_target_free},
    {"target_supported", l_target_supported},
    {"target_mipmaps",  l_target_mipmaps},
    {"target_bind",     l_target_bind},
    {"target_send",     l_target_send},
    {"image3d_new",         l_image3d_new},
    {"image3d_upload_from", l_image3d_upload_from},
    {"image3d_finish",      l_image3d_finish},
    {"image3d_wrap",        l_image3d_wrap},
    {"image3d_filter",      l_image3d_filter},
    {"color_mask",      l_color_mask},
    {"get_color_mask",  l_get_color_mask},
    {"gl_limit",        l_gl_limit},
    {"depth_mode",      l_depth_mode},
    {"get_depth_mode",  l_get_depth_mode},
    {"cull_mode",       l_cull_mode},
    {"get_cull_mode",   l_get_cull_mode},
    {"front_face",      l_front_face},
    {"get_front_face",  l_get_front_face},
    {"shader_has_uniform", l_shader_has_uniform},
    {"pad_has_rumble",  l_pad_has_rumble},
    {"pad_rumble",      l_pad_rumble},
    {"pad_rumble_stop", l_pad_rumble_stop},
    {"peer_open",       l_peer_open},
    {"peer_close",      l_peer_close},
    {"peer_send",       l_peer_send},
    {"peer_broadcast",  l_peer_broadcast},
    {"peer_state",      l_peer_state},
    {"peer_count",      l_peer_count},
    {"peer_id",         l_peer_id},
    {"peer_name",       l_peer_name},
    {"peer_transport",  l_peer_transport},
    {"peer_poll",       l_peer_poll},
    {"peer_dropped",    l_peer_dropped},
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

/* conf.lua: the cart picks its resolution, LOVE's way.
 *
 * Runs BEFORE the prelude (which captures __WC_WIDTH/__WC_HEIGHT into locals
 * and nils them) and before the renderer initializes, so everything downstream
 * sees the chosen size. The asset is optional; so is defining love.conf inside
 * it. Only t.window.width/height are honored, clamped to the MAX_* buffers --
 * the engine's buffers are statically sized, so this is a choice of extent,
 * never an allocation. */
static void apply_conf(void) {
    if (wc_asset_size("conf.lua", 8) <= 0) return;
    /* LOVE's conf.lua writes `function love.conf(t)`, so `love` must exist
     * before it runs. The prelude later does `love = love or {}`, keeping
     * whatever conf.lua defined. */
    lua_getglobal(L, "love");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_setglobal(L, "love");
    } else {
        lua_pop(L, 1);
    }
    if (run_source("conf.lua") != 0) return;
    static const char *ASK =
        "local t = { window = { width = ..., height = select(2, ...) } }\n"
        "if type(love) == 'table' and type(love.conf) == 'function' then\n"
        "  love.conf(t)\n"
        "end\n"
        "return t.window and t.window.width, t.window and t.window.height\n";
    int st = luaL_loadbuffer(L, ASK, strlen(ASK), "conf-query");
    if (st == LUA_OK) {
        lua_pushinteger(L, scr_w);
        lua_pushinteger(L, scr_h);
        st = lua_pcall(L, 2, 2, 0);
    }
    if (lua_guard("love.conf", st)) return;
    int w = (int)lua_tointeger(L, -2);
    int h = (int)lua_tointeger(L, -1);
    lua_pop(L, 2);
    if (w >= 1 && h >= 1) {
        scr_w = w > MAX_WIDTH  ? MAX_WIDTH  : w;
        scr_h = h > MAX_HEIGHT ? MAX_HEIGHT : h;
        /* Stamp the live struct too: hosts call wc_get_info() BEFORE wc_init
         * and then re-read the struct MEMORY afterward, so an on-call override
         * inside wc_get_info alone would never reach them. */
        wc_info.width  = (uint32_t)scr_w;
        wc_info.height = (uint32_t)scr_h;
    }
}

WC_EXPORT wc_info_t *wc_get_info(void) {
    /* WC_FLAG_NET_PEER is the cart-side half of the networking gate. The
     * other half is the manifest's `net` grant, which the packager writes -
     * a cart that never calls love.net simply never reaches a host that has
     * not been given a domain to allow. */
    WC_FILL_INFO(WC_FLAG_DEBUG | WC_FLAG_DETERMINISTIC | WC_FLAG_POINTER |
                 WC_FLAG_NET_PEER);
    /* WC_FILL_INFO stamps the compile-time DEFAULTs; the cart's conf.lua may
     * have chosen otherwise. The host re-reads this struct after wc_init
     * precisely so a boot-time resolution choice can land. */
    wc_info.width  = (uint32_t)scr_w;
    wc_info.height = (uint32_t)scr_h;
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
    wcl_open_physics3d(L);

    /* the cart's resolution choice, before anything captures the dims */
    apply_conf();

    /* screen dims for the prelude */
    lua_pushinteger(L, scr_w);  lua_setglobal(L, "__WC_WIDTH");
    lua_pushinteger(L, scr_h);  lua_setglobal(L, "__WC_HEIGHT");

    int prelude_ok;
    if (wc_asset_size("prelude.lua", 11) > 0) {
        prelude_ok = run_source("prelude.lua") == 0;
    } else {
        int st = luaL_loadbuffer(L, (const char *)WCL_PRELUDE, WCL_PRELUDE_LEN, "prelude");
        if (st == LUA_OK) st = lua_pcall(L, 0, 0, 0);
        prelude_ok = !lua_guard("prelude(embedded)", st);
    }
    if (!prelude_ok) return;

#ifdef WCL_ENABLE_GL2D
    /* Before love.load, not on the first wc_render. Carts routinely prepare
     * art in a canvas during load; with the backend not yet initialized
     * wcl_r2d_target failed, tripped the sticky fallback, and the cart ran
     * on the software rasterizer for its whole life. */
    wcl_r2d_init(scr_w, scr_h);
#endif

    if (run_source("main.lua") != 0) return;

    /* love.load() */
    lua_getglobal(L, "__wasmcart_load");
    if (lua_isfunction(L, -1)) lua_guard("love.load", lua_pcall(L, 0, 0, 0));
    else lua_pop(L, 1);
}

WC_EXPORT_RENDER void wc_render(void) {
    dbg_draw_calls = 0;

#ifdef WCL_ENABLE_GL2D
    wcl_r2d_init(scr_w, scr_h);   /* no-op after the first */
    /* Returns 0 in sticky cpu_mode, in which case every draw below takes the
     * software path and wcl_r2d_end blits the finished framebuffer. */
    if (wcl_r2d_begin(frame_clear_color)) {
        /* the prelude resets sc_on per frame; keep GL's state in step */
        if (!sc_on) wcl_r2d_scissor(0, 0, -1, -1);
        else wcl_r2d_scissor(sc_x, sc_y, sc_w, sc_h);
        /* Clear last frame's depths. wcl_r2d_begin's glClear covers colour
         * only, so without this every frame after the first would test
         * against the previous frame's depth buffer and geometry would
         * vanish wherever the old frame happened to be nearer -- which
         * reads as flickering holes in a model, not as a depth bug. */
        wcl_r3d_frame_begin();
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
        for (int i = 0; i < scr_w * scr_h; i++)
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
    /* Latch the GPU state for the frame that just ended. Read by the host
     * and by test/gpuonly -- a 0 here means something refused a primitive
     * and every subsequent frame is on the CPU. */
    dbg_gpu2d = wcl_r2d_active() ? 1u : 0u;
    wcl_r2d_end(wc_framebuffer);
#elif defined(WCL_USE_GL)
    gl_present();
#endif
}
