/*
 * render2d_gl.c - GL 2D backend for the Lua engine.
 *
 * Modelled on wasmcart-mruby's runtime/render2d_gl.c, which shipped this
 * approach first. Two decisions are inherited from it deliberately:
 *
 *  1. Fixed-function blending, and therefore NOT bit-exact. The GPU blends
 *     in normalized floats and rounds at 8 bit; the software rasterizer uses
 *     the exact div255 multiply-shift. Those disagree on 39.7% of
 *     (alpha, src, dst) combinations, always by exactly 1. Reproducing
 *     div255 on the GPU would need a destination read per draw
 *     (framebuffer_fetch or a ping-pong FBO), which is a different engine.
 *     +/-1 is accepted here; the software path remains the reference
 *     implementation and stays bit-exact.
 *
 *  2. The CPU fallback is WHOLE-FRAME and STICKY. Anything this backend does
 *     not implement (rotation, canvas render targets, scissor, additive
 *     blending, TTF text, polygons, circles) calls wcl_r2d_disable(), and
 *     from then on every frame is rasterized in software and presented with
 *     one wc_gl_blit. Mixing per-draw would mean reconciling the GL
 *     framebuffer against wc_framebuffer on every switch (~0.19 ms measured),
 *     which a real cart would pay dozens of times per frame.
 */
#include "render2d_gl.h"

#ifdef WCL_ENABLE_GL2D

#define WC_USE_GL
#include "wasmcart.h"
#define WC_GL_BLIT_IMPLEMENTATION
#include "wc_gl_blit.h"
#include <string.h>

#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_NEAREST 0x2600
#define GL_UNSIGNED_BYTE 0x1401
#define GL_RGBA 0x1908
#define GL_BLEND 0x0BE2
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_STATIC_DRAW 0x88E4
#define GL_FLOAT 0x1406
#define GL_TRIANGLES 0x0004
#define GL_UNSIGNED_SHORT 0x1403
#define GL_LINES 0x0001
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_SCISSOR_TEST 0x0C11

typedef struct { float x, y, u, v, r, g, b, a; } vertex_t;
typedef struct {
    const void *pixels;   /* cache key: the image's RGBA payload pointer */
    int w, h;
    int atlas_x, atlas_y;
    int used;
} texture_t;

wcl_r2d_stats_t wcl_r2d_stats;

static int ready;
static int frame_disabled;
/* Sticky: set on the first wcl_r2d_disable. From then on every frame
 * CPU-rasterizes into the cart framebuffer and wcl_r2d_end blits it as a
 * fullscreen quad - a GL host would otherwise present only the clear colour
 * and lose everything the software path drew. Carts that never touch an
 * unsupported feature keep the GL fast path for the whole run. */
static int cpu_mode;
static int width, height;
static float ndc_scale_x, ndc_scale_y;
static GLuint program, vao, buffer, index_buffer;
static GLint pos_attr, uv_attr, color_attr, tex_uniform, textured_uniform;

#define MAX_TEXTURES 64
static texture_t textures[MAX_TEXTURES];
#define ATLAS_SIZE 2048
static GLuint atlas_texture;
static int atlas_x, atlas_y, atlas_row_h;

#define BATCH_MAX 4096
static vertex_t solid_batch[BATCH_MAX * 4];
static int solid_batch_count;
static int solid_batch_has_alpha;
static vertex_t textured_batch[BATCH_MAX * 4];
static int textured_batch_count;
static uint16_t indices[BATCH_MAX * 6];

static int scissor_on;
static int blend_enabled = -1;
static int textured_enabled = -1;
static GLuint bound_texture;
static uint32_t current_clear_color = 0xFFFFFFFFu;
static wcl_r2d_stats_t frame_stats;

static const char *VERTEX_SHADER =
    "#version 300 es\n"
    "in vec2 a_pos;\n"
    "in vec2 a_uv;\n"
    "in vec4 a_color;\n"
    "out vec2 v_uv;\n"
    "out vec4 v_color;\n"
    "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); v_uv = a_uv; v_color = a_color; }\n";

