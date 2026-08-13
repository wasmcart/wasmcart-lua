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
#include "render3d_gl.h"

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
    /* Lazily attached, and ONLY if a cart actually stencils into this
     * canvas. Colour-only FBOs stay colour-only, which is what keeps
     * stencil support free for the carts that never touch it. */
    GLuint stencil_rb;
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
#define ATLAS_SIZE 4096   /* 2048 (4.2M px) was sized for 720p; a 1080p cart's art set exceeds it */
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
/* The full LOVE blend mode, not just add/not-add. Kept alongside
 * blend_add_on because the batchers still branch on "is this additive". */
static int blend_mode_cur = WCL_BLEND_ALPHA;
/* Defined with the host-viewport block further down; declared here because
 * wcl_r2d__screen_viewport sits above it and must restore the same rect. */
static void restore_screen_viewport(void);
static int blend_premult_cur = 0;
static void flush_batches(void);
/* Custom shaders (defined further down). Declared here because set_textured
 * and bind_texture, which sit above them, must consult the bound program. */
/* 8 was sized for a 2D cart, where a shader is an effect and two or three is
 * a lot. A deferred renderer is the opposite: it compiles one program per
 * PASS and per material variant -- geometry, lighting, shadow, SSAO, blur,
 * sky, post -- and 3DreamEngine alone calls newShader 13 times before it
 * draws anything. Each slot is a handle and a few ints; the programs
 * themselves live in the driver either way. */
#define MAX_SHADERS 64
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
/* The 3D pipeline's one extra attribute, sharing index 3 with a_rad: a
 * program declares the 2D set or the 3D set, never both. render3d_gl.c
 * mirrors these as A3_*, and the static asserts there hold the two in step
 * -- a silent disagreement would render colour from the normal attribute,
 * which looks plausible rather than broken. */
#define ATTR_NORMAL 3

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
 * away, because every draw path binds to it.
 *
 * 15 rather than 4 because a deferred renderer's lighting pass samples the
 * whole g-buffer at once -- albedo, normal, depth, material, plus shadow
 * maps and a environment cubemap -- and 4 units is not a shader that has to
 * be split, it is a renderer that cannot be written. GLES 3.0 guarantees at
 * least 16 fragment texture units, so 15 plus the engine's unit 0 is the
 * whole guaranteed set.
 *
 * NOTE: wcl_r3d_target_send hands out units from this same 1..15 space for
 * render targets bound as samplers. The two allocators must not overlap;
 * see the unit assignment there. */
