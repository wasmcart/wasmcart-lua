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
#include <stdio.h>
#include <math.h>

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

/* u,v carry the circle centre and `rad` the radius when this vertex belongs
 * to a circle quad; they are unused (0) for solids and carry real UVs for
 * textured draws. Putting them per-vertex is what lets circles BATCH -- as a
 * uniform, every circle needed its own draw call, and a cart drawing 900 of
 * them spent 0.69 ms/frame in GL call overhead alone. */
typedef struct { float x, y, u, v, r, g, b, a, rad; } vertex_t;
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
static GLint rad_attr;

#define MAX_TEXTURES 64
static texture_t textures[MAX_TEXTURES];
#define ATLAS_SIZE 2048
static GLuint atlas_texture;
static int atlas_x, atlas_y, atlas_row_h;

#define BATCH_MAX 4096
static vertex_t solid_batch[BATCH_MAX * 4];
static int solid_batch_count;
static int solid_batch_has_alpha;
static int solid_batch_is_circle;
static vertex_t textured_batch[BATCH_MAX * 4];
static int textured_batch_count;
/* Which texture the open textured batch is for. Sprites share the atlas, but
 * a canvas is its own texture, so a batch cannot span the two. */
static GLuint textured_batch_tex;
static int textured_batch_mode = 1;   /* 1 = RGBA texture, 2 = glyph coverage */
static uint16_t indices[BATCH_MAX * 6];

static int scissor_on;
static int blend_add_on;
static void flush_batches(void);
/* Custom shaders (defined further down). Declared here because set_textured
 * and bind_texture, which sit above them, must consult the bound program. */
#define MAX_SHADERS 8
typedef struct {
    GLuint program;
    GLint tex_uniform, textured_uniform;
    int used;
} shader_t;
static shader_t shaders[MAX_SHADERS];
static int active_shader = -1;   /* -1 = the engine's default program */
static int blend_enabled = -1;
static int textured_enabled = -1;
static GLuint bound_texture;
static uint32_t current_clear_color = 0xFFFFFFFFu;
static wcl_r2d_stats_t frame_stats;

/* Attribute LOCATIONS are pinned with glBindAttribLocation before every link,
 * default program and custom shaders alike. One VAO is shared by every
 * program, and a VAO records attribute state per INDEX -- so if a custom
 * shader's linker happened to assign a_uv index 0 while the default had it at
 * 1, binding that program would silently read positions as UVs. Pinning the
 * indices is what makes "just glUseProgram a different program" safe. */
#define ATTR_POS 0
#define ATTR_UV 1
#define ATTR_COLOR 2
#define ATTR_RAD 3

static const char *VERTEX_SHADER =
    "#version 300 es\n"
    "in vec2 a_pos;\n"
    "in vec2 a_uv;\n"
    "in vec4 a_color;\n"
    "in highp float a_rad;\n"
    "out vec2 v_uv;\n"
    "out vec4 v_color;\n"
    "out highp vec2 v_centre;\n"
    "out highp float v_rad;\n"
    "void main() {\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "  v_uv = a_uv; v_color = a_color;\n"
    "  v_centre = a_uv; v_rad = a_rad;\n"   /* circle mode reuses a_uv */
    "}\n";

static const char *FRAGMENT_SHADER =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 v_uv;\n"
    "in vec4 v_color;\n"
    "in highp vec2 v_centre;\n"
    "in highp float v_rad;\n"
    "out vec4 frag_color;\n"
    "uniform sampler2D u_tex;\n"
    "uniform int u_textured;\n"
    /* highp, not the mediump default: the circle rule squares the radius and
     * the row offset, and mediump is only guaranteed ~10 bits of mantissa in
     * GLES. At r=66 that put 24 pixels a whole pixel outside the software
     * fill. Colour and UV stay mediump; only the coverage maths needs it. */

    "void main() {\n"
    "  frag_color = v_color;\n"
    /* u_textured: 0 = solid, 1 = RGBA sprite/canvas, 2 = single-channel
     * glyph coverage. Mode 2 exists because TEXTURE_SWIZZLE is GL ES 3.0
     * only and NOT part of WebGL2, which is the surface wasmcart specifies;
     * relying on it left glyphs reading (cov,0,0,1) and text came out red. */
    "  if (u_textured == 1) frag_color *= texture(u_tex, v_uv);\n"
    "  else if (u_textured == 2) frag_color.a *= texture(u_tex, v_uv).r;\n"
    /* Mode 3: a filled circle, evaluated per fragment rather than
     * approximated by geometry. The software fill is, for each row,
     * span = round(sqrt(r*r - dy*dy)) covering |dx| <= span. Computing that
     * same rule here reproduces it EXACTLY at every radius -- no triangle
     * fan, no segment count, no minimum radius, and no tolerance. This is
     * what a 2D GPU renderer should do with a circle. */
    "  else if (u_textured == 3) {\n"
    "    highp float dy = floor(gl_FragCoord.y) - v_centre.y;\n"
    "    highp float dx = floor(gl_FragCoord.x) - v_centre.x;\n"
    "    highp float t = v_rad * v_rad - dy * dy;\n"
    "    if (t < 0.0) discard;\n"
    "    if (abs(dx) > floor(sqrt(t) + 0.5)) discard;\n"
    "  }\n"
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