static const char *FRAGMENT_SHADER =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 v_uv;\n"
    "in vec4 v_color;\n"
    "out vec4 frag_color;\n"
    "uniform sampler2D u_tex;\n"
    "uniform int u_textured;\n"
    "void main() {\n"
    "  frag_color = v_color;\n"
    "  if (u_textured != 0) frag_color *= texture(u_tex, v_uv);\n"
    "}\n";

static void log_obj(GLuint object, const char *what, int shader) {
    GLint ok = 0, len = 0;
    if (shader) glGetShaderiv(object, GL_COMPILE_STATUS, &ok);
    else glGetProgramiv(object, GL_LINK_STATUS, &ok);
    if (ok) return;
    if (shader) glGetShaderiv(object, GL_INFO_LOG_LENGTH, &len);
    else glGetProgramiv(object, GL_INFO_LOG_LENGTH, &len);
    if (len > 0 && len < 512) {
        char log[512];
        if (shader) glGetShaderInfoLog(object, sizeof log, &len, log);
        else glGetProgramInfoLog(object, sizeof log, &len, log);
        wc_log(log, (unsigned int)len);
    } else {
        wc_log(what, (unsigned int)strlen(what));
    }
}

static GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    GLint length = (GLint)strlen(source);
    glShaderSource(shader, 1, &source, &length);
    glCompileShader(shader);
    log_obj(shader, "gl2d shader compilation failed", 1);
    return shader;
}

/* Cart pixels -> clip space. y is flipped because the cart's origin is
 * top-left and GL's is bottom-left. */
static void ndc(float x, float y, float *out_x, float *out_y) {
    *out_x = x * ndc_scale_x - 1.0f;
    *out_y = 1.0f - y * ndc_scale_y;
}

static void set_blend(int enabled) {
    if (blend_enabled == enabled) return;
    if (enabled) glEnable(GL_BLEND);
    else glDisable(GL_BLEND);
    blend_enabled = enabled;
}

static void set_textured(int enabled) {
    if (textured_enabled == enabled) return;
    glUniform1i(textured_uniform, enabled);
    textured_enabled = enabled;
}

static void bind_texture(GLuint texture) {
    if (bound_texture == texture) return;
    glBindTexture(GL_TEXTURE_2D, texture);
    bound_texture = texture;
}

static void flush_solid_batch(void) {
    if (!solid_batch_count) return;
    set_blend(solid_batch_has_alpha);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(vertex_t) * solid_batch_count),
                 solid_batch, GL_DYNAMIC_DRAW);
    set_textured(0);
    glDrawElements(GL_TRIANGLES, (GLsizei)((solid_batch_count / 4) * 6),
                   GL_UNSIGNED_SHORT, (const void *)0);
    frame_stats.draws++;
    frame_stats.solid_flushes++;
    frame_stats.upload_bytes += (uint32_t)(sizeof(vertex_t) * solid_batch_count);
    solid_batch_count = 0;
    solid_batch_has_alpha = 0;
}

static void flush_textured_batch(void) {
    if (!textured_batch_count) return;
    set_blend(1);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(vertex_t) * textured_batch_count),
                 textured_batch, GL_DYNAMIC_DRAW);
    set_textured(1);
    bind_texture(atlas_texture);
    glDrawElements(GL_TRIANGLES, (GLsizei)((textured_batch_count / 4) * 6),
                   GL_UNSIGNED_SHORT, (const void *)0);
    frame_stats.draws++;
    frame_stats.tex_flushes++;
    frame_stats.upload_bytes += (uint32_t)(sizeof(vertex_t) * textured_batch_count);
    textured_batch_count = 0;
}

/* Solids and sprites share one vertex buffer, so whichever batch is open has
 * to be flushed before the other starts, or draw order breaks. */