#define SHADER_TEX_UNITS 15
typedef struct {
    GLint location;
    GLuint tex;
    /* The texture TARGET this sampler binds to. A render target may be a
     * cubemap, an array or a volume, and rebinding one as GL_TEXTURE_2D on a
     * program switch silently leaves the sampler reading nothing. */
    GLenum textarget;
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
    /* LOVE compiles ONE source into BOTH stages and defines VERTEX or
     * PIXEL so the shader can guard the half that belongs to each. That
     * is not a nicety -- it is how a single-file LOVE shader is written,
     * and without the define an `#ifdef PIXEL` body simply vanishes,
     * leaving a shader with no effect() and a syntax error pointing at
     * whatever followed. 19 of 3DreamEngine's shaders are written this
     * way. */
    "#define PIXEL 1\n"
    /* highp, matching the vertex stage's default. A uniform a cart declares
     * once in a source compiled into BOTH stages must come out the same
     * precision on each or the program fails to link. */
    "precision highp float;\n"
    "precision highp int;\n"
    "in vec2 v_uv;\n"
    "in vec4 v_color;\n"
    "in highp vec2 v_centre;\n"
    "in highp float v_rad;\n"
    "out vec4 wc_frag_color;\n"
    "uniform sampler2D u_tex;\n"
    "uniform int u_textured;\n"
    "uniform highp vec2 love_ScreenSize;\n"
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
    /* LOVE fragment built-ins. love_PixelCoord is the window-space pixel
     * centre -- LOVE's spelling of gl_FragCoord.xy -- and post-process
     * shaders use it constantly to sample by pixel rather than by uv.
     * Missing it fails as "`love_PixelCoord' undeclared" at a line the
     * author wrote correctly. */
    "#define love_PixelCoord (gl_FragCoord.xy)\n"
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
    /* LOVE compiles ONE source into BOTH stages and defines VERTEX or
     * PIXEL so the shader can guard the half that belongs to each. That
     * is not a nicety -- it is how a single-file LOVE shader is written,
     * and without the define an `#ifdef PIXEL` body simply vanishes,
     * leaving a shader with no effect() and a syntax error pointing at
     * whatever followed. 19 of 3DreamEngine's shaders are written this
     * way. */
    "#define VERTEX 1\n"
    "in vec4 VertexPosition;\n"
    "in vec4 VertexTexCoord;\n"
    "in vec4 VertexColor;\n"
    "in highp float a_rad;\n"
    "#define a_pos VertexPosition.xy\n"
    "#define a_uv VertexTexCoord.xy\n"
    "#define a_color VertexColor\n"
    "out vec2 v_uv;\n"
    "out vec4 v_color;\n"
    "out highp vec2 v_centre;\n"
    "out highp float v_rad;\n"
    /* mediump to match the fragment stage's declaration of the same uniform;
     * see the note in SHADER_VERT3D_PROLOGUE. A cart shader that declares
     * love_ScreenSize in both stages links only if they agree. */
    "uniform highp vec2 love_ScreenSize;\n"
    "#define Image sampler2D\n"
    "#define CubeImage samplerCube\n"
    "#define ArrayImage sampler2DArray\n"
    "#define VolumeImage sampler3D\n"
    "#define Texel texture\n"
    "#define number float\n"
    "#define extern uniform\n"
    /* Real declarations, NOT #defines -- see the note in
     * SHADER_VERT3D_PROLOGUE. A LOVE shader spells its entry point
     *     vec4 position(mat4 transform_projection, vec4 VertexPosition)
     * and a macro expands inside that parameter list, producing
     * `vec4 vec4(a_pos, 0.0, 1.0)` and a syntax error in generated code.
     *
     * The 2D VBO supplies 2 floats for position; GL fills the missing
     * components with (0, 1), which is exactly what the old macro spelled
     * out. Same for VertexTexCoord's z/w. */
    "#line 1\n";

static const char *SHADER_VERT_EPILOGUE =
    "\nvoid main() {\n"
    "  v_uv = a_uv; v_color = a_color;\n"
    "  v_centre = a_uv; v_rad = a_rad;\n"
    "  gl_Position = position(mat4(1.0), vec4(a_pos, 0.0, 1.0));\n"
    "}\n";

/* ── the 3D variants ──────────────────────────────────────────────────
 *
 * A 3D shader differs from the 2D one in exactly two ways, and both are
 * forced by the vertex format rather than chosen:
 *
 *   * VertexPosition is a real vec4 from a vec3 attribute, not a synthesized
 *     vec4(a_pos, 0, 1). A 3D cart's position() multiplies it by its own
 *     matrices, so the z it gets has to be the model's z.
 *   * VertexNormal exists. It is the one attribute LOVE has no built-in name
 *     for -- g3d declares it in the shader itself -- so it is declared here
 *     and the cart's own redeclaration is stripped (see the rewrite below).
 *
 * transform_projection is the IDENTITY here, as in 2D, and for the same
 * reason: this engine has no matrix stack, and a 3D cart supplies its own
 * projection/view/model uniforms. That matches how g3d works -- it ignores
 * the passed matrix entirely and uses its own three. */
static const char *SHADER_VERT3D_PROLOGUE =
    "#version 300 es\n"
    /* A DEFAULT float precision for the vertex stage too.
     *
     * The vertex stage defaults to highp and the fragment stage to
     * mediump, so a uniform a cart declares ONCE in a source compiled
     * into both stages -- which is the standard single-file LOVE
     * shader -- gets a different precision in each and the program
     * fails to LINK ("declared as type `float16_t' and type
     * `float'"). LOVE emits a matching default in both stages for
     * exactly this reason.
     *
     * highp on BOTH sides rather than mediump: this is 3D, and
     * mediump positions visibly jitter. GLES 3.0 guarantees highp in
     * the fragment stage, so it is safe to ask for. */
    "precision highp float;\n"
    "precision highp int;\n"
    /* LOVE compiles ONE source into BOTH stages and defines VERTEX or
     * PIXEL so the shader can guard the half that belongs to each. That
     * is not a nicety -- it is how a single-file LOVE shader is written,
     * and without the define an `#ifdef PIXEL` body simply vanishes,
     * leaving a shader with no effect() and a syntax error pointing at
     * whatever followed. 19 of 3DreamEngine's shaders are written this
     * way. */
    "#define VERTEX 1\n"
    /* Precision is NOT declared globally here: the vertex stage defaults to
     * highp, and 3D positions need it. Dropping the whole stage to mediump
     * to match the fragment stage would put ~10 bits of mantissa under a
     * projected coordinate, which shows up as geometry jitter that looks
     * like a bad matrix rather than a precision bug.
     *
     * Only the SHARED uniforms are pinned, and they must be: a uniform
     * declared in both stages has to agree on precision or the program fails
     * to LINK, with "declared as type `f16vec2' and type `vec2'" -- naming a
     * type neither prologue contains. The fragment stage is mediump, so
     * these say mediump on both sides. */
    /* Declared as real attributes, NOT #defines.
     *
     * A #define expands wherever the token appears -- including inside a
     * function's PARAMETER LIST. LOVE shaders are written as
     *     vec4 position(mat4 transform_projection, vec4 VertexPosition)
     * and with VertexPosition #defined to vec4(a_pos3,1.0) that becomes
     * `vec4 vec4(a_pos3, 1.0)`, a syntax error inside generated code the
     * cart author never saw. Real `in` declarations shadow cleanly instead:
     * a parameter of the same name simply hides the global, which is what
     * the shader intends. */
    "in vec4 VertexPosition;\n"
    "in vec2 VertexTexCoord;\n"
    "in vec3 VertexNormal;\n"
    "in vec4 VertexColor;\n"
    "out vec2 v_uv;\n"
    "out vec4 v_color;\n"
    "out vec3 v_normal;\n"
    "uniform highp vec2 love_ScreenSize;\n"
    "uniform highp vec4 love_Color;\n"
    "#define Image sampler2D\n"
    "#define Texel texture\n"
    "#define number float\n"
    "#define extern uniform\n"
    "#line 1\n";

static const char *SHADER_VERT3D_EPILOGUE =
    "\nvoid main() {\n"
    "  v_uv = VertexTexCoord; v_color = VertexColor * love_Color;\n"
    "  v_normal = VertexNormal;\n"
    "  gl_Position = position(mat4(1.0), VertexPosition);\n"
    "}\n";

/* The 3D fragment side. Texel() does NOT have the 2D version's u_textured
 * dance for glyph/circle modes -- there are no glyphs or circles in a 3D
 * draw -- but it keeps the untextured rule, so an untextured model's
 * `Texel(tex, uv) * color` reduces to the vertex colour instead of sampling
 * whatever texture happened to be bound. */
static const char *SHADER_FRAG3D_PROLOGUE =
    "#version 300 es\n"
    /* A DEFAULT float precision for the vertex stage too.
     *
     * The vertex stage defaults to highp and the fragment stage to
     * mediump, so a uniform a cart declares ONCE in a source compiled
     * into both stages -- which is the standard single-file LOVE
     * shader -- gets a different precision in each and the program
     * fails to LINK ("declared as type `float16_t' and type
     * `float'"). LOVE emits a matching default in both stages for
     * exactly this reason.
     *
     * highp on BOTH sides rather than mediump: this is 3D, and
     * mediump positions visibly jitter. GLES 3.0 guarantees highp in
     * the fragment stage, so it is safe to ask for. */
    "precision highp float;\n"
    "precision highp int;\n"
    /* LOVE compiles ONE source into BOTH stages and defines VERTEX or
     * PIXEL so the shader can guard the half that belongs to each. That
     * is not a nicety -- it is how a single-file LOVE shader is written,
     * and without the define an `#ifdef PIXEL` body simply vanishes,
     * leaving a shader with no effect() and a syntax error pointing at
     * whatever followed. 19 of 3DreamEngine's shaders are written this
     * way. */
    "#define PIXEL 1\n"
    "in vec2 v_uv;\n"
    "in vec4 v_color;\n"
    "in vec3 v_normal;\n"
    "out vec4 wc_frag_color;\n"
    "uniform sampler2D u_tex;\n"
    "uniform int u_textured;\n"
    "uniform highp vec2 love_ScreenSize;\n"
    "uniform highp vec4 love_Color;\n"
    "#define Image sampler2D\n"
    "#define CubeImage samplerCube\n"
    "#define ArrayImage sampler2DArray\n"
    "#define VolumeImage sampler3D\n"
    "#define VaryingColor v_color\n"
    "#define VaryingTexCoord vec4(v_uv, 0.0, 1.0)\n"
    "#define VertexNormal v_normal\n"
    "#define number float\n"
    "#define extern uniform\n"
    "vec4 Texel(sampler2D t, vec2 uv) {\n"
    "  if (u_textured == 1) return texture(t, uv);\n"
    "  return vec4(1.0);\n"
    "}\n"
    /* LOVE fragment built-ins. love_PixelCoord is the window-space
     * pixel centre -- LOVE's spelling of gl_FragCoord.xy -- and post-
     * process shaders use it constantly to sample by pixel rather than
     * by uv. Missing it fails as "`love_PixelCoord' undeclared" at a
     * line the author wrote correctly. */
    "#define love_PixelCoord (gl_FragCoord.xy)\n"
    "#line 1\n";

static const char *SHADER_FRAG3D_EPILOGUE =
    "\nvoid main() {\n"
    "  wc_frag_color = effect(v_color, u_tex, v_uv, gl_FragCoord.xy);\n"
    "}\n";

/* ── multiple render targets in a fragment shader ─────────────────────
 *
 * effect() returns ONE colour, which is the whole shape of LOVE's shader
 * API and cannot express a geometry pass that writes albedo, normals and
 * material to three attachments at once.
 *
 * LOVE's own answer is `void effect(out vec4 c[N])`; this engine spells it
 *     #pragma wasmcart mrt N
 *     void effect2(out vec4 c0, out vec4 c1) { ... }
 * with one `out` parameter per attachment, because a GLSL ES 3.00 array of
 * `out` parameters is awkward to declare portably and naming them makes the
 * attachment index explicit at the call site.
 *
 * The prologue declares N outputs at pinned locations and the epilogue calls
 * the cart's function with them. Locations MUST be explicit: without them a
 * linker is free to assign output 0 to attachment 1, and the resulting
 * g-buffer is subtly wrong rather than blank.
 *
 * Built per output-count, since the declarations differ. */
static const char *MRT_OUT_DECLS[9] = {
    "", /* 0 unused */
    "layout(location = 0) out vec4 wc_out0;\n",
    "layout(location = 0) out vec4 wc_out0;\n"
    "layout(location = 1) out vec4 wc_out1;\n",
    "layout(location = 0) out vec4 wc_out0;\n"
    "layout(location = 1) out vec4 wc_out1;\n"
    "layout(location = 2) out vec4 wc_out2;\n",
    "layout(location = 0) out vec4 wc_out0;\n"
    "layout(location = 1) out vec4 wc_out1;\n"
    "layout(location = 2) out vec4 wc_out2;\n"
    "layout(location = 3) out vec4 wc_out3;\n",
    "layout(location = 0) out vec4 wc_out0;\n"
    "layout(location = 1) out vec4 wc_out1;\n"
    "layout(location = 2) out vec4 wc_out2;\n"
    "layout(location = 3) out vec4 wc_out3;\n"
    "layout(location = 4) out vec4 wc_out4;\n",
    "layout(location = 0) out vec4 wc_out0;\n"
    "layout(location = 1) out vec4 wc_out1;\n"
    "layout(location = 2) out vec4 wc_out2;\n"
    "layout(location = 3) out vec4 wc_out3;\n"
    "layout(location = 4) out vec4 wc_out4;\n"
    "layout(location = 5) out vec4 wc_out5;\n",
    "layout(location = 0) out vec4 wc_out0;\n"
    "layout(location = 1) out vec4 wc_out1;\n"
    "layout(location = 2) out vec4 wc_out2;\n"
    "layout(location = 3) out vec4 wc_out3;\n"
    "layout(location = 4) out vec4 wc_out4;\n"
    "layout(location = 5) out vec4 wc_out5;\n"
    "layout(location = 6) out vec4 wc_out6;\n",
    "layout(location = 0) out vec4 wc_out0;\n"
    "layout(location = 1) out vec4 wc_out1;\n"
    "layout(location = 2) out vec4 wc_out2;\n"
    "layout(location = 3) out vec4 wc_out3;\n"
    "layout(location = 4) out vec4 wc_out4;\n"
    "layout(location = 5) out vec4 wc_out5;\n"
    "layout(location = 6) out vec4 wc_out6;\n"
    "layout(location = 7) out vec4 wc_out7;\n",
};

/* Two ways to write a multi-target fragment shader, and BOTH are supported
 * because both exist in the wild:
 *
 *   void effect() { love_Canvases[0] = ...; love_Canvases[1] = ...; }
 *       LOVE's own form, and what real LOVE renderers are written against
 *       (3DreamEngine's sky shaders use exactly this). `love_Canvases` is a
 *       writable array of the outputs.
 *
 *   void effect2(out vec4 c0, out vec4 c1) { ... }
 *       This engine's named-parameter form, which makes the attachment
 *       index explicit at the call site.
 *
 * The epilogue below declares love_Canvases as an alias array over the
 * pinned outputs and calls whichever function the cart defined. Which one
 * that is has to be decided by the caller (it can see the source), so the
 * two epilogue families are separate. */
static const char *MRT_EPILOGUES[9] = {
    "",
    "\nvoid main() { effect2(wc_out0); }\n",
    "\nvoid main() { effect2(wc_out0, wc_out1); }\n",
    "\nvoid main() { effect2(wc_out0, wc_out1, wc_out2); }\n",
    "\nvoid main() { effect2(wc_out0, wc_out1, wc_out2, wc_out3); }\n",
    "\nvoid main() { effect2(wc_out0, wc_out1, wc_out2, wc_out3, wc_out4); }\n",
    "\nvoid main() { effect2(wc_out0, wc_out1, wc_out2, wc_out3, wc_out4,"
    " wc_out5); }\n",
    "\nvoid main() { effect2(wc_out0, wc_out1, wc_out2, wc_out3, wc_out4,"
    " wc_out5, wc_out6); }\n",
    "\nvoid main() { effect2(wc_out0, wc_out1, wc_out2, wc_out3, wc_out4,"
    " wc_out5, wc_out6, wc_out7); }\n",
};

/* The love_Canvases form. GLSL ES 3.00 has no array-of-references, so the
 * declared outputs are copied out of a local array after effect() runs --
 * which is also what LOVE's own generated main() does. */
static const char *MRT_CANVAS_DECLS[9] = {
    "",
    "vec4 love_Canvases[1];\n",
    "vec4 love_Canvases[2];\n",
    "vec4 love_Canvases[3];\n",
    "vec4 love_Canvases[4];\n",
    "vec4 love_Canvases[5];\n",
    "vec4 love_Canvases[6];\n",
    "vec4 love_Canvases[7];\n",
    "vec4 love_Canvases[8];\n",
};

static const char *MRT_CANVAS_EPILOGUES[9] = {
    "",
    "\nvoid main() { effect();\n  wc_out0 = love_Canvases[0];\n}\n",
    "\nvoid main() { effect();\n  wc_out0 = love_Canvases[0];"
    "  wc_out1 = love_Canvases[1];\n}\n",
    "\nvoid main() { effect();\n  wc_out0 = love_Canvases[0];"
    "  wc_out1 = love_Canvases[1];  wc_out2 = love_Canvases[2];\n}\n",
    "\nvoid main() { effect();\n  wc_out0 = love_Canvases[0];"
    "  wc_out1 = love_Canvases[1];  wc_out2 = love_Canvases[2];"
    "  wc_out3 = love_Canvases[3];\n}\n",
    "\nvoid main() { effect();\n  wc_out0 = love_Canvases[0];"
    "  wc_out1 = love_Canvases[1];  wc_out2 = love_Canvases[2];"
    "  wc_out3 = love_Canvases[3];  wc_out4 = love_Canvases[4];\n}\n",
    "\nvoid main() { effect();\n  wc_out0 = love_Canvases[0];"
    "  wc_out1 = love_Canvases[1];  wc_out2 = love_Canvases[2];"
    "  wc_out3 = love_Canvases[3];  wc_out4 = love_Canvases[4];"
    "  wc_out5 = love_Canvases[5];\n}\n",
    "\nvoid main() { effect();\n  wc_out0 = love_Canvases[0];"
    "  wc_out1 = love_Canvases[1];  wc_out2 = love_Canvases[2];"
    "  wc_out3 = love_Canvases[3];  wc_out4 = love_Canvases[4];"
    "  wc_out5 = love_Canvases[5];  wc_out6 = love_Canvases[6];\n}\n",
    "\nvoid main() { effect();\n  wc_out0 = love_Canvases[0];"
    "  wc_out1 = love_Canvases[1];  wc_out2 = love_Canvases[2];"
    "  wc_out3 = love_Canvases[3];  wc_out4 = love_Canvases[4];"
    "  wc_out5 = love_Canvases[5];  wc_out6 = love_Canvases[6];"
    "  wc_out7 = love_Canvases[7];\n}\n",
};

/* The MRT fragment prologue is the 3D one minus its single `out`, plus the
 * N declared outputs. Built into a static buffer at newShader time. */
static const char *SHADER_FRAG_MRT_HEAD =
    "#version 300 es\n"
    /* A DEFAULT float precision for the vertex stage too.
     *
     * The vertex stage defaults to highp and the fragment stage to
     * mediump, so a uniform a cart declares ONCE in a source compiled
     * into both stages -- which is the standard single-file LOVE
     * shader -- gets a different precision in each and the program
     * fails to LINK ("declared as type `float16_t' and type
     * `float'"). LOVE emits a matching default in both stages for
     * exactly this reason.
     *
     * highp on BOTH sides rather than mediump: this is 3D, and
     * mediump positions visibly jitter. GLES 3.0 guarantees highp in
     * the fragment stage, so it is safe to ask for. */
    "precision highp float;\n"
    "precision highp int;\n"
    /* LOVE compiles ONE source into BOTH stages and defines VERTEX or
     * PIXEL so the shader can guard the half that belongs to each. That
     * is not a nicety -- it is how a single-file LOVE shader is written,
     * and without the define an `#ifdef PIXEL` body simply vanishes,
     * leaving a shader with no effect() and a syntax error pointing at
     * whatever followed. 19 of 3DreamEngine's shaders are written this
     * way. */
    "#define PIXEL 1\n"
    "in vec2 v_uv;\n"
    "in vec4 v_color;\n"
    "in vec3 v_normal;\n"
    "uniform sampler2D u_tex;\n"
    "uniform int u_textured;\n"
    "uniform highp vec2 love_ScreenSize;\n"
    "uniform highp vec4 love_Color;\n"
    "#define Image sampler2D\n"
    "#define CubeImage samplerCube\n"
    "#define ArrayImage sampler2DArray\n"
    "#define VolumeImage sampler3D\n"
    "#define VaryingColor v_color\n"
    "#define VaryingTexCoord vec4(v_uv, 0.0, 1.0)\n"
    "#define VertexNormal v_normal\n"
    "#define number float\n"
    "#define extern uniform\n"
    "vec4 Texel(sampler2D t, vec2 uv) {\n"
    "  if (u_textured == 1) return texture(t, uv);\n"
    "  return vec4(1.0);\n"
    "}\n"
    /* LOVE fragment built-ins. love_PixelCoord is the window-space
     * pixel centre -- LOVE's spelling of gl_FragCoord.xy -- and post-
     * process shaders use it constantly to sample by pixel rather than
     * by uv. Missing it fails as "`love_PixelCoord' undeclared" at a
     * line the author wrote correctly. */
    "#define love_PixelCoord (gl_FragCoord.xy)\n";

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

/* ── GLSL ES 1.00 -> 3.00 rewriting ───────────────────────────────────
 *
 * LOVE accepts shaders written in the old spellings and rewrites them before
 * handing them to the driver. This engine has to do the same, because that
 * is what every LOVE shader in the wild -- and every LOVE 3D library -- is
 * written in. g3d's vertex shader is the exact case:
 *
 *     attribute vec3 VertexNormal;      -> declared by our prologue already
 *     varying vec4 worldPosition;       -> out (vertex) / in (fragment)
 *
 * Three rules, applied to whole identifiers only:
 *
 *   attribute -> a comment. The vertex attributes this engine supplies are
 *       already declared in the prologue with pinned locations, so a cart's
 *       redeclaration would be a duplicate. Commenting the line out (rather
 *       than mapping it to `in`) is what lets g3d's `attribute vec3
 *       VertexNormal;` coexist with our own declaration of it.
 *   varying   -> `out` in a vertex shader, `in` in a fragment shader. Same
 *       storage qualifier, opposite direction, which is precisely the
 *       distinction GLSL 3.00 introduced.
 *   texture2D -> texture. A straight rename.
 *
 * WHOLE IDENTIFIERS ONLY. A substring replace would corrupt a cart's
 * `varyingScale` uniform or a `texture2DArray` call into something that no
 * longer parses, and the error would point into generated code. is_ident_ch
 * guards both ends of every match.
 *
 * Returns 1 if anything was rewritten. Writes into `dst`, which must hold
 * `cap` bytes; on overflow it returns -1 and the caller refuses the shader
 * rather than compiling a truncated one. */
static int is_ident_ch(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int shader_rewrite_es100(const char *src, char *dst, size_t cap,
                                int is_vertex) {
    size_t o = 0;
    int changed = 0;
    const char *p = src;

#define EMIT(str) do {                                    \
        size_t _n = strlen(str);                          \
        if (o + _n >= cap) return -1;                     \
        memcpy(dst + o, (str), _n); o += _n;              \
    } while (0)
#define COPY1() do { if (o + 1 >= cap) return -1; dst[o++] = *p++; } while (0)

    while (*p) {
        /* COMMENTS ARE COPIED THROUGH VERBATIM, never scanned.
         *
         * Found the hard way: a shader whose comment read
         *     vec3 n = VertexNormal;   // keep the attribute live
         * matched `attribute` INSIDE the comment, and since the rewrite for
         * `attribute` deletes through the next semicolon, it ate the rest of
         * the function -- including its `return`. The driver then reported
         * "function `position' has non-void return type vec4, but no return
         * statement", pointing at a line the author wrote correctly.
         *
         * Any prose containing the words attribute, varying or texture2D is
         * enough to trigger it, which is most shaders that explain
         * themselves. */
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n') COPY1();
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            COPY1(); COPY1();
            while (*p && !(p[0] == '*' && p[1] == '/')) COPY1();
            if (*p) { COPY1(); COPY1(); }
            continue;
        }
        /* A preprocessor directive is copied whole for the same reason: an
         * #if branch that is not taken still gets scanned by this rewriter,
         * and the tokens inside it are not necessarily live code.
         *
         * EXCEPT `#pragma language ...`, which is LOVE's own directive, not
         * GLSL's. LOVE reads it to pick a shader language version and strips
         * it before the driver ever sees it; passing it through produces
         * "syntax error, unexpected NEW_IDENTIFIER" pointing at a line the
         * author wrote correctly. This engine always emits "#version 300 es"
         * (GLSL ES 3.00), which is what `glsl3` asks for anyway. */
        if (*p == '#') {
            const char *line_end = p;
            while (*line_end && *line_end != '\n') line_end++;
            int is_love_pragma = 0;
            if (!strncmp(p, "#pragma", 7)) {
                const char *q = p + 7;
                while (*q == ' ' || *q == '\t') q++;
                if (!strncmp(q, "language", 8)) is_love_pragma = 1;
            }
            if (is_love_pragma) {
                changed = 1;
                p = line_end;          /* drop it; the newline is copied below */
                continue;
            }
            while (*p && *p != '\n') COPY1();
            continue;
        }

        /* Only try to match at an identifier BOUNDARY, so `myvarying` and
         * `texture2DArray` are never touched. */
        int at_boundary = (p == src) || !is_ident_ch(p[-1]);
        if (at_boundary) {
            if (!strncmp(p, "attribute", 9) && !is_ident_ch(p[9])) {
                /* An `attribute` declaration is either a DUPLICATE of one
                 * the prologue already supplies, or a CUSTOM one this engine
                 * knows nothing about.
                 *
                 *   duplicate -> drop it. The prologue declares the built-in
                 *       names, and a second declaration is a compile error.
                 *   custom    -> rewrite to `in`. A declared vertex format
                 *       can carry arbitrary attributes (VertexTangent,
                 *       InstancePosition, ...) and the shader must really
                 *       declare them or they do not exist.
                 *
                 * Getting this backwards is silent both ways: dropping a
                 * custom declaration leaves an undeclared identifier, and
                 * keeping a duplicate fails to compile at a line the author
                 * did not write.
                 *
                 * The semicolon is found by SCANNING PAST COMMENTS rather
                 * than with strchr: `attribute vec3 N; // a; b` would
                 * otherwise be cut at the semicolon inside the comment,
                 * leaving `b` dangling as a statement fragment. */
                const char *q = p + 9;
                while (*q && *q != ';') {
                    if (q[0] == '/' && q[1] == '/') {
                        while (*q && *q != '\n') q++;
                    } else if (q[0] == '/' && q[1] == '*') {
                        q += 2;
                        while (*q && !(q[0] == '*' && q[1] == '/')) q++;
                        if (*q) q += 2;
                    } else {
                        q++;
                    }
                }
                /* Is the declared NAME one the prologue already supplies?
                 * The name is the last identifier before the semicolon. */
                const char *e = q;
                while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n')) e--;
                const char *name_end = e;
                while (e > p && is_ident_ch(e[-1])) e--;
                size_t nlen = (size_t)(name_end - e);
                static const char *builtin[] = {
                    "VertexPosition", "VertexTexCoord", "VertexColor",
                    "VertexNormal", NULL
                };
                int is_builtin = 0;
                for (int bi = 0; builtin[bi]; bi++) {
                    if (strlen(builtin[bi]) == nlen &&
                        !strncmp(e, builtin[bi], nlen)) { is_builtin = 1; break; }
                }
                if (is_builtin) {
                    EMIT("/* attribute declared by the engine */");
                    p = *q ? q + 1 : q;
                } else {
                    /* Custom: keep the declaration, as `in`. */
                    EMIT("in");
                    p += 9;
                }
                changed = 1;
                continue;
            }
            if (!strncmp(p, "varying", 7) && !is_ident_ch(p[7])) {
                EMIT(is_vertex ? "out" : "in");
                changed = 1;
                p += 7;
                continue;
            }
            if (!strncmp(p, "texture2D", 9) && !is_ident_ch(p[9])) {
                EMIT("texture");
                changed = 1;
                p += 9;
                continue;
            }
        }
        if (o + 1 >= cap) return -1;
        dst[o++] = *p++;
    }
#undef EMIT
    if (o >= cap) return -1;
    dst[o] = 0;
    return changed;
}

