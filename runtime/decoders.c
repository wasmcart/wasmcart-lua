/* MP3 / FLAC / Opus decoders, each in this one translation unit.
 *
 * WHY THIS FILE EXISTS: the same reason vorbis.c does. dr_mp3 and dr_flac are
 * single-header libraries whose IMPLEMENTATION halves define short bare macros,
 * and Lua's headers declare every API function as `(lua_State *L, ...)`. Keeping
 * the codec implementations out of runtime.c's TU is the established pattern
 * here; this file just adds three more entry points runtime.c declares extern.
 *
 * OUTPUT CONTRACT: every decoder writes INTERLEAVED S16 and returns the frame
 * count (samples per channel), or -1. S16 rather than the float32 that
 * webaudio-node's audio_decoders.cpp emits, because the consumer differs:
 * wc_mixer_load_raw() takes `const int16_t *`, so float output would only be
 * converted straight back down. dr_mp3, dr_flac and opusfile all expose native
 * S16 readers, so this costs nothing -- note opusfile's op_read() is S16 out of
 * a FLOAT internal decode, which is not the same thing as an OP_FIXED_POINT
 * build and does not change the decode math.
 *
 * The caller owns *out and frees it with free().
 */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* dr_mp3's x86 SSE path. Emscripten lowers SSE2 intrinsics to wasm128, and the
 * engine already builds with -msimd128 -msse2, so take the SIMD path rather
 * than the scalar fallback. ONLY_SIMD then skips the runtime CPU detection,
 * which cannot tell us anything useful in wasm.
 *
 * __i386__ IS the load-bearing line: dr_mp3 selects SSE on __i386__/__x86_64__,
 * neither of which emscripten defines, so without it the header falls through
 * to DRMP3_HAVE_SIMD 0 and ONLY_SIMD becomes a hard #error. Defined HERE rather
 * than in build.sh because it must not leak into any other translation unit --
 * it is a lie about the target that only this header's dispatch should believe.
 * webaudio-node passes the same three flags to the same library under emcc. */
#define __i386__ 1
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#define DR_MP3_ONLY_SIMD
#include "dr_mp3.h"
#undef __i386__

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#include "dr_flac.h"

#ifdef WCL_ENABLE_OPUS
#include <opusfile.h>
#endif

/* ── MP3 ──────────────────────────────────────────────────────────── */

int wcl_decode_mp3(const unsigned char *data, int size,
                   int16_t **out, int *channels, int *sample_rate) {
    drmp3 mp3;
    if (!drmp3_init_memory(&mp3, data, (size_t)size, NULL)) return -1;

    drmp3_uint64 frames = drmp3_get_pcm_frame_count(&mp3);
    if (frames == 0 || mp3.channels < 1) { drmp3_uninit(&mp3); return -1; }

    /* The mixer holds one int16 per sample and indexes with int, so refuse a
     * stream whose interleaved sample count would not survive the conversion
     * rather than truncating it into a buffer overrun. ~93 minutes stereo. */
    if (frames > (drmp3_uint64)(INT32_MAX / (mp3.channels * (int)sizeof(int16_t)))) {
        drmp3_uninit(&mp3);
        return -1;
    }

    int16_t *pcm = (int16_t *)malloc((size_t)frames * mp3.channels * sizeof(int16_t));
    if (!pcm) { drmp3_uninit(&mp3); return -1; }

    drmp3_uint64 got = drmp3_read_pcm_frames_s16(&mp3, frames, pcm);
    *channels    = (int)mp3.channels;
    *sample_rate = (int)mp3.sampleRate;
    drmp3_uninit(&mp3);

    /* A short read is a truncated/corrupt file, not a failure: keep what
     * decoded. drmp3_get_pcm_frame_count() walks the frames to count them, so
     * a stream that stops early here has already been seen to be short. */
    if (got == 0) { free(pcm); return -1; }
    *out = pcm;
    return (int)got;
}

/* ── FLAC ─────────────────────────────────────────────────────────── */

int wcl_decode_flac(const unsigned char *data, int size,
                    int16_t **out, int *channels, int *sample_rate) {
    drflac *flac = drflac_open_memory(data, (size_t)size, NULL);
    if (!flac) return -1;

    drflac_uint64 frames = flac->totalPCMFrameCount;
    if (frames == 0 || flac->channels < 1) { drflac_close(flac); return -1; }
    if (frames > (drflac_uint64)(INT32_MAX / (flac->channels * (int)sizeof(int16_t)))) {
        drflac_close(flac);
        return -1;
    }

    int16_t *pcm = (int16_t *)malloc((size_t)frames * flac->channels * sizeof(int16_t));
    if (!pcm) { drflac_close(flac); return -1; }

    drflac_uint64 got = drflac_read_pcm_frames_s16(flac, frames, pcm);
    *channels    = (int)flac->channels;
    *sample_rate = (int)flac->sampleRate;
    drflac_close(flac);

    if (got == 0) { free(pcm); return -1; }
    *out = pcm;
    return (int)got;
}

/* ── Opus ─────────────────────────────────────────────────────────── */

#ifdef WCL_ENABLE_OPUS
int wcl_decode_opus(const unsigned char *data, int size,
                    int16_t **out, int *channels, int *sample_rate) {
    int err = 0;
    OggOpusFile *of = op_open_memory(data, (size_t)size, &err);
    if (!of || err != 0) { if (of) op_free(of); return -1; }

    int ch = op_channel_count(of, -1);      /* -1 = whole stream (first link) */
    if (ch < 1 || ch > 8) { op_free(of); return -1; }

    /* Opus is ALWAYS 48 kHz internally and opusfile always decodes to 48 kHz,
     * which is exactly the engine's mix rate (wc_info.audio_sample_rate), so
     * nothing here ever resamples. */
    *sample_rate = 48000;
    *channels    = ch;

    /* op_pcm_total() is a hint, not a guarantee -- a non-seekable or chained
     * stream reports negative. Grow as we decode rather than trusting it. */
    ogg_int64_t hint = op_pcm_total(of, -1);
    size_t cap = (size_t)(hint > 0 ? hint : 48000) * (size_t)ch;
    int16_t *buf = (int16_t *)malloc(cap * sizeof(int16_t));
    if (!buf) { op_free(of); return -1; }

    size_t filled = 0;                      /* interleaved samples written */
    const int CHUNK = 5760;                 /* 120ms @48k, opusfile's recommendation */
    for (;;) {
        size_t need = filled + (size_t)CHUNK * (size_t)ch;
        if (need > cap) {
            if (need > (size_t)(INT32_MAX / sizeof(int16_t))) { free(buf); op_free(of); return -1; }
            size_t grow = need * 2;
            if (grow > (size_t)(INT32_MAX / sizeof(int16_t))) grow = need;
            int16_t *g = (int16_t *)realloc(buf, grow * sizeof(int16_t));
            if (!g) { free(buf); op_free(of); return -1; }
            buf = g; cap = grow;
        }
        /* op_read gives S16 directly; the float path would only be converted
         * back down for the mixer. Count is in SAMPLES available, not frames. */
        int n = op_read(of, buf + filled, (int)(cap - filled), NULL);
        if (n < 0) { free(buf); op_free(of); return -1; }
        if (n == 0) break;                  /* end of stream */
        filled += (size_t)n * (size_t)ch;
    }
    op_free(of);

    if (filled == 0) { free(buf); return -1; }

    int16_t *fit = (int16_t *)realloc(buf, filled * sizeof(int16_t));
    *out = fit ? fit : buf;                 /* shrink is best-effort */
    return (int)(filled / (size_t)ch);
}
#endif /* WCL_ENABLE_OPUS */
