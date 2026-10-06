/* simd_portable.h: the few wasm simd128 operations three_c.c uses, as plain
 * C for a build without simd128 (the native bench host). A v128_t holds two
 * doubles (or four floats for the demote store); the operations are lane by
 * lane, so the results are the doubles the simd128 build produces, and
 * sincos_f64x2 is the C library's sin / cos per lane. */
#ifndef SIMD_PORTABLE_H
#define SIMD_PORTABLE_H
#include <math.h>
#include <stdint.h>
#include <string.h>

typedef union { double d[2]; float f[4]; int64_t i[2]; } v128_t;

static inline v128_t wasm_f64x2_make(double a, double b) { v128_t r; r.d[0] = a; r.d[1] = b; return r; }
static inline v128_t wasm_f64x2_splat(double a) { return wasm_f64x2_make(a, a); }
static inline v128_t wasm_f64x2_add(v128_t a, v128_t b) { return wasm_f64x2_make(a.d[0] + b.d[0], a.d[1] + b.d[1]); }
static inline v128_t wasm_f64x2_sub(v128_t a, v128_t b) { return wasm_f64x2_make(a.d[0] - b.d[0], a.d[1] - b.d[1]); }
static inline v128_t wasm_f64x2_mul(v128_t a, v128_t b) { return wasm_f64x2_make(a.d[0] * b.d[0], a.d[1] * b.d[1]); }
static inline v128_t wasm_f64x2_div(v128_t a, v128_t b) { return wasm_f64x2_make(a.d[0] / b.d[0], a.d[1] / b.d[1]); }
#define wasm_f64x2_extract_lane(v, l) ((v).d[(l)])
static inline v128_t wasm_v128_load(const void *p) { v128_t r; memcpy(&r, p, 16); return r; }
static inline void wasm_v128_store(void *p, v128_t v) { memcpy(p, &v, 16); }
/* the two doubles to floats in lanes 0, 1; lanes 2, 3 zero */
static inline v128_t wasm_f32x4_demote_f64x2_zero(v128_t a) {
    v128_t r; r.f[0] = (float)a.d[0]; r.f[1] = (float)a.d[1]; r.f[2] = 0; r.f[3] = 0; return r;
}
/* lane picks: 0..3 from a, 4..7 from b (32-bit lanes) */
static inline v128_t simd_portable_i32x4_shuffle(v128_t a, v128_t b, int l0, int l1, int l2, int l3) {
    float s[8]; memcpy(s, a.f, 16); memcpy(s + 4, b.f, 16);
    v128_t r; r.f[0] = s[l0]; r.f[1] = s[l1]; r.f[2] = s[l2]; r.f[3] = s[l3]; return r;
}
#define wasm_i32x4_shuffle(a, b, l0, l1, l2, l3) simd_portable_i32x4_shuffle((a), (b), (l0), (l1), (l2), (l3))
/* lane picks: 0..1 from a, 2..3 from b (64-bit lanes) */
static inline v128_t simd_portable_i64x2_shuffle(v128_t a, v128_t b, int l0, int l1) {
    int64_t s[4]; memcpy(s, a.i, 16); memcpy(s + 2, b.i, 16);
    v128_t r; r.i[0] = s[l0]; r.i[1] = s[l1]; return r;
}
#define wasm_i64x2_shuffle(a, b, l0, l1) simd_portable_i64x2_shuffle((a), (b), (l0), (l1))
static inline void sincos_f64x2(v128_t x, v128_t *s, v128_t *c) {
    *s = wasm_f64x2_make(sin(x.d[0]), sin(x.d[1]));
    *c = wasm_f64x2_make(cos(x.d[0]), cos(x.d[1]));
}
#endif