/* ── custom shaders ────────────────────────────────────────────────────
 *
 * A cart shader is a SECOND PROGRAM built to the same contract as the
 * default one: same four attributes at the same pinned indices, same
 * u_tex/u_textured uniforms with the same meanings. That is what lets every
 * existing draw path keep working while one is bound -- a rect still batches
 * as a solid, a sprite still samples the atlas, text still uses coverage
 * mode 2 -- with only the fragment maths swapped.
 *
 * The cart writes LOVE's shape, a function
 *     vec4 effect(vec4 color, Image tex, vec2 texture_coords, vec2 screen_coords)
 * and this file wraps it: the #version line, the ins/outs, the Texel /
 * love_ScreenSize / VaryingColor predefines, and a main() that dispatches on
 * u_textured exactly the way the default fragment shader does and then hands
 * the result to effect(). A cart never writes any of that, and CANNOT write
 * its own #version -- the synthesized one has to come first, and two
 * #version lines is a compile error rather than an override.
 */
/* How many texture units the cart's own Image uniforms may claim. Unit 0 is
 * the engine's (the atlas, a canvas, or a glyph texture) and is never given
 * away, because every draw path binds to it. */
#define SHADER_TEX_UNITS 4
typedef struct {
    GLint location;
    GLuint tex;
    int unit;
    int used;
} shader_sampler_t;
static shader_sampler_t shader_samplers[MAX_SHADERS][SHADER_TEX_UNITS];

static void shader_log(const char *msg) {
    wc_log(msg, (unsigned int)strlen(msg));
}

/* Compile without the gl_broken side effect log_obj has.
 *
 * A CART's shader failing to compile is a cart bug: report it, refuse the
 * handle, and carry on rendering. log_obj sets gl_broken, which the engine
 * reads as "this host has no usable GL" -- letting a typo in a cart's GLSL
 * flip that would silently drop the whole run onto the software rasterizer,
 * which is exactly the failure this is meant to make visible. */
static int shader_compile_checked(GLenum type, const char *source, GLuint *out) {
    GLuint sh = glCreateShader(type);
    GLint length = (GLint)strlen(source);
    glShaderSource(sh, 1, &source, &length);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        char log[1024];
        const char *tag = (type == GL_FRAGMENT_SHADER)
            ? "love.graphics.newShader: pixel shader failed to compile:\n"
            : "love.graphics.newShader: vertex shader failed to compile:\n";
        shader_log(tag);
        if (len > 0) {
            GLsizei got = 0;
            glGetShaderInfoLog(sh, (GLsizei)sizeof log, &got, log);
            if (got > 0) wc_log(log, (unsigned int)got);
        } else {
            /* A host that stubs `gl` reports "not compiled" with an empty log
             * for a shader that never existed. Say which case it might be
             * rather than printing nothing at all. */
            shader_log("(no info log: the driver gave none, or this host has no GL)");
        }
        glDeleteShader(sh);
        return 0;
    }
    *out = sh;
    return 1;
}

/* The generated wrapper around the cart's effect().
 *
 * u_textured is handled here identically to the default fragment shader, so
 * the same rect/sprite/glyph/circle batches keep their meanings:
 *   0  solid       -> the cart sees a white 1x1 texel
 *   1  RGBA texture-> the cart sees the sampled texel
 *   2  glyph       -> coverage in .r, expanded to (1,1,1,cov) so a shader
 *                     that ignores the texture still tints text correctly
 *   3  circle      -> the coverage discard runs BEFORE effect(), because a
 *                     discarded fragment must not run cart code at all
 *
 * The Texel/love_ScreenSize/VaryingColor names are LOVE's, defined here so a
 * shader copied from a LOVE project compiles unchanged. */
static const char *SHADER_FRAG_PROLOGUE =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 v_uv;\n"
    "in vec4 v_color;\n"
    "in highp vec2 v_centre;\n"
    "in highp float v_rad;\n"
    "out vec4 wc_frag_color;\n"
    "uniform sampler2D u_tex;\n"
    "uniform int u_textured;\n"
    "uniform vec2 love_ScreenSize;\n"
    "#define Image sampler2D\n"
    "#define VaryingColor v_color\n"
    "#define VaryingTexCoord vec4(v_uv, 0.0, 1.0)\n"
    "#define number float\n"
    "#define extern uniform\n"
    /* Texel() honours u_textured, because otherwise a shader gets a real
     * atlas texel for a draw that has no texture at all.
     *
     * This is not a nicety. `return vec4(1.0 - Texel(tex, uv).rgb, px.a)` on
     * a plain rectangle sampled atlas texel (0,0), which is opaque black, so
     * every solid rect inverted to WHITE instead of to its own negative --
     * visibly wrong, and wrong in a way that still looks like "the shader
     * ran". A solid draw must see an all-white texel so `Texel(...) * color`
     * reduces to the vertex colour, which is what LOVE's untextured draws do.
     *
     * The rule is applied unconditionally rather than only to the engine's
     * own sampler, because GLSL ES 3.00 forbids comparing opaque sampler
     * types -- `t == u_tex` will not compile. A cart sampling its OWN Image
     * uniform (a palette or lookup table) should call texture() directly;
     * that is plain GLSL and works here. */
    "vec4 Texel(sampler2D t, vec2 uv) {\n"
    "  if (u_textured == 1) return texture(t, uv);\n"
    "  if (u_textured == 2) return vec4(1.0, 1.0, 1.0, texture(t, uv).r);\n"
    "  return vec4(1.0);\n"
    "}\n"
    "#line 1\n";

