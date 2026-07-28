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
#define GL_R8 0x8229
#define GL_RED 0x1903
#define GL_TEXTURE_SWIZZLE_R 0x8E42
#define GL_TEXTURE_SWIZZLE_G 0x8E43
#define GL_TEXTURE_SWIZZLE_B 0x8E44
#define GL_TEXTURE_SWIZZLE_A 0x8E45
#define GL_ONE 1
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_BLEND 0x0BE2
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_ONE 1
#define GL_ZERO 0
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
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5

typedef struct { float x, y, u, v, r, g, b, a; } vertex_t;
typedef struct {
    const void *pixels;   /* cache key: the image's RGBA payload pointer */
    int w, h;
    int atlas_x, atlas_y;
    int used;
} texture_t;

/* A render target is its own texture with an FBO attached, NOT an atlas
 * slot: the atlas is one shared texture, so rendering into a sub-rect of it
 * would let one canvas's draws land on another's pixels. Keyed by the same
 * RGBA payload pointer sprites use, so drawing the canvas later finds it. */
typedef struct {
    const void *key;
    GLuint tex, fbo;
    int w, h;
    int used;
} target_t;
#define MAX_TARGETS 16
static target_t targets[MAX_TARGETS];
static target_t *target_find(const void *key);
static target_t *current_target;   /* NULL = the screen */

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
/* Which texture the open textured batch is for. Sprites share the atlas, but
 * a canvas is its own texture, so a batch cannot span the two. */
static GLuint textured_batch_tex;
static int textured_batch_mode = 1;   /* 1 = RGBA texture, 2 = glyph coverage */
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
    /* u_textured: 0 = solid, 1 = RGBA sprite/canvas, 2 = single-channel
     * glyph coverage. Mode 2 exists because TEXTURE_SWIZZLE is GL ES 3.0
     * only and NOT part of WebGL2, which is the surface wasmcart specifies;
     * relying on it left glyphs reading (cov,0,0,1) and text came out red. */
    "  if (u_textured == 1) frag_color *= texture(u_tex, v_uv);\n"
    "  else if (u_textured == 2) frag_color.a *= texture(u_tex, v_uv).r;\n"
    "}\n";

/* Set when a shader or the program reports failure.
 *
 * On a real driver this would mean the GLSL below is wrong -- a CART bug, to
 * be fixed in the shader, not worked around here. Both shaders are verified
 * to compile on a real GLES3 driver, so that is not the case being handled.
 *
 * What this actually catches is a host that STUBS the `gl` module: every
 * import returns 0, so glGetShaderiv reports "not compiled" for a shader
 * that never existed. Indistinguishable from a compile error at this level,
 * and the response is the same either way -- do not pretend GL works. */
static int gl_broken;