/* Pin the attribute indices, then link. Shared by the default program and
 * every cart shader so one VAO stays valid across all of them. */
static void bind_attribs_and_link(GLuint prog) {
    glBindAttribLocation(prog, ATTR_POS, "a_pos");
    glBindAttribLocation(prog, ATTR_UV, "a_uv");
    glBindAttribLocation(prog, ATTR_COLOR, "a_color");
    glBindAttribLocation(prog, ATTR_RAD, "a_rad");
    /* The 3D names. Position, uv and colour share their 2D indices; the one
     * genuinely new attribute, a_normal, takes the index the 2D layout uses
     * for a_rad. That reuse is deliberate and is why the two sets can share
     * this one function: a program declares one set or the other, never
     * both, and each pipeline binds its own VAO before drawing (see the A3_*
     * block in render3d_gl.c). Binding a name the program does not declare
     * is a no-op in GL, so one unconditional list serves both.
     *
     * A_COLOR MUST STAY AT ITS 2D INDEX. The 3D VAO's colour pointer is set
     * up at A3_COLOR, so if this pinned a_color anywhere else the shader
     * would read colour from whatever the normal attribute supplies -- a
     * mesh drawn in the colours of its own surface directions, which looks
     * deliberate enough to survive review. Hence A3_COLOR == ATTR_RAD's
     * neighbour is NOT how these line up; the asserts below hold them. */
    /* And every attribute name a declared vertex format has registered, by
     * its LOVE name (VertexPosition, VertexTangent, ...). A shader written
     * against a custom format declares those names directly, and without an
     * explicit binding the linker is free to put VertexTangent on the index
     * the VBO fills with positions -- which renders, wrongly, silently. */
    wcl_r3d_bind_format_attribs((unsigned int)prog);
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
     * generated preamble, so catch them by name instead.
     *
     * NOTE what is NOT here: `attribute`, `varying`, `texture2D`. Those are
     * GLSL ES 1.00 spellings, and LOVE's own shader preprocessor rewrites
     * them rather than refusing them -- so every LOVE shader in the wild,
     * and every LOVE 3D library without exception, is written with them.
     * Refusing them by name rejected the entire ecosystem this engine is
     * trying to run. They are rewritten in shader_rewrite_es100 below.
     * `gl_FragColor` stays refused: it has no meaning inside effect(), which
     * RETURNS its colour, so rewriting it would produce a shader that
     * compiles and draws nothing. */
    static const char *banned[] = {
        "layout(local_size", "gl_GlobalInvocationID", "imageStore", "imageLoad",
        "gl_FragColor", NULL
    };
    static const char *why[] = {
        "compute shaders are not in GLES 3.0",
        "compute shaders are not in GLES 3.0",
        "image load/store is GLES 3.1+, not available here",
        "image load/store is GLES 3.1+, not available here",
        "gl_FragColor is GLSL ES 1.00; return the colour from effect() instead",
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

int wcl_r2d_shader_new(const char *pixel_src, const char *vertex_src, int is_3d,
                       int mrt_outputs) {
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

    /* Rewrite GLSL ES 1.00 spellings before composing. The rewrite buffer is
     * separate from shader_src_buf, which holds the COMPOSED source; both
     * are static and single-use, which is safe because newShader is not
     * reentrant. */
    static char rewrite_buf[SHADER_SRC_MAX];

    GLuint vs = 0, fs = 0;
    if (vertex_src) {
        if (shader_reject_unsupported(vertex_src, "vertex")) return -1;
        int r = shader_rewrite_es100(vertex_src, rewrite_buf, sizeof rewrite_buf, 1);
        if (r < 0) {
            shader_log("love.graphics.newShader: the vertex shader is too "
                       "large after rewriting GLSL ES 1.00 spellings "
                       "(limit is 16 KB of cart GLSL)");
            return -1;
        }
        if (!shader_compose(is_3d ? SHADER_VERT3D_PROLOGUE : SHADER_VERT_PROLOGUE,
                            rewrite_buf,
                            is_3d ? SHADER_VERT3D_EPILOGUE : SHADER_VERT_EPILOGUE))
            return -1;
#ifdef WCL_DUMP_SHADER
        { /* the exact source the driver saw, with line numbers */
            int ln = 1; char b[128];
            const char *q = shader_src_buf;
            while (*q) {
                const char *e = strchr(q, '\n');
                int len = e ? (int)(e - q) : (int)strlen(q);
                if (ln >= 160 && ln <= 175) {
                    int k = snprintf(b, sizeof b, "V%03d| %.*s", ln,
                                     len > 90 ? 90 : len, q);
                    if (k > 0) wc_log(b, (unsigned)k);
                }
                ln++; if (!e) break; q = e + 1;
            }
        }
#endif
        if (!shader_compile_checked(GL_VERTEX_SHADER, shader_src_buf, &vs)) return -1;
    } else if (is_3d && mrt_outputs == 0) {
        /* A 3D program with no cart vertex shader has no transform at all,
         * so it would draw model-space coordinates as clip space. Refuse it
         * here rather than render a shape that is technically on screen.
         *
         * NOT for an MRT shader, though. A multi-target FRAGMENT shader with
         * no vertex stage is an ordinary post-process -- it writes several
         * attachments while drawing a full-screen quad through the engine's
         * own vertex path, and needs no transform of its own. 3DreamEngine's
         * blur_cube_multi (six cube faces in one pass) is exactly that, and
         * refusing it broke a renderer that was doing nothing wrong. */
        shader_log("love.graphics.newShader: a 3D shader must supply a vertex "
                   "stage with position(), which is where the model/view/"
                   "projection transform lives.");
        return -1;
    } else {
        vs = compile_shader(GL_VERTEX_SHADER, VERTEX_SHADER);
    }

    if (pixel_src) {
        if (shader_reject_unsupported(pixel_src, "pixel")) { glDeleteShader(vs); return -1; }
        int r = shader_rewrite_es100(pixel_src, rewrite_buf, sizeof rewrite_buf, 0);
        if (r < 0) {
            shader_log("love.graphics.newShader: the pixel shader is too "
                       "large after rewriting GLSL ES 1.00 spellings "
                       "(limit is 16 KB of cart GLSL)");
            glDeleteShader(vs);
            return -1;
        }
        /* An MRT shader replaces the whole fragment scaffold: N declared
         * outputs instead of one, and a main() that calls effect2 with them.
         * mrt_outputs is 0 for an ordinary shader. */
        static char mrt_prologue[SHADER_SRC_MAX];
        const char *frag_pro, *frag_epi;
        if (mrt_outputs != 0) {
            /* NEGATIVE selects LOVE's own `void effect()` + love_Canvases[]
             * form; positive selects the named-parameter effect2 form. The
             * caller decides, because only it has seen the source. */
            const int canvases = mrt_outputs < 0;
            int n = canvases ? -mrt_outputs : mrt_outputs;
            if (n > 8) n = 8;
            snprintf(mrt_prologue, sizeof mrt_prologue, "%s%s%s#line 1\n",
                     SHADER_FRAG_MRT_HEAD, MRT_OUT_DECLS[n],
                     canvases ? MRT_CANVAS_DECLS[n] : "");
            frag_pro = mrt_prologue;
            frag_epi = canvases ? MRT_CANVAS_EPILOGUES[n] : MRT_EPILOGUES[n];
        } else {
            frag_pro = is_3d ? SHADER_FRAG3D_PROLOGUE : SHADER_FRAG_PROLOGUE;
            frag_epi = is_3d ? SHADER_FRAG3D_EPILOGUE : SHADER_FRAG_EPILOGUE;
        }
        if (!shader_compose(frag_pro, rewrite_buf, frag_epi)) {
            glDeleteShader(vs);
            return -1;
        }
#ifdef WCL_DUMP_SHADER
        { int ln = 1; char b[128]; const char *q = shader_src_buf;
          while (*q) { const char *e = strchr(q, '\n');
            int len = e ? (int)(e - q) : (int)strlen(q);
            if (ln >= 160 && ln <= 175) {
                int k = snprintf(b, sizeof b, "F%03d| %.*s", ln,
                                 len > 90 ? 90 : len, q);
                if (k > 0) wc_log(b, (unsigned)k); }
            ln++; if (!e) break; q = e + 1; } }
#endif
        if (!shader_compile_checked(GL_FRAGMENT_SHADER, shader_src_buf, &fs)) {
            glDeleteShader(vs);
            return -1;
        }
    } else if (is_3d) {
        /* No cart fragment stage: supply a default that samples the texture
         * and modulates by vertex colour, which is what an untextured or
         * plainly-textured model wants and what g3d's own default does. */
        if (!shader_compose(SHADER_FRAG3D_PROLOGUE,
                            "vec4 effect(vec4 color, Image tex, vec2 uv, vec2 sc) {\n"
                            "  return Texel(tex, uv) * color;\n"
                            "}\n",
                            SHADER_FRAG3D_EPILOGUE)) {
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
            /* s->textarget, not GL_TEXTURE_2D: a render target bound as a
             * sampler may be a cubemap/array/volume, and rebinding it to the
             * 2D target on a program switch leaves the sampler reading
             * nothing at all. */
            glBindTexture(s->textarget ? s->textarget : GL_TEXTURE_2D, s->tex);
        }
        glActiveTexture(GL_TEXTURE0);
    }
}

int wcl_r2d_shader_active(void) { return active_shader >= 0; }

/* Re-establish the bound shader's sampler units.
 *
 * A texture unit's binding is GLOBAL state, not program state, so anything
 * that binds a texture between Shader:send and the draw silently steals the
 * unit -- and every 3D mesh draw binds its own texture on unit 0, while a
 * render target bind or an atlas upload can touch others. The 2D path
 * re-establishes these on every program switch; a 3D mesh draw never
 * switches programs, so it needs to ask for the same thing explicitly.
 *
 * The symptom this fixes is brutal to chase: the pixels are right, the UVs
 * are right, the uniform is bound and sent, and the sampler still reads the
 * wrong texture. 3DreamEngine hits it on every textured mesh. */
void wcl_r2d__rebind_samplers(void) {
    if (active_shader < 0) return;
    for (int i = 0; i < SHADER_TEX_UNITS; i++) {
        shader_sampler_t *s = &shader_samplers[active_shader][i];
        if (!s->used) continue;
        glActiveTexture((GLenum)(GL_TEXTURE0 + s->unit));
        glBindTexture(s->textarget ? s->textarget : GL_TEXTURE_2D, s->tex);
    }
    glActiveTexture(GL_TEXTURE0);
}

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
        /* TRANSPOSE = TRUE, and this is not a detail.
         *
         * LOVE's Shader:send takes matrices in ROW-major order (its own docs
         * call the flat form "row-major"), and every LOVE library writes them
         * that way -- g3d's setProjectionMatrix assigns the perspective row
         * `0,0,-1,0` to elements 13..16, i.e. the last ROW. GL reads a flat
         * array as COLUMN-major, so uploading it untransposed puts that row
         * in the last COLUMN, which makes w = -z... for the wrong term and
         * sends every vertex outside the clip volume.
         *
         * The failure mode is why this is called out: there is no GL error,
         * no shader warning, and no missing uniform. The draw executes
         * perfectly and rasterizes nothing, so it reads as "3D doesn't work"
         * rather than "the matrix is transposed". */
        case 16:
#ifdef WCL_R3D_TRACE
            {
                char b[300];
                int k = snprintf(b, sizeof b,
                    "send mat4 %s = [%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | "
                    "%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]", name,
                    v[0],v[1],v[2],v[3], v[4],v[5],v[6],v[7],
                    v[8],v[9],v[10],v[11], v[12],v[13],v[14],v[15]);
                if (k > 0) wc_log(b, (unsigned)k);
            }
#endif
            glUniformMatrix4fv(loc, 1, 1, v);
            break;
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

/* Standalone textures for images bound to SAMPLER uniforms, keyed by the
 * image's RGBA payload pointer. Separate from the atlas because a sampler
 * needs the whole 0..1 image; separate from the 3D target table because
 * these are plain 2D cart images. */
GLuint wcl_r2d__upload_standalone(const void *pixels, int w, int h);
#define MAX_SAMPLER_TEX 32
static struct { const void *key; GLuint tex; int used; } sampler_texs[MAX_SAMPLER_TEX];

static GLuint sampler_texture_for(const void *pixels, int w, int h) {
    for (int i = 0; i < MAX_SAMPLER_TEX; i++)
        if (sampler_texs[i].used && sampler_texs[i].key == pixels)
            return sampler_texs[i].tex;
    for (int i = 0; i < MAX_SAMPLER_TEX; i++) {
        if (sampler_texs[i].used) continue;
        GLuint t = wcl_r2d__upload_standalone(pixels, w, h);
        if (!t) return 0;
        sampler_texs[i].key = pixels;
        sampler_texs[i].tex = t;
        sampler_texs[i].used = 1;
        return t;
    }
    shader_log("Shader:send: out of sampler textures (32 max)");
    return 0;
}

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
        /* A STANDALONE texture, not the shared atlas.
         *
         * An atlas entry is a sub-rect, so a shader sampling it would need
         * uv scaled into that rect -- which no shader written for LOVE
         * does, because in LOVE a sampler uniform is the whole image at
         * 0..1. This used to hand over the atlas with a warning explaining
         * the wrong uv range; the warning was correct and useless, since a
         * cart cannot act on it without rewriting its shaders.
         *
         * Uploading the image once on its own fixes the uv range and gets
         * REPEAT wrapping and mipmaps as a side effect, which is what a
         * texture used by a 3D material wants anyway. Cached per payload
         * pointer so it uploads once, not per send. */
        tex = sampler_texture_for(pixels, w, h);
        if (!tex) return 0;
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
        shader_log("Shader:send: out of texture units (15 sampler uniforms "
                   "max per shader)");
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
    s->textarget = GL_TEXTURE_2D;
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

/* love.graphics.setBlendMode(mode, alphamode).
 *
 * This used to be a single add/alpha toggle, which silently mapped EVERY
 * other mode onto plain alpha. That is invisible in 2D carts -- alpha is
 * what they wanted anyway -- and fatal to a deferred 3D renderer, whose
 * post chain composites with "replace" and multiplies AO and bloom into
 * the colour buffer with "multiply". With both collapsed to alpha the
 * geometry pass renders perfectly and the composite comes out black, with
 * no GL error and a correct draw-call count.
 *
 * Modes are LOVE's. "alphamultiply" premultiplies the source by its alpha
 * in the blend equation; "premultiplied" means the source already is. */
void wcl_r2d_blend_mode(int mode, int premultiplied) {
    if (!ready) return;
    if (blend_mode_cur == mode && blend_premult_cur == premultiplied) return;
    flush_batches();
    blend_mode_cur = mode;
    blend_premult_cur = premultiplied;
    blend_add_on = (mode == WCL_BLEND_ADD);

    /* The source factor for the colour channels: a premultiplied source must
     * NOT be scaled by its alpha a second time. */
    GLenum src_c = premultiplied ? GL_ONE : GL_SRC_ALPHA;

    switch (mode) {
    case WCL_BLEND_NONE:            /* "replace": write the source through */
        glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);
        glBlendEquation(GL_FUNC_ADD);
        break;
    case WCL_BLEND_ADD:
        glBlendFuncSeparate(src_c, GL_ONE, GL_ZERO, GL_ONE);
        glBlendEquation(GL_FUNC_ADD);
        break;
    case WCL_BLEND_SUBTRACT:
        glBlendFuncSeparate(src_c, GL_ONE, GL_ZERO, GL_ONE);
        glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
        break;
    case WCL_BLEND_MULTIPLY:
        /* dst * src. LOVE documents this one as premultiplied-only. */
        glBlendFuncSeparate(GL_DST_COLOR, GL_ZERO, GL_DST_ALPHA, GL_ZERO);
        glBlendEquation(GL_FUNC_ADD);
        break;
    case WCL_BLEND_LIGHTEN:
        glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
        glBlendEquation(GL_MAX);
        break;
    case WCL_BLEND_DARKEN:
        glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
        glBlendEquation(GL_MIN);
        break;
    case WCL_BLEND_SCREEN:
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_COLOR,
                            GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glBlendEquation(GL_FUNC_ADD);
        break;
    case WCL_BLEND_ALPHA:
    default:
        glBlendFuncSeparate(src_c, GL_ONE_MINUS_SRC_ALPHA,
                            GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glBlendEquation(GL_FUNC_ADD);
        break;
    }

    /* "replace" is the one mode that must run with blending DISABLED to be
     * exact: with it on, a source alpha of 0 would still write through the
     * ONE/ZERO factors, but the equation cost is pointless and some drivers
     * differ on dual-source edge cases. Everything else needs it on. */
    if (mode == WCL_BLEND_NONE) {
        glDisable(GL_BLEND);
        blend_enabled = 0;
    } else {
        glEnable(GL_BLEND);
        blend_enabled = 1;
    }
}

/* Back-compat shim for the old binary toggle. */
void wcl_r2d_blend_add(int on) {
    wcl_r2d_blend_mode(on ? WCL_BLEND_ADD : WCL_BLEND_ALPHA, 0);
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
/* Sized from the engine's max resolution (runtime.c MAX_WIDTH/MAX_HEIGHT).
 * Hardcoding 1280*720 here silently capped every cart at 720p. */
#ifndef WCL_MAX_PIXELS
#define WCL_MAX_PIXELS (1920 * 1080)
#endif
static uint32_t blit_rgba[WCL_MAX_PIXELS];

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

/* ── the seam render3d_gl.c reaches through ───────────────────────────
 *
 * The 3D pipeline is a separate translation unit with its own VAO, buffers
 * and program, but it shares this file's GL context and has to cooperate
 * with its batcher and its texture cache. Rather than expose the whole
 * static state, these five entry points are the entire contract:
 *
 *   flush   - a 3D draw must not land in the middle of pending 2D vertices,
 *             so it flushes first, exactly as a texture change does
 *   program - the bound cart shader, which for 3D is where the transform
 *             lives; 0 means the default 2D program, which cannot draw 3D
 *   rebind  - restore the 2D VAO/buffer after a 3D draw, since a VAO records
 *             attribute state per index and the two layouts disagree
 *   invalidate - drop the cached texture binding, which the 3D path changes
 *             behind this file's back
 *
 * Declared in render3d_gl.c as extern; deliberately NOT in a header, since
 * nothing outside these two files may call them. */
void wcl_r2d__flush_for_3d(void) {
    if (wcl_r2d_active()) flush_batches();
}

GLuint wcl_r2d__active_program(void) {
    if (active_shader < 0) return 0;
    shader_t *sh = shader_by_handle(active_shader);
    return sh ? sh->program : 0;
}

void wcl_r2d__rebind_2d_state(void) {
    if (!ready) return;
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, index_buffer);
}

void wcl_r2d__invalidate_texture_binding(void) {
    bound_texture = 0;
}

/* Restore the screen's viewport and the clip-space scale derived from it.
 * A GPU render target sets its own viewport, and leaving that in place after
 * unbinding would rescale every subsequent 2D draw -- silently, since the
 * geometry is still "on screen", just the wrong size. Mirrors what
 * wcl_r2d_target(NULL,...) does for the 2D canvas path. */
void wcl_r2d__set_clip_size(int w, int h) {
    if (!ready || w < 1 || h < 1) return;
    ndc_scale_x = 2.0f / (float)w;
    ndc_scale_y = 2.0f / (float)h;
}

void wcl_r2d__screen_viewport(void) {
    if (!ready) return;
    if (current_target) {
        glViewport(0, 0, current_target->w, current_target->h);
        ndc_scale_x = 2.0f / (float)current_target->w;
        ndc_scale_y = 2.0f / (float)current_target->h;
        return;
    }
    restore_screen_viewport();
    ndc_scale_x = 2.0f / (float)width;
    ndc_scale_y = 2.0f / (float)height;
}

/* The program object behind a shader HANDLE, whether or not it is bound.
 * Shader:send sets uniforms on a shader the cart may not have bound yet,
 * which is legal in LOVE and is how a renderer configures several programs
 * up front. */
GLuint wcl_r2d__program_of(int handle) {
    shader_t *sh = shader_by_handle(handle);
    return sh ? sh->program : 0;
}

void wcl_r2d__restore_program(void) {
    if (!ready) return;
    shader_restore_program();
}

/* Claim a texture unit for `loc` in shader `handle`, from the SAME table the
 * 2D image path uses.
 *
 * This has to be shared. Both paths hand out units 1..15, and two
 * independent allocators would each hand out unit 1 -- so a shader sampling
 * an Image and a render target would have the second binding silently
 * replace the first, and the g-buffer read would return the atlas. Returns
 * the unit, or 0 when the shader is out of units.
 *
 * The unit is remembered per (shader, uniform location), so re-sending the
 * same uniform reuses its unit rather than exhausting the table. `tex` and
 * `target` are recorded so wcl_r2d_shader_use can re-establish the binding
 * on a program switch, which is where a texture unit's binding would
 * otherwise be lost. */
int wcl_r2d__claim_texture_unit(int handle, GLint loc, GLuint tex, GLenum textarget) {
    if (handle < 0 || handle >= MAX_SHADERS || loc < 0) return 0;
    int idx = -1;
    for (int i = 0; i < SHADER_TEX_UNITS; i++)
        if (shader_samplers[handle][i].used &&
            shader_samplers[handle][i].location == loc) { idx = i; break; }
    if (idx < 0)
        for (int i = 0; i < SHADER_TEX_UNITS; i++)
            if (!shader_samplers[handle][i].used) { idx = i; break; }
    if (idx < 0) return 0;
    shader_sampler_t *s = &shader_samplers[handle][idx];
    s->location = loc;
    s->tex = tex;
    s->textarget = textarget;
    s->unit = idx + 1;
    s->used = 1;
    return s->unit;
}

/* The cart's RGBA payload for an image, uploaded as a STANDALONE texture
 * rather than an atlas slot. See render3d_gl.h for why 3D cannot use the
 * atlas: a model's uv leaves 0..1 and relies on GL_REPEAT. Returns 0 on
 * failure. The 3D side owns and caches the result. */
GLuint wcl_r2d__upload_standalone(const void *pixels, int w, int h) {
    if (!ready || !pixels || w <= 0 || h <= 0) return 0;
    flush_batches();
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    /* REPEAT is the whole reason this texture is not in the atlas. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    /* Trilinear + mips: a 3D surface is routinely minified far below 1:1
     * (a floor receding to the horizon), where NEAREST aliases into moire.
     * The 2D path's NEAREST is right for pixel art at 1:1 and wrong here. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenerateMipmap(GL_TEXTURE_2D);
    bound_texture = 0;   /* we changed the binding behind the cache's back */
    return t;
}

void wcl_r2d_disable_why(const char *why) {
    /* The GPU 2D path is not a preference, it is the contract. Anything that
     * silently drops the whole frame to the software rasterizer is a DEFECT,
     * and a silent one is the worst kind: the picture still looks right while
     * the frame time quietly triples. Say which primitive did it. */
    if (wcl_r2d_active()) {
        WC_LOG("GPU 2D path DISABLED for the rest of the run -- this is a BUG, not a fallback:");
        WC_LOG(why);
    }
    wcl_r2d_disable();
}

void wcl_r2d_disable(void) {
    if (wcl_r2d_active()) flush_batches();
    /* Depth testing and culling are global GL state. Leaving them enabled
     * while the frame falls back to the software path would apply them to
     * wc_gl_blit's fullscreen present quad, which can discard it entirely. */
    wcl_r3d_reset();
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
    /* Sized from the shared cap, not a bare 64. With the cap raised this
     * array would otherwise be written past its end -- a silent stack smash
     * rather than a refusal. */
    int idx[WCL_MAX_POLY_PTS];
    const int ccw = poly_area2(xs, ys, n) > 0;
    for (int i = 0; i < n; i++) idx[i] = ccw ? i : (n - 1 - i);

    int m = n, tris = 0, guard = 0;
    /* Runaway guard scales with the cap: an ear clipper needs at most n-2
     * successful clips, and the bound is generous enough that a legitimately
     * awkward polygon is never cut short. */
    while (m > 3 && guard++ < WCL_MAX_POLY_PTS * WCL_MAX_POLY_PTS) {
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
/* Cache slots are sized for SMALL polygons; see the note below. */
#define TRICACHE_MAX_PTS 64

typedef struct {
    uint32_t hash;
    int n, ntri;
    /* Cache entries stay SMALL on purpose. This memoises the ear-clipping
     * of repeated shapes; sizing each slot for the largest legal polygon
     * made the table 48 KB of static memory for a benefit that only
     * applies to little ones. Large polygons skip the cache and are
     * clipped fresh -- still on the GPU, which is the part that matters. */
    int tri[3 * TRICACHE_MAX_PTS];
    int used;
} tricache_t;
/* Largest filled polygon the GL path will accept.
 *
 * This used to be a bare 64 in three places, and it was NOT a GPU limit --
 * just the size of a stack array. Anything larger was REFUSED, and a
 * refusal drops the whole frame (3D included) to the software rasterizer
 * for the rest of the run. So a big enough ellipse or arc silently cost
 * every later frame its GPU path.
 *
 * Matches MAX_POLY_PTS on the Lua side so the two layers agree: a polygon
 * that the prelude is willing to emit is one the GL path will take. */
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
    if (!wcl_r2d_active() || n < 3 || n > WCL_MAX_POLY_PTS) return 0;

    /* Simplicity is checked FIRST, before convexity.
     *
     * A pentagram turns the same way at every vertex, so poly_is_convex
     * calls it convex and it would take the fan path -- skipping the
     * self-intersection guard entirely and filling a centre that even-odd
     * leaves hollow. That was latent while only convex fills were on GL;
     * adding ear clipping is what surfaced it. Cheap enough at this n to
     * run unconditionally, and it is the correctness gate for both paths. */
    if (!poly_is_simple(xs, ys, n)) return 0;

    int tri[3 * WCL_MAX_POLY_PTS];
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
        /* Large polygons skip the CACHE, not the GPU. Sizing every cache
         * slot for the biggest legal polygon would cost 48 KB of static
         * memory to memoise shapes that are rare anyway. */
        const int cacheable = (n <= TRICACHE_MAX_PTS);
        for (int i = 0; cacheable && i < MAX_TRICACHE; i++)
            if (tricache[i].used && tricache[i].hash == h && tricache[i].n == n) {
                hit = &tricache[i]; break;
            }
        if (hit) {
            ntri = hit->ntri;
            for (int i = 0; i < ntri * 3; i++) tri[i] = hit->tri[i];
        } else {
            ntri = poly_triangulate(xs, ys, n, tri);
            if (ntri <= 0) return 0;
            if (cacheable) {
                tricache_t *slot = &tricache[tricache_next];
                tricache_next = (tricache_next + 1) % MAX_TRICACHE;
                slot->hash = h; slot->n = n; slot->ntri = ntri; slot->used = 1;
                for (int i = 0; i < ntri * 3; i++) slot->tri[i] = tri[i];
            }
        }
    }
    if (ntri <= 0) return 0;
    flush_batches();

    /* THIS was the fourth hardcoded 64, and the one that actually crashed:
     * a triangulated 256-gon emits 254 triangles = 762 vertices, which
     * overran this array by 3x. The others were merely refusals; this was
     * a stack smash. Sized from the shared cap like the rest. */
    vertex_t v[3 * WCL_MAX_POLY_PTS];
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
        if (atlas_y + h > ATLAS_SIZE) {
            /* SAY SO. This returned NULL silently and every caller "fell
             * back", which on a phone meant a whole column of a match-three
             * board simply had no jewels in it -- with no error anywhere,
             * on a build that rendered perfectly on desktop. A silent
             * capacity failure is the worst kind: it looks like a layout
             * bug and sends you hunting the viewport. */
            WC_LOG("atlas full: no room for another texture");
            return NULL;
        }
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
        /* 2px gutter: packed edge-to-edge, bilinear sampling at a sprite's
         * border blends in the NEIGHBORING entry's texels (visible as a
         * colored fringe around alpha-soft art, e.g. portrait vignettes). */
        atlas_x += w + 2;
        if (h + 2 > atlas_row_h) atlas_row_h = h + 2;
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

/* ── meshes ────────────────────────────────────────────────────────────
 *
 * Arbitrary triangles, so this does NOT use the batcher. flush_batches()
 * first (draw order), fill a scratch buffer, one glDrawArrays -- the same
 * shape wcl_r2d_poly uses, with two differences that matter:
 *
 *   * no triangle cap. A polygon is capped at 64 points because ear clipping
 *     is O(n^3); a mesh arrives already triangulated, so the only limit is
 *     how much fits in one upload, and the draw is CHUNKED rather than
 *     refused past that.
 *   * textured. u_textured mode 1 with the atlas bound, which is where the
 *     uv remap comes in.
 *
 * The remap: a cart writes uv in 0..1 against ITS OWN image, LOVE's contract.
 * Sprites here live in a shared 2048^2 atlas, so 0..1 would address the whole
 * atlas -- every other image in the cart, at the wrong scale. The image's
 * sub-rect is (atlas_x, atlas_y, w, h), so the correct sample is
 *     atlas_uv = (atlas_xy + uv * wh) / ATLAS_SIZE
 * A wrong remap here does not look blank; it looks like a garbled crop of a
 * NEIGHBOURING sprite, which is why the example draws a recognisable image.
 *
 * A Canvas is its own texture, so 0..1 is already right there -- but an FBO
 * has GL's bottom-left origin against the cart's top-left, so V flips. Same
 * rule wcl_r2d_sprite applies, expressed as v -> 1-v because a mesh's uv is
 * normalized rather than a texel rect. */
#define MESH_CHUNK 1024   /* vertices per upload; a multiple of 3 */
static vertex_t mesh_scratch[MESH_CHUNK];

int wcl_r2d_mesh(const float *verts, int count,
                 const void *pixels, int tw, int th,
                 uint32_t tint, int alpha) {
    if (!wcl_r2d_active() || !verts || count < 3) return 0;
    count -= count % 3;

    /* Resolve the texture BEFORE flushing: get_texture may itself flush and
     * upload, and doing that mid-draw would interleave the atlas upload with
     * this mesh's own vertices. */
    GLuint tex = 0;
    int mode = 0;              /* u_textured: 0 solid, 1 RGBA texture */
    float ax = 0, ay = 0, aw = 1, ah = 1;   /* uv -> atlas transform */
    int flip_v = 0;
    if (pixels) {
        target_t *tgt = target_find(pixels);
        if (tgt) {
            if (tgt == current_target) return 0;   /* cannot sample the target
                                                      it is drawing into */
            tex = tgt->tex;
            flip_v = 1;                            /* FBO origin is bottom-left */
        } else {
            texture_t *t = get_texture(pixels, tw, th);
            if (!t) return 0;                      /* atlas full: caller reports */
            tex = atlas_texture;
            ax = (float)t->atlas_x / (float)ATLAS_SIZE;
            ay = (float)t->atlas_y / (float)ATLAS_SIZE;
            aw = (float)t->w / (float)ATLAS_SIZE;
            ah = (float)t->h / (float)ATLAS_SIZE;
        }
        mode = 1;
    }

    flush_batches();

    /* The mesh's own colours are per-vertex; the current draw colour
     * MODULATES them, which is what LOVE does (setColor tints a mesh). */
    const float tr = (float)((tint >> 16) & 255) / 255.0f;
    const float tg = (float)((tint >> 8) & 255) / 255.0f;
    const float tb = (float)(tint & 255) / 255.0f;
    const float ta = (float)alpha / 255.0f;

    /* Blending stays on unless every vertex is opaque. Cheaper to just ask
     * for it: a mesh is one draw, so there is no batch to split. */
    int opaque = (alpha >= 255);
    if (opaque) {
        for (int i = 0; i < count; i++)
            if (verts[i * 8 + 7] < 1.0f) { opaque = 0; break; }
    }
    set_blend(!opaque);
    if (mode) bind_texture(tex);
    set_textured(mode);
    /* A texture unit's binding is GLOBAL, and bind_texture just claimed unit
     * 0. Every mesh drawn this frame does the same, so by the time a cart's
     * shader runs, the sampler units it was given at Shader:send time may be
     * bound to someone else's texture. Re-establish them here.
     *
     * 3DreamEngine hits this on every textured mesh: it sends its material
     * samplers once per material switch and then draws many meshes, so all
     * but the first sample whatever was bound last. The symptom is a
     * correctly lit, correctly UV-mapped, SOLID WHITE surface. */
    wcl_r2d__rebind_samplers();
    glBindBuffer(GL_ARRAY_BUFFER, buffer);

    for (int base = 0; base < count; base += MESH_CHUNK) {
        int n = count - base;
        if (n > MESH_CHUNK) n = MESH_CHUNK;
        for (int i = 0; i < n; i++) {
            const float *s = &verts[(base + i) * 8];
            vertex_t *v = &mesh_scratch[i];
            ndc(s[0], s[1], &v->x, &v->y);
            v->u = ax + s[2] * aw;
            v->v = flip_v ? (1.0f - s[3]) : (ay + s[3] * ah);
            v->r = s[4] * tr; v->g = s[5] * tg; v->b = s[6] * tb;
            v->a = s[7] * ta;
            v->rad = 0;
        }
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(vertex_t) * n),
                     mesh_scratch, GL_DYNAMIC_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, n);
        frame_stats.draws++;
        frame_stats.upload_bytes += (uint32_t)(sizeof(vertex_t) * n);
        frame_stats.quads += (uint32_t)(n / 3);
    }
    return 1;
}

static target_t *target_find(const void *key) {
    for (int i = 0; i < MAX_TARGETS; i++)
        if (targets[i].used && targets[i].key == key) return &targets[i];
    return NULL;
}

/* THE HOST'S VIEWPORT RECT, when it is not simply (0,0,cart_w,cart_h).
 *
 * A host that letterboxes -- an Android device whose screen is not the
 * cart's aspect -- sets a viewport once and expects it to stay. It did not:
 * restoring the screen here reset the viewport to the CART's size, so the
 * first setCanvas() in a frame silently destroyed the letterbox and every
 * later draw was scaled to fill the window. On a 2244x1008 phone running a
 * 1920x1080 cart that cut 127px off the top and bottom -- a whole row of a
 * match-three board, plus a clipped column -- and it also broke touch,
 * because the host maps taps through the rect it believes is current.
 *
 * Zero width means "not set", and the old behaviour applies. */
static int host_vp_x, host_vp_y, host_vp_w, host_vp_h;

void wcl_r2d_set_host_viewport(int x, int y, int w, int h) {
    host_vp_x = x; host_vp_y = y; host_vp_w = w; host_vp_h = h;
}

static void restore_screen_viewport(void) {
    if (host_vp_w > 0 && host_vp_h > 0)
        glViewport(host_vp_x, host_vp_y, host_vp_w, host_vp_h);
    else
        glViewport(0, 0, width, height);
}

int wcl_r2d_target(const void *key, int w, int h) {
    if (!ready) return 0;
    flush_batches();

    if (!key) {                       /* back to the screen */
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        restore_screen_viewport();
        current_target = NULL;
        ndc_scale_x = 2.0f / (float)width;
        ndc_scale_y = 2.0f / (float)height;
        return 1;
    }

    target_t *t = target_find(key);
    if (!t) {
        for (int i = 0; i < MAX_TARGETS && !t; i++) if (!targets[i].used) t = &targets[i];
        if (!t) return 0;             /* out of targets: caller falls back */
        /* A RECYCLED slot may still carry the previous canvas's stencil
         * renderbuffer, sized for the previous canvas. Drop it: a stale
         * one is both a leak and the wrong dimensions, and the next
         * stencil() on this target reallocates at the right size. */
        if (t->stencil_rb) { glDeleteRenderbuffers(1, &t->stencil_rb); t->stencil_rb = 0; }
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
    /* DEPTH TOO, ALWAYS -- not only when a depth mode is currently live.
     *
     * A freshly bound render target's depth attachment holds undefined
     * values. With the depth test on -- which a 3D pass turns on before it
     * binds its g-buffer -- every fragment is compared against that garbage
     * and, at lequal, discarded. The pass then runs perfectly and writes
     * nothing: geometry submitted, no GL error, black canvas.
     *
     * This used to be guarded on "a depth mode is live", which looks
     * equivalent and is not. A deferred renderer binds its g-buffer with the
     * depth test OFF, clears, and only THEN enables depth for the geometry
     * pass -- 3DreamEngine does exactly this:
     *
     *     love.graphics.setDepthMode()             -- depth off
     *     love.graphics.clear(false, false, true)  -- guard skipped depth here
     *     ...
     *     love.graphics.setDepthMode("less", true) -- now test vs garbage
     *
     * so the one clear that had to reach the depth buffer was the one the
     * guard skipped, and the whole scene failed the test in silence.
     *
     * LOVE's clear() clears depth unconditionally, so matching it is also
     * simply correct. glClear honours the depth write MASK, so the mask is
     * forced on for the clear and restored after. */
    uint32_t dcmp = 0; int dwrite = 0;
    wcl_r3d_get_depth_mode(&dcmp, &dwrite);
    glDepthMask(1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (!dwrite) glDepthMask(0);
}

void wcl_r2d_forget(const void *key) {
    target_t *t = target_find(key);
    if (t) { t->used = 0; t->key = NULL; }
    for (int i = 0; i < MAX_TEXTURES; i++)
        if (textures[i].used && textures[i].pixels == key) textures[i].used = 0;
    /* Shader SAMPLER textures are a SECOND cache, also keyed by the pixel
     * pointer, and forgetting only the draw-path cache left them stale
     * forever: an ImageData painted with setPixel and then handed to a
     * shader kept sampling whatever the texture held when it was first
     * uploaded -- for a freshly allocated ImageData, solid white. The pixels
     * were right, the UVs were right, and the picture was blank. */
    for (int i = 0; i < MAX_SAMPLER_TEX; i++) {
        if (sampler_texs[i].used && sampler_texs[i].key == key) {
            glDeleteTextures(1, &sampler_texs[i].tex);
            sampler_texs[i].used = 0;
            sampler_texs[i].key = NULL;
            sampler_texs[i].tex = 0;
        }
    }
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

    /* SCISSOR IS IN WINDOW PIXELS, not cart pixels.
     *
     * Everything else the cart draws goes through the projection, so cart
     * coordinates land wherever the viewport puts them. glScissor does not:
     * it clips in the window's own pixels. Passing the cart rect straight
     * through works only when the viewport happens to be the cart's size
     * and sits at the origin.
     *
     * On a letterboxed host it silently clips the wrong region. Measured on
     * a 2244x1008 phone running a 1920x1080 cart: the board's cart rect
     * x 60..1260 clipped at WINDOW x 60..1260, while the board actually
     * drew at window x 282..1402 -- so the right ~140px was cut and the
     * last column of a match-three board had no jewels in it, with the cell
     * backgrounds (drawn outside the scissor) still visible. It rendered
     * perfectly on desktop, where viewport == cart size.
     *
     * A canvas target is its own surface at its own size, so cart
     * coordinates are already right there and only the screen needs the
     * mapping. */
    if (!current_target && host_vp_w > 0 && host_vp_h > 0) {
        float sx = (float)host_vp_w / (float)width;
        float sy = (float)host_vp_h / (float)height;
        int gx = host_vp_x + (int)(x * sx + 0.5f);
        int gw = (int)(w * sx + 0.5f);
        int gh = (int)(h * sy + 0.5f);
        /* GL's origin is bottom-left; the cart's is top-left. Flip within
         * the WINDOW, not within the cart. */
        int gy = host_vp_y + (int)((height - (y + h)) * sy + 0.5f);
        glScissor(gx, gy, gw, gh);
        return;
    }
    glScissor(x, height - (y + h), w, h);
}

/* ── stencil ──────────────────────────────────────────────────────────
 *
 * love.graphics.stencil(fn, action, value) draws `fn` into the stencil
 * buffer instead of the colour buffer; setStencilTest then keeps or
 * rejects later fragments by comparing against it. That is how a game
 * masks to a non-rectangular region -- a circular spotlight, a torn page
 * edge, a health bar clipped to a curve. Scissor only does rectangles.
 *
 * PERFORMANCE: everything here is OFF and COSTS NOTHING until a cart
 * calls stencil(). The stencil renderbuffer is allocated lazily on first
 * use, GL_STENCIL_TEST is only enabled while a test is active, and the
 * hot draw path is untouched -- no per-draw branch was added. A cart that
 * never stencils runs exactly the code it ran before.
 *
 * The default framebuffer's stencil bits are requested by the host at
 * context creation; when they are absent (or an FBO cannot get a stencil
 * attachment) these calls refuse and the caller reports it, rather than
 * silently drawing an unmasked frame that looks almost right. */

/* Two enums the vendored GL header does not carry. Defined here rather
 * than added to wasmcart.h, which is a vendored copy that a dependency
 * bump would overwrite. Values are from the GLES2/GL spec and are not
 * implementation-specific. */
#ifndef GL_STENCIL_INDEX8
#define GL_STENCIL_INDEX8 0x8D48
#endif
#ifndef GL_STENCIL_BITS
#define GL_STENCIL_BITS   0x0D57
#endif

static int stencil_rb_ok = -1;      /* -1 unknown, 0 unavailable, 1 ready */
static int stencil_test_on;

/* Attach a stencil renderbuffer to the CURRENT target if it has none.
 * Screen targets use whatever the context was created with. */
static int stencil_ensure(void) {
    if (current_target) {
        /* A canvas FBO is colour-only by default; give it a stencil the
         * first time one is actually needed, and remember per-target so a
         * second stencil pass on the same canvas costs nothing. */
        if (!current_target->stencil_rb) {
            GLuint rb = 0;
            glGenRenderbuffers(1, &rb);
            glBindRenderbuffer(GL_RENDERBUFFER, rb);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8,
                                  current_target->w, current_target->h);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                                      GL_RENDERBUFFER, rb);
            glBindRenderbuffer(GL_RENDERBUFFER, 0);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                                          GL_RENDERBUFFER, 0);
                glDeleteRenderbuffers(1, &rb);
                return 0;
            }
            current_target->stencil_rb = rb;
        }
        return 1;
    }
    if (stencil_rb_ok < 0) {
        GLint bits = 0;
        glGetIntegerv(GL_STENCIL_BITS, &bits);
        stencil_rb_ok = (bits > 0) ? 1 : 0;
    }
    return stencil_rb_ok;
}