static const char *SHADER_FRAG_EPILOGUE =
    "\nvoid main() {\n"
    "  if (u_textured == 3) {\n"
    "    highp float dy = floor(gl_FragCoord.y) - v_centre.y;\n"
    "    highp float dx = floor(gl_FragCoord.x) - v_centre.x;\n"
    "    highp float t = v_rad * v_rad - dy * dy;\n"
    "    if (t < 0.0) discard;\n"
    "    if (abs(dx) > floor(sqrt(t) + 0.5)) discard;\n"
    "  }\n"
    "  vec2 wc_uv = (u_textured == 1 || u_textured == 2) ? v_uv : vec2(0.0);\n"
    "  wc_frag_color = effect(v_color, u_tex, wc_uv, gl_FragCoord.xy);\n"
    "}\n";

/* The vertex side. A cart's position() sees the same signature LOVE gives it,
 * but our vertices arrive ALREADY IN CLIP SPACE (see ndc()) -- there is no
 * model/view/projection matrix in this engine, so transform_projection is the
 * identity. A shader that multiplies by it gets the right answer; one that
 * builds its own projection from it will not, and that is documented rather
 * than papered over. */
static const char *SHADER_VERT_PROLOGUE =
    "#version 300 es\n"
    "in vec2 a_pos;\n"
    "in vec2 a_uv;\n"
    "in vec4 a_color;\n"
    "in highp float a_rad;\n"
    "out vec2 v_uv;\n"
    "out vec4 v_color;\n"
    "out highp vec2 v_centre;\n"
    "out highp float v_rad;\n"
    "uniform vec2 love_ScreenSize;\n"
    "#define Image sampler2D\n"
    "#define Texel texture\n"
    "#define number float\n"
    "#define extern uniform\n"
    "#define VertexPosition vec4(a_pos, 0.0, 1.0)\n"
    "#define VertexTexCoord vec4(a_uv, 0.0, 1.0)\n"
    "#define VertexColor a_color\n"
    "#line 1\n";

static const char *SHADER_VERT_EPILOGUE =
    "\nvoid main() {\n"
    "  v_uv = a_uv; v_color = a_color;\n"
    "  v_centre = a_uv; v_rad = a_rad;\n"
    "  gl_Position = position(mat4(1.0), vec4(a_pos, 0.0, 1.0));\n"
    "}\n";

/* Composition buffer. Shaders are small and this runs once per newShader, so
 * a single static buffer beats a malloc that has to be freed on every error
 * path. Overflow is a hard refusal, not a truncation: a truncated shader
 * would report a bewildering syntax error at the cut point. */
#define SHADER_SRC_MAX 16384
static char shader_src_buf[SHADER_SRC_MAX];

static int shader_compose(const char *prologue, const char *body,
                          const char *epilogue) {
    size_t a = strlen(prologue), b = strlen(body), c = strlen(epilogue);
    if (a + b + c + 1 > SHADER_SRC_MAX) {
        shader_log("love.graphics.newShader: shader source is too large "
                   "(limit is 16 KB of cart GLSL)");
        return 0;
    }
    memcpy(shader_src_buf, prologue, a);
    memcpy(shader_src_buf + a, body, b);
    memcpy(shader_src_buf + a + b, epilogue, c);
    shader_src_buf[a + b + c] = 0;
    return 1;
}

/* Pin the attribute indices, then link. Shared by the default program and
 * every cart shader so one VAO stays valid across all of them. */
static void bind_attribs_and_link(GLuint prog) {
    glBindAttribLocation(prog, ATTR_POS, "a_pos");
    glBindAttribLocation(prog, ATTR_UV, "a_uv");
    glBindAttribLocation(prog, ATTR_COLOR, "a_color");
    glBindAttribLocation(prog, ATTR_RAD, "a_rad");
    glLinkProgram(prog);
}

/* Reject desktop GLSL before the driver sees it.
 *
 * The surface here is WebGL2 / GLES 3.0 and nothing else. A cart's own
 * #version line cannot be honoured (the synthesized one must come first, and
 * two #version directives is an error), and desktop-only constructs give
 * driver errors that point at generated line numbers a cart author never
 * wrote. Naming the actual problem is worth the handful of string scans. */
static int shader_reject_unsupported(const char *src, const char *which) {
    char line[256];
    if (strstr(src, "#version")) {
        snprintf(line, sizeof line,
                 "love.graphics.newShader: the %s shader declares its own "
                 "#version. This engine targets WebGL2 / GLES 3.0 and emits "
                 "\"#version 300 es\" itself; remove the directive.", which);
        shader_log(line);
        return 1;
    }
    /* GLES 3.0 has no compute, no geometry/tessellation stages, and no
     * image load/store. These fail in the driver with messages about the
     * generated preamble, so catch them by name instead. */
    static const char *banned[] = {
        "layout(local_size", "gl_GlobalInvocationID", "imageStore", "imageLoad",
        "gl_FragColor", "texture2D", "varying", "attribute", NULL
    };
    static const char *why[] = {
        "compute shaders are not in GLES 3.0",
        "compute shaders are not in GLES 3.0",
        "image load/store is GLES 3.1+, not available here",
        "image load/store is GLES 3.1+, not available here",
        "gl_FragColor is GLSL ES 1.00; return the colour from effect() instead",
        "texture2D is GLSL ES 1.00; use Texel(...) or texture(...)",
        "`varying` is GLSL ES 1.00; use in/out",
        "`attribute` is GLSL ES 1.00; use in",
        NULL
    };
    for (int i = 0; banned[i]; i++) {
        if (strstr(src, banned[i])) {
            snprintf(line, sizeof line,
                     "love.graphics.newShader: the %s shader uses \"%s\" -- %s.",
                     which, banned[i], why[i]);
            shader_log(line);
            return 1;
        }
    }
    return 0;
}

