/* Native harness for runtime/decoders.c + the sniffer's format dispatch.
 *
 * WHY NATIVE: the decoders are plain C over a memory buffer with no engine
 * state, so they can be exercised directly with a real assertion on the PCM
 * rather than through a cart. That the wasm build LINKS says nothing about
 * whether an mp3 turns into audio.
 *
 * WHAT IT ASSERTS: every fixture is the SAME 440 Hz stereo half-second tone,
 * so each decoder must agree on rate/channels/duration and produce a signal
 * whose dominant frequency is 440 Hz. A decoder that returns silence, noise,
 * or a channel-swapped/half-rate buffer fails here -- "it returned nonzero"
 * would not catch any of those.
 *
 * Build: see test/run-decoders.sh (needs the same -D__i386__ dance decoders.c
 * does internally, plus libopus when Opus is enabled).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

extern int wcl_decode_mp3(const unsigned char *, int, int16_t **, int *, int *);
extern int wcl_decode_flac(const unsigned char *, int, int16_t **, int *, int *);
#ifdef WCL_ENABLE_OPUS
extern int wcl_decode_opus(const unsigned char *, int, int16_t **, int *, int *);
#endif
/* stb_vorbis, via the engine's own vorbis.c translation unit. */
extern int stb_vorbis_decode_memory(const unsigned char *, int, int *, int *, short **);

static int failures = 0;
static int checks   = 0;

static void ok(int cond, const char *what, const char *detail) {
    checks++;
    if (cond) { printf("  ok   %s\n", what); return; }
    printf("  FAIL %s -- %s\n", what, detail ? detail : "");
    failures++;
}

static unsigned char *slurp(const char *path, int *size) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = (unsigned char *)malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "short read %s\n", path); exit(2); }
    fclose(f);
    *size = (int)n;
    return b;
}

/* Dominant frequency of channel 0 by Goertzel-scan: correlate against a sweep
 * of candidate bins and take the strongest. Cheaper than an FFT and enough to
 * tell 440 Hz from silence, noise, or a wrong-rate playback. */
static double dominant_hz(const int16_t *pcm, int frames, int channels, int rate) {
    double best_mag = -1, best_hz = 0;
    for (double hz = 100; hz <= 2000; hz += 1.0) {
        double w = 2.0 * M_PI * hz / rate;
        double coeff = 2.0 * cos(w), s0 = 0, s1 = 0, s2 = 0;
        for (int i = 0; i < frames; i++) {
            s0 = pcm[(size_t)i * channels] / 32768.0 + coeff * s1 - s2;
            s2 = s1; s1 = s0;
        }
        double mag = s1 * s1 + s2 * s2 - coeff * s1 * s2;
        if (mag > best_mag) { best_mag = mag; best_hz = hz; }
    }
    return best_hz;
}

static double rms(const int16_t *pcm, int frames, int channels) {
    double acc = 0;
    for (int i = 0; i < frames * channels; i++) {
        double s = pcm[i] / 32768.0;
        acc += s * s;
    }
    return sqrt(acc / (frames * channels));
}

typedef int (*decode_fn)(const unsigned char *, int, int16_t **, int *, int *);

/* Level of the source WAV every fixture was encoded from -- measured at
 * startup, never assumed. */
static double source_rms = 0;

/* Minimal 16-bit PCM WAV reader: enough for our own generated fixture, whose
 * layout we control. Walks the chunk list rather than assuming data starts at
 * byte 44 (ffmpeg writes a LIST/INFO chunk before it). */
static void measure_source_wav(const char *path) {
    int size = 0;
    unsigned char *b = slurp(path, &size);
    if (size < 44 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) {
        fprintf(stderr, "fixture %s is not a WAV\n", path); exit(2);
    }
    int off = 12;
    while (off + 8 <= size) {
        unsigned int clen = (unsigned)b[off+4] | ((unsigned)b[off+5] << 8) |
                            ((unsigned)b[off+6] << 16) | ((unsigned)b[off+7] << 24);
        if (memcmp(b + off, "data", 4) == 0) {
            int n = (int)clen; if (off + 8 + n > size) n = size - off - 8;
            const int16_t *s = (const int16_t *)(b + off + 8);
            source_rms = rms(s, n / (int)sizeof(int16_t), 1);
            free(b);
            return;
        }
        off += 8 + (int)clen + ((int)clen & 1);
    }
    fprintf(stderr, "no data chunk in %s\n", path);
    exit(2);
}