static void flush_batches(void) {
    flush_solid_batch();
    flush_textured_batch();
}

int wcl_r2d_init(int w, int h) {
    if (ready) return 1;
    width = w; height = h;
    ndc_scale_x = 2.0f / (float)w;
    ndc_scale_y = 2.0f / (float)h;

    GLuint vs = compile_shader(GL_VERTEX_SHADER, VERTEX_SHADER);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, FRAGMENT_SHADER);
    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    log_obj(program, "gl2d program link failed", 0);
    glUseProgram(program);

    pos_attr = glGetAttribLocation(program, "a_pos");
    uv_attr = glGetAttribLocation(program, "a_uv");
    color_attr = glGetAttribLocation(program, "a_color");
    tex_uniform = glGetUniformLocation(program, "u_tex");
    textured_uniform = glGetUniformLocation(program, "u_textured");

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glEnableVertexAttribArray(pos_attr);
    glVertexAttribPointer(pos_attr, 2, GL_FLOAT, 0, sizeof(vertex_t), (const void *)0);
    glEnableVertexAttribArray(uv_attr);
    glVertexAttribPointer(uv_attr, 2, GL_FLOAT, 0, sizeof(vertex_t), (const void *)8);
    glEnableVertexAttribArray(color_attr);
    glVertexAttribPointer(color_attr, 4, GL_FLOAT, 0, sizeof(vertex_t), (const void *)16);

    /* Quad indices are constant for every batch, so upload them once. */
    for (int i = 0; i < BATCH_MAX; i++) {
        uint16_t b = (uint16_t)(i * 4);
        indices[i * 6 + 0] = b;     indices[i * 6 + 1] = b + 1; indices[i * 6 + 2] = b + 2;
        indices[i * 6 + 3] = b;     indices[i * 6 + 4] = b + 2; indices[i * 6 + 5] = b + 3;
    }
    glGenBuffers(1, &index_buffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, index_buffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)sizeof(indices), indices, GL_STATIC_DRAW);

    /* One shared atlas: every sprite lives in it, so a whole frame of
     * sprites is one draw call with no texture rebinds. NEAREST matches the
     * software rasterizer's point sampling. */
    glGenTextures(1, &atlas_texture);
    glBindTexture(GL_TEXTURE_2D, atlas_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ATLAS_SIZE, ATLAS_SIZE, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, (const void *)0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    bound_texture = atlas_texture;
    glUniform1i(tex_uniform, 0);

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    blend_enabled = -1;
    textured_enabled = -1;
    ready = 1;
    return 1;
}

int wcl_r2d_begin(uint32_t clear_color) {
    if (!ready) return 0;
    solid_batch_count = 0;
    textured_batch_count = 0;
    solid_batch_has_alpha = 0;
    memset(&frame_stats, 0, sizeof(frame_stats));
    if (cpu_mode) {
        /* caller clears + CPU-rasterizes the framebuffer; end() blits it */
        frame_disabled = 1;
        return 0;
    }
    frame_disabled = 0;
    if (clear_color != current_clear_color) {
        glClearColor((float)((clear_color >> 16) & 255) / 255.0f,
                     (float)((clear_color >> 8) & 255) / 255.0f,
                     (float)(clear_color & 255) / 255.0f, 1.0f);
        current_clear_color = clear_color;
    }
    glClear(GL_COLOR_BUFFER_BIT);
    return 1;
}

/* XRGB u32 framebuffer -> RGBA bytes for the fallback blit texture. */
static uint32_t blit_rgba[1280 * 720];