int wcl_r2d_shader_new(const char *pixel_src, const char *vertex_src) {
    if (!ready) {
        shader_log("love.graphics.newShader: no GL context on this host, so "
                   "shaders cannot run (the engine is on the software "
                   "rasterizer)");
        return -1;
    }
    if (!pixel_src && !vertex_src) return -1;

    int slot = -1;
    for (int i = 0; i < MAX_SHADERS; i++) if (!shaders[i].used) { slot = i; break; }
    if (slot < 0) {
        shader_log("love.graphics.newShader: out of shader slots (8 max)");
        return -1;
    }

    GLuint vs = 0, fs = 0;
    if (vertex_src) {
        if (shader_reject_unsupported(vertex_src, "vertex")) return -1;
        if (!shader_compose(SHADER_VERT_PROLOGUE, vertex_src, SHADER_VERT_EPILOGUE))
            return -1;
        if (!shader_compile_checked(GL_VERTEX_SHADER, shader_src_buf, &vs)) return -1;
    } else {
        vs = compile_shader(GL_VERTEX_SHADER, VERTEX_SHADER);
    }

    if (pixel_src) {
        if (shader_reject_unsupported(pixel_src, "pixel")) { glDeleteShader(vs); return -1; }
        if (!shader_compose(SHADER_FRAG_PROLOGUE, pixel_src, SHADER_FRAG_EPILOGUE)) {
            glDeleteShader(vs);
            return -1;
        }
        if (!shader_compile_checked(GL_FRAGMENT_SHADER, shader_src_buf, &fs)) {
            glDeleteShader(vs);
            return -1;
        }
    } else {
        fs = compile_shader(GL_FRAGMENT_SHADER, FRAGMENT_SHADER);
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    bind_attribs_and_link(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    /* The shader objects are attached; the program keeps them alive. */
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        shader_log("love.graphics.newShader: program failed to link:\n");
        if (len > 0) {
            char log[1024];
            GLsizei got = 0;
            glGetProgramInfoLog(prog, (GLsizei)sizeof log, &got, log);
            if (got > 0) wc_log(log, (unsigned int)got);
        } else {
            shader_log("(no info log; commonly a missing effect() or "
                       "position() function)");
        }
        glDeleteProgram(prog);
        return -1;
    }

    shader_t *sh = &shaders[slot];
    sh->program = prog;
    sh->used = 1;
    sh->tex_uniform = glGetUniformLocation(prog, "u_tex");
    sh->textured_uniform = glGetUniformLocation(prog, "u_textured");
    for (int i = 0; i < SHADER_TEX_UNITS; i++) shader_samplers[slot][i].used = 0;

    /* Seed the program's own uniform state. Uniform values belong to the
     * PROGRAM, so this cannot wait for the first draw under the shared
     * set_textured cache -- that cache tracks whichever program is current. */
    glUseProgram(prog);
    if (sh->tex_uniform >= 0) glUniform1i(sh->tex_uniform, 0);
    GLint screen = glGetUniformLocation(prog, "love_ScreenSize");
    if (screen >= 0) glUniform2f(screen, (float)width, (float)height);
    glUseProgram(active_shader >= 0 ? shaders[active_shader].program : program);
    return slot;
}

static shader_t *shader_by_handle(int handle) {
    if (handle < 0 || handle >= MAX_SHADERS || !shaders[handle].used) return NULL;
    return &shaders[handle];
}

void wcl_r2d_shader_use(int handle) {
    if (!ready) return;
    if (handle >= 0 && !shader_by_handle(handle)) return;
    if (handle == active_shader) return;
    /* A batch built for one program must not be drawn by another: the
     * program is pipeline state, so pending vertices belong to the shader
     * that was bound when they were queued. Same rule as a texture change. */
    flush_batches();
    active_shader = handle;
    shader_t *sh = shader_by_handle(handle);
    glUseProgram(sh ? sh->program : program);
    /* u_textured lives in the program object, so the cached value from the
     * program we just left says nothing about this one. */
    textured_enabled = -1;
    if (sh) {
        /* Rebind the cart's own sampler uniforms: their texture-unit
         * assignment is program state too, and a unit's binding is global,
         * so both have to be re-established on every switch. */
        for (int i = 0; i < SHADER_TEX_UNITS; i++) {
            shader_sampler_t *s = &shader_samplers[handle][i];
            if (!s->used) continue;
            glActiveTexture((GLenum)(GL_TEXTURE0 + s->unit));
            glBindTexture(GL_TEXTURE_2D, s->tex);
        }
        glActiveTexture(GL_TEXTURE0);
    }
}

int wcl_r2d_shader_active(void) { return active_shader >= 0; }

/* Uniform writes go to the named program, which must be current for the call
 * to land, so the previous program is restored afterwards. LOVE lets a cart
 * send to a shader that is not bound, and games do exactly that during load. */
static GLint shader_uniform(int handle, const char *name, shader_t **out) {
    shader_t *sh = shader_by_handle(handle);
    if (!sh) return -1;
    *out = sh;
    return glGetUniformLocation(sh->program, name);
}

static void shader_restore_program(void) {
    glUseProgram(active_shader >= 0 ? shaders[active_shader].program : program);
}

int wcl_r2d_shader_has_uniform(int handle, const char *name) {
    shader_t *sh = NULL;
    return shader_uniform(handle, name, &sh) >= 0;
}

int wcl_r2d_shader_send_float(int handle, const char *name, const float *v, int n) {
    shader_t *sh = NULL;
    GLint loc = shader_uniform(handle, name, &sh);
    if (!sh || loc < 0) return 0;
    glUseProgram(sh->program);
    switch (n) {
        case 1: glUniform1f(loc, v[0]); break;
        case 2: glUniform2f(loc, v[0], v[1]); break;
        case 3: glUniform3f(loc, v[0], v[1], v[2]); break;
        case 4: glUniform4f(loc, v[0], v[1], v[2], v[3]); break;
        case 16: glUniformMatrix4fv(loc, 1, 0, v); break;
        default: glUseProgram(program); shader_restore_program(); return 0;
    }
    shader_restore_program();
    return 1;
}

int wcl_r2d_shader_send_int(int handle, const char *name, const int *v, int n) {
    shader_t *sh = NULL;
    GLint loc = shader_uniform(handle, name, &sh);
    if (!sh || loc < 0) return 0;
    glUseProgram(sh->program);
    switch (n) {
        case 1: glUniform1i(loc, v[0]); break;
        case 2: glUniform2i(loc, v[0], v[1]); break;
        case 3: glUniform3i(loc, v[0], v[1], v[2]); break;
        case 4: glUniform4i(loc, v[0], v[1], v[2], v[3]); break;
        default: shader_restore_program(); return 0;
    }
    shader_restore_program();
    return 1;
}

static texture_t *get_texture(const void *pixels, int w, int h);

int wcl_r2d_shader_send_image(int handle, const char *name,
                              const void *pixels, int w, int h) {
    shader_t *sh = NULL;
    GLint loc = shader_uniform(handle, name, &sh);
    if (!sh || loc < 0 || !pixels) return 0;

    /* Which GL texture backs this image? A canvas has its own; anything else
     * lives in the shared atlas -- and an atlas entry is a SUB-RECT, so the
     * uv range the shader must sample is not 0..1. That is a real limitation
     * and it is reported rather than silently sampling the whole atlas. */
    target_t *tgt = target_find(pixels);
    GLuint tex;
    if (tgt) {
        tex = tgt->tex;
    } else {
        texture_t *t = get_texture(pixels, w, h);
        if (!t) return 0;
        if (t->w != ATLAS_SIZE || t->h != ATLAS_SIZE) {
            /* Not fatal: many shaders use a lookup texture at fixed
             * coordinates and do not care. Say what the uv range actually is
             * so a cart author can scale for it. */
            char line[256];
            snprintf(line, sizeof line,
                     "Shader:send(\"%s\", image): sprite images live in a shared "
                     "atlas, so this sampler's uv range is (%g..%g, %g..%g), not "
                     "0..1. Use a Canvas for a 0..1 sampler.",
                     name,
                     (double)t->atlas_x / ATLAS_SIZE,
                     (double)(t->atlas_x + t->w) / ATLAS_SIZE,
                     (double)t->atlas_y / ATLAS_SIZE,
                     (double)(t->atlas_y + t->h) / ATLAS_SIZE);
            shader_log(line);
        }
        tex = atlas_texture;
    }

    /* Find (or claim) a texture unit for this uniform. Unit 0 belongs to the
     * engine -- every draw path binds its own texture there -- so cart
     * samplers start at 1. */
    int idx = -1;
    for (int i = 0; i < SHADER_TEX_UNITS; i++) {
        if (shader_samplers[handle][i].used &&
            shader_samplers[handle][i].location == loc) { idx = i; break; }
    }
    if (idx < 0) {
        for (int i = 0; i < SHADER_TEX_UNITS; i++)
            if (!shader_samplers[handle][i].used) { idx = i; break; }
    }
    if (idx < 0) {
        shader_log("Shader:send: out of texture units (4 image uniforms max "
                   "per shader)");
        return 0;
    }
    shader_sampler_t *s = &shader_samplers[handle][idx];
    s->location = loc;
    s->tex = tex;
    s->unit = idx + 1;
    s->used = 1;

    flush_batches();   /* a pending batch was queued with the old binding */
    glUseProgram(sh->program);
    glUniform1i(loc, s->unit);
    glActiveTexture((GLenum)(GL_TEXTURE0 + s->unit));
    glBindTexture(GL_TEXTURE_2D, tex);
    glActiveTexture(GL_TEXTURE0);
    shader_restore_program();
    return 1;
}

/* Cart pixels -> clip space. y is flipped because the cart's origin is
 * top-left and GL's is bottom-left. */
static void ndc(float x, float y, float *out_x, float *out_y) {
    *out_x = x * ndc_scale_x - 1.0f;
    *out_y = 1.0f - y * ndc_scale_y;
}

static void set_blend(int enabled) {
    /* Additive needs blending ON even for fully opaque source colours, since
     * "add" is about the destination, not the source alpha. */
    if (blend_add_on) enabled = 1;
    if (blend_enabled == enabled) return;
    if (enabled) glEnable(GL_BLEND);
    else glDisable(GL_BLEND);
    blend_enabled = enabled;
}

/* love.graphics.setBlendMode("add"). The software path does
 * dst = min(255, dst + src*a/255), which is GL_SRC_ALPHA / GL_ONE.
 * Destination alpha is left alone: the framebuffer and every canvas stay
 * opaque, exactly as blend_px leaves them. */
void wcl_r2d_blend_add(int on) {
    if (!ready || blend_add_on == on) return;
    flush_batches();
    blend_add_on = on;
    if (on) {
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
        glEnable(GL_BLEND);
        blend_enabled = 1;
    } else {
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                            GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        blend_enabled = -1;   /* force the next set_blend to re-apply */
    }
}

/* u_textured is per-DRAW state, and its location differs per program (a cart
 * shader is a separate program object with its own uniform storage), so the
 * location has to come from whichever program is current. The cached
 * textured_enabled is invalidated on every program switch in
 * wcl_r2d_shader_use for the same reason. */
static void set_textured(int enabled) {
    if (textured_enabled == enabled) return;
    GLint loc = textured_uniform;
    if (active_shader >= 0) loc = shaders[active_shader].textured_uniform;
    if (loc >= 0) glUniform1i(loc, enabled);
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
    set_textured(solid_batch_is_circle ? 3 : 0);
    glDrawElements(GL_TRIANGLES, (GLsizei)((solid_batch_count / 4) * 6),
                   GL_UNSIGNED_SHORT, (const void *)0);
    frame_stats.draws++;
    frame_stats.solid_flushes++;
    frame_stats.upload_bytes += (uint32_t)(sizeof(vertex_t) * solid_batch_count);
    solid_batch_count = 0;
    solid_batch_has_alpha = 0;
    solid_batch_is_circle = 0;
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
    bind_attribs_and_link(program);
    log_obj(program, "gl2d program link failed", 0);
    glUseProgram(program);

    /* Pinned by bind_attribs_and_link, so these are the same indices every
     * cart shader gets and the one shared VAO stays valid across programs. */
    pos_attr = ATTR_POS;
    uv_attr = ATTR_UV;
    color_attr = ATTR_COLOR;
    rad_attr = ATTR_RAD;
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
    if (rad_attr >= 0) {
        glEnableVertexAttribArray(rad_attr);
        glVertexAttribPointer(rad_attr, 1, GL_FLOAT, 0, sizeof(vertex_t), (const void *)32);
    }

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
        /* wc_gl_blit leaves its own program/texture/VAO bound, so the cached
         * "which program is current" is stale too. Re-establish it rather
         * than trusting active_shader, or the first draw of the next GL frame
         * would go through the blit's program. */
        bound_texture = 0;
        textured_enabled = -1;
        blend_enabled = -1;
        glUseProgram(active_shader >= 0 ? shaders[active_shader].program : program);
    }
    wcl_r2d_stats = frame_stats;
}