static void log_obj(GLuint object, const char *what, int shader) {
    GLint ok = 0, len = 0;
    if (shader) glGetShaderiv(object, GL_COMPILE_STATUS, &ok);
    else glGetProgramiv(object, GL_LINK_STATUS, &ok);
    if (ok) return;
    gl_broken = 1;
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
    set_textured(textured_batch_mode);
    bind_texture(textured_batch_tex ? textured_batch_tex : atlas_texture);
    glDrawElements(GL_TRIANGLES, (GLsizei)((textured_batch_count / 4) * 6),
                   GL_UNSIGNED_SHORT, (const void *)0);
    frame_stats.draws++;
    frame_stats.tex_flushes++;
    frame_stats.upload_bytes += (uint32_t)(sizeof(vertex_t) * textured_batch_count);
    textured_batch_count = 0;
    textured_batch_tex = 0;
    textured_batch_mode = 1;
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

    /* Separate alpha blending, which matters for render targets.
     *
     * A single glBlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA) applies to the
     * ALPHA channel as well, so drawing at alpha 0.6 into an opaque canvas
     * leaves 0.6 + 1*0.4 = 0.76 there rather than 1.0. Drawing that canvas
     * back to the screen then multiplies by 0.76 a second time and the
     * result is visibly darker -- which is exactly what the canvas
     * conformance cart showed, as a delta of 4 across all three channels
     * with the geometry perfectly aligned.
     *
     * The software rasterizer has no such problem: blend_px only ever writes
     * colour and canvases stay opaque. GL_ONE / GL_ONE_MINUS_SRC_ALPHA for
     * the alpha channel reproduces that -- destination alpha saturates at 1
     * instead of being scaled down. */
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                        GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    blend_enabled = -1;
    textured_enabled = -1;

    /* A host that stubs the `gl` module (every call returns 0) gets here
     * with nothing actually compiled. Rendering into that produces a black
     * frame with no error, which is the worst failure mode available, so
     * treat it as no GL at all and stay on the software rasterizer. */
    if (gl_broken || !program || !atlas_texture) {
        /* Either the host stubbed `gl` (the usual cause: every call returns
         * 0) or the shaders genuinely failed. The second would be a bug in
         * this file and shows up on every host; the log line is deliberately
         * about the outcome rather than guessing which. */
        WC_LOG("gl2d: no usable GL context, using the software rasterizer");
        cpu_mode = 1;
        frame_disabled = 1;
        /* ready stays 0 on purpose, which also suppresses the wc_gl_blit in
         * wcl_r2d_end: if the gl module is stubbed then the blit cannot
         * present either, and a 2D host reads fb_ptr directly. Attempting it
         * would just be more no-op calls on the way to the same frame. */
        ready = 0;
        return 0;
    }

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
        /* leave the screen bound for the next frame */
        if (current_target) wcl_r2d_target(NULL, 0, 0);
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

/* A font's baked coverage bitmap, uploaded once as a single-channel texture.
 * The shared shader's mode 2 multiplies only ALPHA by the texture's red
 * channel, which is the same thing blend_px(cov * a / 255) does on the
 * software path. (Doing it with TEXTURE_SWIZZLE would be neater but that is
 * GL ES 3.0 only, not WebGL2, so it silently failed and text came out red.) */
typedef struct {
    const unsigned char *key;
    GLuint tex;
    int w, h;
    int used;
} glyphtex_t;
#define MAX_GLYPHTEX 8
static glyphtex_t glyphtexes[MAX_GLYPHTEX];

static glyphtex_t *glyphtex_get(const unsigned char *atlas, int aw, int ah) {
    for (int i = 0; i < MAX_GLYPHTEX; i++)
        if (glyphtexes[i].used && glyphtexes[i].key == atlas) return &glyphtexes[i];
    for (int i = 0; i < MAX_GLYPHTEX; i++) {
        if (glyphtexes[i].used) continue;
        glyphtex_t *g = &glyphtexes[i];
        flush_batches();
        glGenTextures(1, &g->tex);
        glBindTexture(GL_TEXTURE_2D, g->tex);
        /* rows are byte-packed, not word-aligned */
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, aw, ah, 0,
                     GL_RED, GL_UNSIGNED_BYTE, atlas);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        bound_texture = g->tex;
        g->key = atlas; g->w = aw; g->h = ah; g->used = 1;
        return g;
    }
    return NULL;
}

int wcl_r2d_glyph(const unsigned char *atlas, int aw, int ah,
                  int dx, int dy, int dw, int dh,
                  int sx, int sy, int srcw, int srch,
                  uint32_t color, int alpha) {
    if (!wcl_r2d_active() || !atlas || dw <= 0 || dh <= 0) return 0;
    glyphtex_t *g = glyphtex_get(atlas, aw, ah);
    if (!g) return 0;
    if (solid_batch_count) flush_solid_batch();
    if (textured_batch_tex && textured_batch_tex != g->tex) flush_textured_batch();
    if (textured_batch_mode != 2 && textured_batch_count) flush_textured_batch();
    if (textured_batch_count + 4 > BATCH_MAX * 4) flush_textured_batch();
    textured_batch_tex = g->tex;
    textured_batch_mode = 2;

    vertex_t *v = &textured_batch[textured_batch_count];
    float x0, y0, x1, y1;
    ndc((float)dx, (float)dy, &x0, &y0);
    ndc((float)(dx + dw), (float)(dy + dh), &x1, &y1);
    float u0 = (float)sx / (float)g->w, u1 = (float)(sx + srcw) / (float)g->w;
    float v0 = (float)sy / (float)g->h, v1 = (float)(sy + srch) / (float)g->h;
    float r = (float)((color >> 16) & 255) / 255.0f;
    float gg = (float)((color >> 8) & 255) / 255.0f;
    float b = (float)(color & 255) / 255.0f;
    float a = (float)alpha / 255.0f;
    const float us[4] = { u0, u1, u1, u0 };
    const float vs[4] = { v0, v0, v1, v1 };
    const float xs[4] = { x0, x1, x1, x0 };
    const float ys[4] = { y0, y0, y1, y1 };
    for (int i = 0; i < 4; i++) {
        v[i].x = xs[i]; v[i].y = ys[i]; v[i].u = us[i]; v[i].v = vs[i];
        v[i].r = r; v[i].g = gg; v[i].b = b; v[i].a = a;
    }
    textured_batch_count += 4;
    frame_stats.quads++;
    return 1;
}

