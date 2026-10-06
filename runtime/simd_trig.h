/* simd_trig.h: sin and cos of two doubles at once (wasm simd128 f64x2).
 *
 * musl's own algorithm, lane by lane: the "medium" Cody-Waite reduction by
 * pi/2 (its constants, both rounds always taken), then its __sin / __cos
 * polynomials and the quadrant. The result is within an ulp of musl's
 * scalar sin / cos (the same algorithm, so usually identical), not
 * bit-identical: musl skips the reduction below pi/4 and takes its second
 * round only on cancellation. A pair with a lane outside the range the
 * reduction covers (|x| >= 2^19 pi/2, inf, NaN) or with deep cancellation
 * is computed with the scalar functions instead. */
#ifndef SIMD_TRIG_H
#define SIMD_TRIG_H

#include <math.h>
#include <wasm_simd128.h>

static inline v128_t st_splat(double v) { return wasm_f64x2_splat(v); }

/* s, c: sin and cos of the two lanes of x */
static inline __attribute__((always_inline)) void sincos_f64x2(v128_t x, v128_t *s, v128_t *c) {
    const v128_t invpio2 = st_splat(6.36619772367581382433e-01);
    const v128_t pio2_1 = st_splat(1.57079632673412561417e+00), pio2_1t = st_splat(6.07710050650619224932e-11);
    const v128_t pio2_2 = st_splat(6.07710050630396597660e-11), pio2_2t = st_splat(2.02226624879595063154e-21);
    const v128_t ax = wasm_f64x2_abs(x);
    /* out of range, not finite: the scalar functions */
    v128_t bad = wasm_v128_or(wasm_f64x2_ge(ax, st_splat(823549.6558254041)), wasm_f64x2_ne(x, x));
    if (wasm_v128_any_true(bad)) goto scalar;
    {
        v128_t fn = wasm_f64x2_nearest(wasm_f64x2_mul(x, invpio2));
        v128_t r = wasm_f64x2_sub(x, wasm_f64x2_mul(fn, pio2_1));
        v128_t w = wasm_f64x2_mul(fn, pio2_1t);
        /* second round (good to 118 bits) */
        v128_t t = r;
        w = wasm_f64x2_mul(fn, pio2_2);
        r = wasm_f64x2_sub(t, w);
        w = wasm_f64x2_sub(wasm_f64x2_mul(fn, pio2_2t), wasm_f64x2_sub(wasm_f64x2_sub(t, r), w));
        v128_t y0 = wasm_f64x2_sub(r, w);
        v128_t y1 = wasm_f64x2_sub(wasm_f64x2_sub(r, y0), w);
        /* deep cancellation (musl's third round): scalar */
        v128_t tiny = wasm_f64x2_lt(wasm_f64x2_abs(y0), wasm_f64x2_mul(ax, st_splat(0x1p-40)));
        tiny = wasm_v128_and(tiny, wasm_f64x2_ne(fn, st_splat(0)));
        if (wasm_v128_any_true(tiny)) goto scalar;

        /* __sin(y0, y1, 1) */
        v128_t z = wasm_f64x2_mul(y0, y0);
        v128_t ww = wasm_f64x2_mul(z, z);
        v128_t rs = wasm_f64x2_add(
            wasm_f64x2_add(st_splat(8.33333333332248946124e-03),
                           wasm_f64x2_mul(z, wasm_f64x2_add(st_splat(-1.98412698298579493134e-04),
                                                            wasm_f64x2_mul(z, st_splat(2.75573137070700676789e-06))))),
            wasm_f64x2_mul(wasm_f64x2_mul(z, ww),
                           wasm_f64x2_add(st_splat(-2.50507602534068634195e-08),
                                          wasm_f64x2_mul(z, st_splat(1.58969099521155010221e-10)))));
        v128_t v = wasm_f64x2_mul(z, y0);
        /* x - ((z*(0.5*y - v*r) - y) - v*S1) */
        v128_t sn = wasm_f64x2_sub(
            y0, wasm_f64x2_sub(
                    wasm_f64x2_sub(wasm_f64x2_mul(z, wasm_f64x2_sub(wasm_f64x2_mul(st_splat(0.5), y1), wasm_f64x2_mul(v, rs))), y1),
                    wasm_f64x2_mul(v, st_splat(-1.66666666666666324348e-01))));

        /* __cos(y0, y1) */
        v128_t rc = wasm_f64x2_add(
            wasm_f64x2_mul(z, wasm_f64x2_add(st_splat(4.16666666666666019037e-02),
                                             wasm_f64x2_mul(z, wasm_f64x2_add(st_splat(-1.38888888888741095749e-03),
                                                                              wasm_f64x2_mul(z, st_splat(2.48015872894767294178e-05)))))),
            wasm_f64x2_mul(wasm_f64x2_mul(ww, ww),
                           wasm_f64x2_add(st_splat(-2.75573143513906633035e-07),
                                          wasm_f64x2_mul(z, wasm_f64x2_add(st_splat(2.08757232129817482790e-09),
                                                                           wasm_f64x2_mul(z, st_splat(-1.13596475577881948265e-11)))))));
        v128_t hz = wasm_f64x2_mul(st_splat(0.5), z);
        v128_t wc = wasm_f64x2_sub(st_splat(1.0), hz);
        v128_t cs = wasm_f64x2_add(wc, wasm_f64x2_add(wasm_f64x2_sub(wasm_f64x2_sub(st_splat(1.0), wc), hz),
                                                       wasm_f64x2_sub(wasm_f64x2_mul(z, rc), wasm_f64x2_mul(y0, y1))));

        /* quadrant n & 3: 0 (s, c), 1 (c, -s), 2 (-s, -c), 3 (-c, s) */
        v128_t q = wasm_f64x2_sub(fn, wasm_f64x2_mul(st_splat(4), wasm_f64x2_floor(wasm_f64x2_mul(fn, st_splat(0.25)))));
        v128_t q1 = wasm_f64x2_eq(q, st_splat(1)), q2 = wasm_f64x2_eq(q, st_splat(2)), q3 = wasm_f64x2_eq(q, st_splat(3));
        v128_t swap = wasm_v128_or(q1, q3);
        v128_t sign = wasm_i64x2_splat((long long)0x8000000000000000ull);
        v128_t so = wasm_v128_bitselect(cs, sn, swap), co = wasm_v128_bitselect(sn, cs, swap);
        so = wasm_v128_xor(so, wasm_v128_and(wasm_v128_or(q2, q3), sign));
        co = wasm_v128_xor(co, wasm_v128_and(wasm_v128_or(q1, q2), sign));
        *s = so;
        *c = co;
        return;
    }
scalar: {
        double a0 = wasm_f64x2_extract_lane(x, 0), a1 = wasm_f64x2_extract_lane(x, 1);
        *s = wasm_f64x2_make(sin(a0), sin(a1));
        *c = wasm_f64x2_make(cos(a0), cos(a1));
    }
}

#endif