/* Warned once, not per call: a cart that trips the fallback trips it every
 * frame, and 60 identical log lines a second buries the one that matters. */
static int warned_shader_dropped;

void wcl_r2d_disable(void) {
    if (wcl_r2d_active()) flush_batches();
    /* A bound shader cannot follow the frame onto the software rasterizer --
     * there is no CPU path that runs GLSL. Rendering would carry on looking
     * plausible while the shader did nothing at all, so say it out loud. */
    if (active_shader >= 0 && !warned_shader_dropped) {
        warned_shader_dropped = 1;
        WC_LOG("love.graphics.setShader: a custom shader is bound, but this "
               "frame used a feature the GL backend does not implement, so the "
               "engine fell back to the software rasterizer for the rest of "
               "the run. The shader is NOT being applied from here on.");
    }
    frame_disabled = 1;
    cpu_mode = 1;
}

int wcl_r2d_active(void) { return ready && !frame_disabled; }

int wcl_r2d_solid(int x, int y, int w, int h, uint32_t color, int alpha) {
    if (!wcl_r2d_active() || w <= 0 || h <= 0) return 0;
    if (textured_batch_count) flush_textured_batch();
    if (solid_batch_count && solid_batch_is_circle) flush_solid_batch();
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
        v[i].u = v[i].v = 0; v[i].rad = 0; v[i].rad = 0;
        v[i].r = r; v[i].g = g; v[i].b = b; v[i].a = a;
    }
    solid_batch_count += 4;
    frame_stats.quads++;
    return 1;
}