/* Begin writing the mask. Colour and depth writes are masked off so the
 * callback's geometry only touches stencil -- LOVE's stencil() draws
 * nothing visible. */
int wcl_r2d_stencil_begin(int action, int value) {
    if (!wcl_r2d_active()) return 0;
    flush_batches();
    if (!stencil_ensure()) return 0;

    glEnable(GL_STENCIL_TEST);
    /* glClear OBEYS THE SCISSOR TEST and the stencil write mask. An
     * earlier draw may have left a scissor enabled, which would clear
     * only part of the buffer and leave stale mask bits outside it --
     * and glStencilMask(0x00) from a previous test would make the clear
     * a no-op entirely. Force both open across the clear, then restore
     * the scissor the cart actually asked for. */
    int had_scissor = scissor_on;
    if (had_scissor) glDisable(GL_SCISSOR_TEST);
    glStencilMask(0xFF);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    if (had_scissor) glEnable(GL_SCISSOR_TEST);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthMask(GL_FALSE);
    glStencilFunc(GL_ALWAYS, value, 0xFF);
    glStencilMask(0xFF);

    GLenum op = GL_REPLACE;
    switch (action) {
        case 0: op = GL_REPLACE; break;   /* "replace"   */
        case 1: op = GL_INCR;    break;   /* "increment" */
        case 2: op = GL_DECR;    break;   /* "decrement" */
        case 3: op = GL_INVERT;  break;   /* "invert"    */
        case 4: op = GL_INCR_WRAP; break; /* "incrementwrap" */
        case 5: op = GL_DECR_WRAP; break; /* "decrementwrap" */
        default: op = GL_REPLACE; break;
    }
    glStencilOp(GL_KEEP, GL_KEEP, op);
    return 1;
}