void wcl_r2d_end(const uint32_t *fb) {
    if (wcl_r2d_active()) {
        flush_batches();
    } else if (ready && frame_disabled && fb) {
        int n = width * height;
        for (int i = 0; i < n; i++) {
            uint32_t px = fb[i];
            blit_rgba[i] = 0xFF000000u | ((px >> 16) & 0xFFu) |
                           (px & 0x0000FF00u) | ((px & 0xFFu) << 16);
        }
        wc_gl_blit(blit_rgba, width, height);
        /* wc_gl_blit leaves its own program/texture/VAO bound. */
        bound_texture = 0;
        textured_enabled = -1;
        blend_enabled = -1;
    }
    wcl_r2d_stats = frame_stats;
}

void wcl_r2d_disable(void) {
    if (wcl_r2d_active()) flush_batches();
    frame_disabled = 1;
    cpu_mode = 1;
}

int wcl_r2d_active(void) { return ready && !frame_disabled; }

int wcl_r2d_solid(int x, int y, int w, int h, uint32_t color, int alpha) {
    if (!wcl_r2d_active() || w <= 0 || h <= 0) return 0;
    if (textured_batch_count) flush_textured_batch();
    if (solid_batch_count + 4 > BATCH_MAX * 4) flush_solid_batch();
    if (alpha < 255) solid_batch_has_alpha = 1;
    vertex_t *v = &solid_batch[solid_batch_count];
    float x0, y0, x1, y1;
    float r = (float)((color >> 16) & 255) / 255.0f;
    float g = (float)((color >> 8) & 255) / 255.0f;
    float b = (float)(color & 255) / 255.0f;
    float a = (float)alpha / 255.0f;
    ndc((float)x, (float)y, &x0, &y0);
    ndc((float)(x + w), (float)(y + h), &x1, &y1);
    v[0].x = x0; v[0].y = y0; v[1].x = x1; v[1].y = y0;
    v[2].x = x1; v[2].y = y1; v[3].x = x0; v[3].y = y1;
    for (int i = 0; i < 4; i++) {
        v[i].u = v[i].v = 0;
        v[i].r = r; v[i].g = g; v[i].b = b; v[i].a = a;
    }
    solid_batch_count += 4;
    frame_stats.quads++;
    return 1;
}

int wcl_r2d_line(int x0, int y0, int x1, int y1, uint32_t color, int alpha) {
    if (!wcl_r2d_active()) return 0;
    flush_batches();
    set_blend(1);
    vertex_t v[2];
    /* +0.5 puts the endpoints at pixel centres. GL_LINES is half-open -- the
     * final pixel is NOT drawn -- while Bresenham draws both endpoints, so
     * the end has to be extended by one pixel along the line's direction or
     * every line comes up one pixel short. */
    float ex = (float)x1 + 0.5f, ey = (float)y1 + 0.5f;
    {
        int ddx = x1 - x0, ddy = y1 - y0;
        int ax = ddx < 0 ? -ddx : ddx, ay = ddy < 0 ? -ddy : ddy;
        if (ax || ay) {
            /* step one more pixel along the major axis, matching Bresenham */
            float m = (float)(ax > ay ? ax : ay);
            ex += (float)ddx / m;
            ey += (float)ddy / m;
        }
    }
    ndc((float)x0 + 0.5f, (float)y0 + 0.5f, &v[0].x, &v[0].y);
    ndc(ex, ey, &v[1].x, &v[1].y);
    float r = (float)((color >> 16) & 255) / 255.0f;
    float g = (float)((color >> 8) & 255) / 255.0f;
    float b = (float)(color & 255) / 255.0f;
    float a = (float)alpha / 255.0f;
    for (int i = 0; i < 2; i++) {
        v[i].u = v[i].v = 0;
        v[i].r = r; v[i].g = g; v[i].b = b; v[i].a = a;
    }
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(v), v, GL_DYNAMIC_DRAW);
    set_textured(0);
    glDrawArrays(GL_LINES, 0, 2);
    frame_stats.draws++;
    return 1;
}

/* Look up (or allocate) the atlas slot for an image. The RGBA payload
 * pointer is the key: image_t keeps it for the image's lifetime, so an
 * image uploads once no matter how often it is drawn. */