/* Polygon fill as triangles.
 *
 * The software path is an even-odd scanline fill sampling at pixel centres,
 * which is what GPU triangle rasterization does too, so interiors agree and
 * only the boundary can differ -- the same edge-coverage story as rotated
 * sprites.
 *
 * Convex polygons fan trivially. Concave ones are ear-clipped first, which
 * is exact for any SIMPLE polygon. Self-intersecting polygons are the one
 * case that must NOT be triangulated: even-odd leaves the overlap as a hole
 * (a pentagram's centre is empty) while a triangulation of the outline fills
 * it in. Those keep the scanline fill.
 */
static int poly_is_convex(const double *xs, const double *ys, int n) {
    int sign = 0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n, k = (i + 2) % n;
        double cross = (xs[j] - xs[i]) * (ys[k] - ys[j])
                     - (ys[j] - ys[i]) * (xs[k] - xs[j]);
        if (cross > 1e-9)      { if (sign < 0) return 0; sign = 1; }
        else if (cross < -1e-9) { if (sign > 0) return 0; sign = -1; }
    }
    return 1;
}

static double poly_area2(const double *xs, const double *ys, int n) {
    double a = 0;
    for (int i = 0, j = n - 1; i < n; j = i++)
        a += (xs[j] - xs[i]) * (ys[j] + ys[i]);
    return a;
}