/* Finish the mask: colour writes back on, stencil writes off. The TEST is
 * left in whatever state setStencilTest last chose. */
void wcl_r2d_stencil_end(void) {
    if (!wcl_r2d_active()) return;
    flush_batches();
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0x00);
    if (!stencil_test_on) glDisable(GL_STENCIL_TEST);
}

/* compare: 0 off, else a GL func chosen by the caller's enum. */
void wcl_r2d_stencil_test(int compare, int value) {
    if (!wcl_r2d_active()) return;
    flush_batches();
    if (compare <= 0) {
        stencil_test_on = 0;
        glDisable(GL_STENCIL_TEST);
        return;
    }
    if (!stencil_ensure()) return;
    GLenum f = GL_ALWAYS;
    switch (compare) {
        case 1: f = GL_EQUAL;    break;   /* "equal"        */
        case 2: f = GL_NOTEQUAL; break;   /* "notequal"     */
        case 3: f = GL_LESS;     break;   /* "less"         */
        case 4: f = GL_LEQUAL;   break;   /* "lequal"       */
        case 5: f = GL_GREATER;  break;   /* "greater"      */
        case 6: f = GL_GEQUAL;   break;   /* "gequal"       */
        default: f = GL_ALWAYS;  break;
    }
    stencil_test_on = 1;
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(f, value, 0xFF);
    glStencilMask(0x00);              /* test only; never write */
}

#endif /* WCL_ENABLE_GL2D */