int wcl_r2d_sprite(const void *pixels, int sw, int sh,
                   const double *cx, const double *cy,
                   int sx, int sy, int srcw, int srch,
                   uint32_t tint, int alpha) {
    if (!wcl_r2d_active() || !pixels || !cx || !cy) return 0;

    /* A canvas already lives on the GPU in its own texture; anything else
     * goes through the shared atlas. */
    target_t *tgt = target_find(pixels);
    if (tgt && tgt == current_target) return 0;   /* cannot sample the target
                                                     it is drawing into */
    GLuint want = tgt ? tgt->tex : atlas_texture;
    float u0, v0, u1, v1;
    if (tgt) {
        u0 = (float)sx / (float)tgt->w;
        u1 = (float)(sx + srcw) / (float)tgt->w;
        /* An FBO texture has GL's bottom-left origin while the cart's
         * coordinates are top-left, so sampling flips V.
         *
         * Flip the ROWS, not the edges. Texture coordinates address texel
         * boundaries, so swapping v0/v1 moves every sample one texel earlier
         * under NEAREST -- which showed up as GL reading a neighbouring
         * source texel (0 where the software path read 4, on a sprite whose
         * channels step by 4). Measuring from the far edge keeps the sample
         * on the same texel. */
        v0 = (float)(tgt->h - sy) / (float)tgt->h;
        v1 = (float)(tgt->h - (sy + srch)) / (float)tgt->h;
    } else {
        texture_t *t = get_texture(pixels, sw, sh);
        if (!t) return 0;             /* atlas full: caller falls back */
        /* Source rect in atlas UV. The atlas is NEAREST-filtered, so an
         * axis-aligned quad at an integer scale samples the same texels the
         * software path indexes; rotation and fractional scales can differ
         * by a texel at boundaries, inside the accepted tolerance. */
        u0 = (float)(t->atlas_x + sx) / (float)ATLAS_SIZE;
        v0 = (float)(t->atlas_y + sy) / (float)ATLAS_SIZE;
        u1 = (float)(t->atlas_x + sx + srcw) / (float)ATLAS_SIZE;
        v1 = (float)(t->atlas_y + sy + srch) / (float)ATLAS_SIZE;
    }
    if (solid_batch_count) flush_solid_batch();
    if (textured_batch_tex && textured_batch_tex != want) flush_textured_batch();
    if (textured_batch_mode != 1 && textured_batch_count) flush_textured_batch();
    if (textured_batch_count + 4 > BATCH_MAX * 4) flush_textured_batch();
    textured_batch_tex = want;
    textured_batch_mode = 1;

    vertex_t *v = &textured_batch[textured_batch_count];
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

static target_t *target_find(const void *key) {
    for (int i = 0; i < MAX_TARGETS; i++)
        if (targets[i].used && targets[i].key == key) return &targets[i];
    return NULL;
}

int wcl_r2d_target(const void *key, int w, int h) {
    if (!ready) return 0;
    flush_batches();

    if (!key) {                       /* back to the screen */
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, width, height);
        current_target = NULL;
        ndc_scale_x = 2.0f / (float)width;
        ndc_scale_y = 2.0f / (float)height;
        return 1;
    }

    target_t *t = target_find(key);
    if (!t) {
        for (int i = 0; i < MAX_TARGETS && !t; i++) if (!targets[i].used) t = &targets[i];
        if (!t) return 0;             /* out of targets: caller falls back */
        glGenTextures(1, &t->tex);
        glBindTexture(GL_TEXTURE_2D, t->tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, (const void *)0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        bound_texture = t->tex;
        glGenFramebuffers(1, &t->fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, t->tex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            return 0;                 /* incomplete: caller falls back */
        }
        t->key = key; t->w = w; t->h = h; t->used = 1;
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    }

    glViewport(0, 0, t->w, t->h);
    current_target = t;
    /* Draw coordinates are canvas-relative while a target is bound. The y
     * flip in ndc() still applies: an FBO texture has the same bottom-left
     * origin as the default framebuffer, and the cart's is top-left. */
    ndc_scale_x = 2.0f / (float)t->w;
    ndc_scale_y = 2.0f / (float)t->h;
    return 1;
}

void wcl_r2d_clear(uint32_t color, int alpha) {
    if (!wcl_r2d_active()) return;
    flush_batches();
    glClearColor((float)((color >> 16) & 255) / 255.0f,
                 (float)((color >> 8) & 255) / 255.0f,
                 (float)(color & 255) / 255.0f,
                 (float)alpha / 255.0f);
    /* the cached screen-clear colour no longer reflects GL state */
    current_clear_color = 0xFFFFFFFFu;
    glClear(GL_COLOR_BUFFER_BIT);
}

void wcl_r2d_forget(const void *key) {
    target_t *t = target_find(key);
    if (t) { t->used = 0; t->key = NULL; }
    for (int i = 0; i < MAX_TEXTURES; i++)
        if (textures[i].used && textures[i].pixels == key) textures[i].used = 0;
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