static int seg_hits(double ax, double ay, double bx, double by,
                    double cx, double cy, double dx, double dy) {
    const double d1 = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    const double d2 = (bx - ax) * (dy - ay) - (by - ay) * (dx - ax);
    const double d3 = (dx - cx) * (ay - cy) - (dy - cy) * (ax - cx);
    const double d4 = (dx - cx) * (by - cy) - (dy - cy) * (bx - cx);
    return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
}

/* Only non-adjacent edges can cross in a simple polygon. O(n^2), but n is
 * capped at 64 and this runs once per polygon, not per pixel. */
static int poly_is_simple(const double *xs, const double *ys, int n) {
    for (int i = 0; i < n; i++) {
        const int i2 = (i + 1) % n;
        for (int j = i + 1; j < n; j++) {
            const int j2 = (j + 1) % n;
            if (i == j || i2 == j || j2 == i) continue;
            if (seg_hits(xs[i], ys[i], xs[i2], ys[i2],
                         xs[j], ys[j], xs[j2], ys[j2])) return 0;
        }
    }
    return 1;
}

static int pt_in_tri(double px, double py,
                     double ax, double ay, double bx, double by,
                     double cx, double cy) {
    const double d1 = (px - bx) * (ay - by) - (ax - bx) * (py - by);
    const double d2 = (px - cx) * (by - cy) - (bx - cx) * (py - cy);
    const double d3 = (px - ax) * (cy - ay) - (cx - ax) * (py - ay);
    const int neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    const int pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return !(neg && pos);
}

/* Ear clipping. Writes 3*(n-2) indices into `out`; returns the triangle
 * count, or 0 if the polygon could not be triangulated. */
static int poly_triangulate(const double *xs, const double *ys, int n, int *out) {
    int idx[64];
    const int ccw = poly_area2(xs, ys, n) > 0;
    for (int i = 0; i < n; i++) idx[i] = ccw ? i : (n - 1 - i);

    int m = n, tris = 0, guard = 0;
    while (m > 3 && guard++ < 64 * 64) {
        int clipped = 0;
        for (int i = 0; i < m; i++) {
            const int ia = idx[(i + m - 1) % m], ib = idx[i], ic = idx[(i + 1) % m];
            const double ax = xs[ia], ay = ys[ia];
            const double bx = xs[ib], by = ys[ib];
            const double cx = xs[ic], cy = ys[ic];
            /* convex corner? (consistent winding, so cross > 0) */
            if ((bx - ax) * (cy - ay) - (by - ay) * (cx - ax) <= 0) continue;
            /* no other vertex inside the candidate ear */
            int ok = 1;
            for (int k = 0; k < m && ok; k++) {
                const int iv = idx[k];
                if (iv == ia || iv == ib || iv == ic) continue;
                if (pt_in_tri(xs[iv], ys[iv], ax, ay, bx, by, cx, cy)) ok = 0;
            }
            if (!ok) continue;
            out[tris * 3 + 0] = ia; out[tris * 3 + 1] = ib; out[tris * 3 + 2] = ic;
            tris++;
            for (int k = i; k + 1 < m; k++) idx[k] = idx[k + 1];
            m--;
            clipped = 1;
            break;
        }
        if (!clipped) return 0;   /* no ear found: give up, caller falls back */
    }
    out[tris * 3 + 0] = idx[0]; out[tris * 3 + 1] = idx[1]; out[tris * 3 + 2] = idx[2];
    return tris + 1;
}

/* Filled circle, evaluated in the fragment shader and BATCHED.
 *
 * Coverage uses the software rasterizer's own rule --
 *   span = round(sqrt(r*r - dy*dy));  covered iff |dx| <= span
 * -- which is bit-exact at every radius. A triangle fan cannot be: it
 * approximates the boundary with straight edges, measured at ~0.5 px per
 * boundary pixel, which is 1% of the area at r=100 but 23% at r=4.
 *
 * The circle's centre and radius travel in the VERTEX (reusing u,v plus a
 * dedicated `rad` attribute) rather than a uniform. As a uniform each circle
 * needed its own draw call, and a cart drawing 900 of them spent 0.69 ms per
 * frame in GL call overhead -- slower than software drawing them outright.
 * Per-vertex, a whole frame of circles is one draw.
 */