static texture_t *get_texture(const void *pixels, int w, int h) {
    for (int i = 0; i < MAX_TEXTURES; i++)
        if (textures[i].used && textures[i].pixels == pixels) return &textures[i];
    for (int i = 0; i < MAX_TEXTURES; i++) {
        if (textures[i].used) continue;
        texture_t *t = &textures[i];
        if (w > ATLAS_SIZE || h > ATLAS_SIZE) return NULL;
        if (atlas_x + w > ATLAS_SIZE) {
            atlas_x = 0;
            atlas_y += atlas_row_h;
            atlas_row_h = 0;
        }
        if (atlas_y + h > ATLAS_SIZE) return NULL;   /* atlas full */
        t->pixels = pixels; t->w = w; t->h = h;
        t->atlas_x = atlas_x; t->atlas_y = atlas_y; t->used = 1;
        /* An upload has to land in the atlas, not whatever a batch left
         * bound, and it must not be interleaved with pending vertices that
         * still reference the old contents. */
        flush_batches();
        bind_texture(atlas_texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, atlas_x, atlas_y, w, h,
                        GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        atlas_x += w;
        if (h > atlas_row_h) atlas_row_h = h;
        return t;
    }
    return NULL;
}

int wcl_r2d_sprite(const void *pixels, int sw, int sh,
                   const double *cx, const double *cy,
                   int sx, int sy, int srcw, int srch,
                   uint32_t tint, int alpha) {
    if (!wcl_r2d_active() || !pixels || !cx || !cy) return 0;
    texture_t *t = get_texture(pixels, sw, sh);
    if (!t) return 0;                 /* atlas full: caller falls back */
    if (solid_batch_count) flush_solid_batch();
    if (textured_batch_count + 4 > BATCH_MAX * 4) flush_textured_batch();

    vertex_t *v = &textured_batch[textured_batch_count];
    /* Source rect in atlas UV. The atlas is NEAREST-filtered, so an
     * axis-aligned quad at an integer scale samples the same texels the
     * software path indexes; rotation and fractional scales can differ by a
     * texel at boundaries, which is inside the accepted tolerance. */
    float u0 = (float)(t->atlas_x + sx) / (float)ATLAS_SIZE;
    float v0 = (float)(t->atlas_y + sy) / (float)ATLAS_SIZE;
    float u1 = (float)(t->atlas_x + sx + srcw) / (float)ATLAS_SIZE;
    float v1 = (float)(t->atlas_y + sy + srch) / (float)ATLAS_SIZE;
    const float us[4] = { u0, u1, u1, u0 };
    const float vs[4] = { v0, v0, v1, v1 };

    float r = (float)((tint >> 16) & 255) / 255.0f;
    float g = (float)((tint >> 8) & 255) / 255.0f;
    float b = (float)(tint & 255) / 255.0f;
    float a = (float)alpha / 255.0f;

    for (int i = 0; i < 4; i++) {
        ndc((float)cx[i], (float)cy[i], &v[i].x, &v[i].y);
        v[i].u = us[i]; v[i].v = vs[i];
        v[i].r = r; v[i].g = g; v[i].b = b; v[i].a = a;
    }
    textured_batch_count += 4;
    frame_stats.quads++;
    return 1;
}

/* Scissor. glScissor's origin is bottom-left, the cart's is top-left, so the
 * y coordinate is flipped here rather than at every call site. A batch built
 * under one scissor must not be drawn under another, so flush first. */
void wcl_r2d_scissor(int x, int y, int w, int h) {
    if (!wcl_r2d_active()) return;
    flush_batches();
    if (w < 0) {
        if (scissor_on) { glDisable(GL_SCISSOR_TEST); scissor_on = 0; }
        return;
    }
    if (!scissor_on) { glEnable(GL_SCISSOR_TEST); scissor_on = 1; }
    glScissor(x, height - (y + h), w, h);
}

#endif /* WCL_ENABLE_GL2D */