static int vorbis_shim(const unsigned char *d, int n, int16_t **out, int *ch, int *rate) {
    short *v = NULL;
    int frames = stb_vorbis_decode_memory(d, n, ch, rate, &v);
    *out = (int16_t *)v;
    return frames;
}

static void check_codec(const char *name, const char *path, decode_fn fn) {
    printf("%s (%s)\n", name, path);
    int size = 0;
    unsigned char *data = slurp(path, &size);

    int16_t *pcm = NULL; int ch = 0, rate = 0;
    int frames = fn(data, size, &pcm, &ch, &rate);

    char detail[256];
    snprintf(detail, sizeof detail, "frames=%d channels=%d rate=%d", frames, ch, rate);
    ok(frames > 0 && pcm != NULL, "decodes", detail);
    if (frames <= 0 || !pcm) { free(data); failures++; return; }

    ok(rate == 48000, "sample rate is 48000", detail);
    ok(ch == 2, "stereo", detail);

    /* 0.5s at 48k = 24000 frames. Lossy codecs pad with encoder delay, so
     * allow a tolerance rather than demanding an exact count. */
    snprintf(detail, sizeof detail, "frames=%d (want ~24000)", frames);
    ok(frames > 20000 && frames < 30000, "~0.5s of audio", detail);

    /* Compare LEVEL against the source WAV rather than a guessed constant.
     * ffmpeg's sine generator emits at rms 0.0625 (peak 2896), so an absolute
     * "loud enough" threshold picked by eye fails every codec while they are
     * all decoding correctly -- measure the oracle, do not assume it. A codec
     * that halved or doubled the amplitude still fails this. */
    double r = rms(pcm, frames, ch);
    snprintf(detail, sizeof detail, "rms=%.4f (source %.4f)", r, source_rms);
    ok(r > source_rms * 0.7 && r < source_rms * 1.4, "level matches the source WAV", detail);

    double hz = dominant_hz(pcm, frames, ch, rate);
    snprintf(detail, sizeof detail, "dominant=%.0f Hz (want 440)", hz);
    ok(hz > 430 && hz < 450, "tone is 440 Hz", detail);

    free(pcm);
    free(data);
}

/* Every decoder must REFUSE garbage rather than crash or invent audio. This is
 * the control that must fail: without it, a decoder that returns a bogus
 * nonzero frame count for any input would still pass every test above. */
static void check_rejects_garbage(const char *name, decode_fn fn) {
    unsigned char junk[4096];
    for (unsigned i = 0; i < sizeof junk; i++) junk[i] = (unsigned char)(i * 37u + 11u);
    int16_t *pcm = NULL; int ch = 0, rate = 0;
    int frames = fn(junk, (int)sizeof junk, &pcm, &ch, &rate);
    char d[128];
    snprintf(d, sizeof d, "returned frames=%d on random bytes", frames);
    ok(frames <= 0, "rejects garbage", d);
    if (frames > 0 && pcm) free(pcm);

    /* Truncation must not read past the buffer either. */
    pcm = NULL;
    frames = fn(junk, 3, &pcm, &ch, &rate);
    snprintf(d, sizeof d, "returned frames=%d on a 3-byte input", frames);
    ok(frames <= 0, "rejects a truncated buffer", d);
    if (frames > 0 && pcm) free(pcm);
    (void)name;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "test/audio-fixtures";
    char p[512];

    snprintf(p, sizeof p, "%s/tone.wav", dir);
    measure_source_wav(p);
    printf("source: %s rms=%.4f\n", p, source_rms);

    printf("== decoders ==\n");

    snprintf(p, sizeof p, "%s/tone.mp3", dir);
    check_codec("mp3", p, wcl_decode_mp3);
    snprintf(p, sizeof p, "%s/tone.flac", dir);
    check_codec("flac", p, wcl_decode_flac);
    snprintf(p, sizeof p, "%s/tone.ogg", dir);
    check_codec("vorbis", p, vorbis_shim);
#ifdef WCL_ENABLE_OPUS
    snprintf(p, sizeof p, "%s/tone.opus", dir);
    check_codec("opus", p, wcl_decode_opus);
#else
    printf("opus SKIPPED (engine built without WCL_ENABLE_OPUS)\n");
#endif

    printf("== garbage rejection ==\n");
    printf("mp3\n");  check_rejects_garbage("mp3", wcl_decode_mp3);
    printf("flac\n"); check_rejects_garbage("flac", wcl_decode_flac);
#ifdef WCL_ENABLE_OPUS
    printf("opus\n"); check_rejects_garbage("opus", wcl_decode_opus);
#endif

    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