int wcl_r2d_circle(int cx, int cy, int r, uint32_t color, int alpha) {
    if (!wcl_r2d_active() || r <= 0) return 0;
    if (textured_batch_count) flush_textured_batch();
    /* circle quads and plain solids cannot share a batch: the shader picks
     * its rule from u_textured, which is per-draw state */
    if (solid_batch_count && !solid_batch_is_circle) flush_solid_batch();
    if (solid_batch_count + 4 > BATCH_MAX * 4) flush_solid_batch();
    solid_batch_is_circle = 1;
    if (alpha < 255) solid_batch_has_alpha = 1;

    /* gl_FragCoord.y counts up from the bottom; the cart's y counts down. */
    const float fcy = (float)(current_target ? current_target->h : height) - 1.0f - (float)cy;
    vertex_t *v = &solid_batch[solid_batch_count];
    float x0, y0, x1, y1;
    ndc((float)(cx - r), (float)(cy - r), &x0, &y0);
    ndc((float)(cx + r + 1), (float)(cy + r + 1), &x1, &y1);
    const float rr = (float)((color >> 16) & 255) / 255.0f;
    const float gg = (float)((color >> 8) & 255) / 255.0f;
    const float bb = (float)(color & 255) / 255.0f;
    const float aa = (float)alpha / 255.0f;
    const float xs[4] = { x0, x1, x1, x0 };
    const float ys[4] = { y0, y0, y1, y1 };
    for (int i = 0; i < 4; i++) {
        v[i].x = xs[i]; v[i].y = ys[i];
        v[i].u = (float)cx; v[i].v = fcy; v[i].rad = (float)r;
        v[i].r = rr; v[i].g = gg; v[i].b = bb; v[i].a = aa;
    }
    solid_batch_count += 4;
    frame_stats.quads++;
    return 1;
}

/* Triangulation cache.
 *
 * Ear clipping is O(n^3) in the worst case and a cart re-submits the same
 * shape every frame: a 19-vertex comb costs ~3400 point-in-triangle tests.
 * The triangulation depends only on the vertex positions, so cache it
 * against a hash of them. Convex polygons skip all of this -- their fan is
 * derived directly.
 */
typedef struct {
    uint32_t hash;
    int n, ntri;
    int tri[3 * 64];
    int used;
} tricache_t;
#define MAX_TRICACHE 16
static tricache_t tricache[MAX_TRICACHE];
static int tricache_next;

static uint32_t poly_hash(const double *xs, const double *ys, int n) {
    uint32_t h = 2166136261u ^ (uint32_t)n;
    for (int i = 0; i < n; i++) {
        /* quantize to 1/16 px: far finer than any visible difference, and
         * stable against the float noise a transform stack introduces */
        const int32_t qx = (int32_t)(xs[i] * 16.0);
        const int32_t qy = (int32_t)(ys[i] * 16.0);
        h = (h ^ (uint32_t)qx) * 16777619u;
        h = (h ^ (uint32_t)qy) * 16777619u;
    }
    return h;
}

int wcl_r2d_poly(const double *xs, const double *ys, int n,
                 uint32_t color, int alpha) {
    if (!wcl_r2d_active() || n < 3 || n > 64) return 0;

    /* Simplicity is checked FIRST, before convexity.
     *
     * A pentagram turns the same way at every vertex, so poly_is_convex
     * calls it convex and it would take the fan path -- skipping the
     * self-intersection guard entirely and filling a centre that even-odd
     * leaves hollow. That was latent while only convex fills were on GL;
     * adding ear clipping is what surfaced it. Cheap enough at n <= 64 to
     * run unconditionally, and it is the correctness gate for both paths. */
    if (!poly_is_simple(xs, ys, n)) return 0;

    int tri[3 * 64];
    int ntri = 0;
    if (poly_is_convex(xs, ys, n)) {
        /* a fan needs no clipping and no cache */
        for (int i = 1; i + 1 < n; i++) {
            tri[ntri * 3 + 0] = 0; tri[ntri * 3 + 1] = i; tri[ntri * 3 + 2] = i + 1;
            ntri++;
        }
    } else {
        const uint32_t h = poly_hash(xs, ys, n);
        tricache_t *hit = NULL;
        for (int i = 0; i < MAX_TRICACHE; i++)
            if (tricache[i].used && tricache[i].hash == h && tricache[i].n == n) {
                hit = &tricache[i]; break;
            }
        if (hit) {
            ntri = hit->ntri;
            for (int i = 0; i < ntri * 3; i++) tri[i] = hit->tri[i];
        } else {
            ntri = poly_triangulate(xs, ys, n, tri);
            if (ntri <= 0) return 0;
            tricache_t *slot = &tricache[tricache_next];
            tricache_next = (tricache_next + 1) % MAX_TRICACHE;
            slot->hash = h; slot->n = n; slot->ntri = ntri; slot->used = 1;
            for (int i = 0; i < ntri * 3; i++) slot->tri[i] = tri[i];
        }
    }
    if (ntri <= 0) return 0;
    flush_batches();

    vertex_t v[3 * 64];
    float r = (float)((color >> 16) & 255) / 255.0f;
    float g = (float)((color >> 8) & 255) / 255.0f;
    float b = (float)(color & 255) / 255.0f;
    float a = (float)alpha / 255.0f;
    int vc = 0;
    for (int t = 0; t < ntri * 3; t++) {
        ndc((float)xs[tri[t]], (float)ys[tri[t]], &v[vc].x, &v[vc].y);
        v[vc].u = v[vc].v = 0; v[vc].rad = 0;
        v[vc].r = r; v[vc].g = g; v[vc].b = b; v[vc].a = a;
        vc++;
    }
    set_blend(alpha < 255);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(vertex_t) * vc), v, GL_DYNAMIC_DRAW);
    set_textured(0);
    glDrawArrays(GL_TRIANGLES, 0, vc);
    frame_stats.draws++;
    frame_stats.quads += (uint32_t)ntri;
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
        v[i].u = v[i].v = 0; v[i].rad = 0; v[i].rad = 0;
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
        WC_LOG("atlas-upload");
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
        v[i].u = us[i]; v[i].v = vs[i]; v[i].rad = 0;
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
