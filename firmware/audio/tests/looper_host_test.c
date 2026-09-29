/* Host tests for the looper (src/dsp/looper.c) and its borrowed memory
 * (src/dsp/loop_mem.c). Built by tests/test_looper.py; every check is an
 * assert. Prints the measured quality (SNR / THD of a recorded sine). */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsp/looper.h"
#include "dsp/loop_mem.h"

#define FS 44100.0
#define TWO_PI_ (2 * 3.14159265358979)
#define GUARD 64
#define SEC 44096u                     /* ~1 s in whole blocks */
/* the engine's areas: the delay line and the conv2 tail */
#define BYTES0 (DELAY_LEN * 2u)
#define BYTES1 (sizeof(conv2_tail_t))

static looper_t lp;
static uint8_t *mem0, *mem1;          /* with GUARD bytes on both sides */

static void mem_new(void)
{
    free(mem0);
    free(mem1);
    mem0 = malloc(BYTES0 + 2 * GUARD);
    mem1 = malloc(BYTES1 + 2 * GUARD);
    memset(mem0, 0xA5, BYTES0 + 2 * GUARD);
    memset(mem1, 0xA5, BYTES1 + 2 * GUARD);
}

static void guards_ok(void)
{
    for (unsigned i = 0; i < GUARD; i++) {
        assert(mem0[i] == 0xA5 && mem0[GUARD + BYTES0 + i] == 0xA5);
        assert(mem1[i] == 0xA5 && mem1[GUARD + BYTES1 + i] == 0xA5);
    }
}

static void fresh(int hq)
{
    mem_new();
    looper_init(&lp);
    assert(looper_set_hq(&lp, hq) == 0);
    looper_attach(&lp, mem0 + GUARD, BYTES0, mem1 + GUARD, BYTES1);
    assert(lp.state == LOOPER_EMPTY);
}

/* n samples (multiple of DSP_BLOCK) of x through the looper; y = what the
 * looper added (L; R must be the same). x may be NULL (silence). */
static void run(const float *x, float *y, size_t n)
{
    float l[DSP_BLOCK], r[DSP_BLOCK];
    assert(n % DSP_BLOCK == 0);                   /* the engine's blocks */
    for (size_t o = 0; o < n; o += DSP_BLOCK) {
        for (unsigned i = 0; i < DSP_BLOCK; i++) l[i] = r[i] = x ? x[o + i] : 0.0f;
        looper_process(&lp, l, r, DSP_BLOCK);
        for (unsigned i = 0; i < DSP_BLOCK; i++) {
            float in = x ? x[o + i] : 0.0f;
            assert(l[i] == r[i]);
            if (y) y[o + i] = l[i] - in;
        }
        looper_poll(&lp);                 /* the main loop runs between blocks */
    }
}

/* n rounded up to whole blocks */
static size_t B(size_t n) { return (n + DSP_BLOCK - 1) / DSP_BLOCK * DSP_BLOCK; }

static float *sine(size_t n, double f, double a, double ph)
{
    float *x = malloc(n * sizeof *x);
    for (size_t i = 0; i < n; i++) x[i] = (float)(a * sin(TWO_PI_ * f * (double)i / FS + ph));
    return x;
}

/* least squares: y ~ sum_h (a_h cos + b_h sin)(h f) + c over [0, n):
 * amplitude of each harmonic 1..H, residual rms */
#define HMAX 5
static void fit(const float *y, size_t n, double f, int H, double amp[HMAX + 1], double *res)
{
    /* the harmonics are near-orthogonal over many periods: project, then
     * subtract (two passes refine) */
    double *e = malloc(n * sizeof *e);
    for (size_t i = 0; i < n; i++) e[i] = y[i];
    double A[HMAX + 1] = {0}, B[HMAX + 1] = {0};
    for (int pass = 0; pass < 3; pass++) {
        for (int h = 1; h <= H; h++) {
            double sa = 0, sb = 0, cc = 0, ss = 0;
            for (size_t i = 0; i < n; i++) {
                double w = TWO_PI_ * f * h * (double)i / FS, c = cos(w), s = sin(w);
                sa += e[i] * c; sb += e[i] * s; cc += c * c; ss += s * s;
            }
            double da = sa / cc, db = sb / ss;
            A[h] += da; B[h] += db;
            for (size_t i = 0; i < n; i++) {
                double w = TWO_PI_ * f * h * (double)i / FS;
                e[i] -= da * cos(w) + db * sin(w);
            }
        }
    }
    double r = 0;
    for (size_t i = 0; i < n; i++) r += e[i] * e[i];
    *res = sqrt(r / (double)n);
    for (int h = 1; h <= H; h++) amp[h] = hypot(A[h], B[h]);
    free(e);
}

/* ------------------------------------------------------------ codec */

static void test_adpcm(void)
{
    static const double freqs[] = {41.2, 110.0, 440.0, 1000.0, 3000.0};
    for (unsigned k = 0; k < sizeof freqs / sizeof freqs[0]; k++) {
        for (double lvl = 0.5; lvl > 0.01; lvl /= 10.0) {
            adpcm_t e = {0, 0}, d = {0, 0};
            double s2 = 0, n2 = 0;
            for (int i = 0; i < 22050; i++) {
                double v = lvl * 32767 * sin(TWO_PI_ * freqs[k] * i / 22050.0);
                unsigned nib = looper_adpcm_enc(&e, (int)v);
                int out = looper_adpcm_dec(&d, nib);
                assert(out == e.pred && d.idx == e.idx);     /* encoder tracks the decoder */
                if (i >= 2205) { s2 += v * v; n2 += (out - v) * (out - v); }
            }
            double snr = 10 * log10(s2 / n2);
            printf("adpcm %6.1f Hz %5.1f dBFS @22.05k: SNR %.1f dB\n", freqs[k],
                   20 * log10(lvl), snr);
            assert(snr > 20.0);
        }
    }
    /* extremes: full scale square and silence stay in range */
    adpcm_t e = {0, 0}, d = {0, 0};
    for (int i = 0; i < 1000; i++) {
        int v = (i / 50) & 1 ? 32767 : -32768;
        int out = looper_adpcm_dec(&d, looper_adpcm_enc(&e, v));
        assert(out >= -32768 && out <= 32767);
    }
    printf("adpcm: round trip OK\n");
}

/* ------------------------------------------------------------ quality */

/* Record a sine (1.5 s), close, play the next pass with no input; fit the
 * loop output away from the wrap: SNR (all but the fundamental) and THD. */
static void quality(int hq, double f, double a, double *snr, double *thd)
{
    fresh(hq);
    const size_t R = 66144, P = R + SEC;       /* record, then play past a wrap */
    float *x = sine(R, f, a, 0.3), *y = malloc(P * sizeof *y);
    assert(looper_cmd(&lp, LOOPER_TAP) == 0);
    run(x, NULL, R);
    assert(looper_cmd(&lp, LOOPER_TAP) == 0 && lp.state == LOOPER_REC);   /* closes at once */
    run(NULL, y, P);
    assert(lp.state == LOOPER_PLAY && lp.passes >= 1);
    /* the loop plays pass 2 at y[len_full..): take 0.5 s from 0.3 s in */
    size_t len = (size_t)lp.len * (hq ? 1u : 2u), off = len + 13230, n = 22050;
    assert(off + n <= P);
    double amp[HMAX + 1], res;
    /* time base: the loop sample i plays at y[len * k + i + latency]; the
     * fit has a free phase, so only the frequency matters */
    fit(y + off, n, f, HMAX, amp, &res);
    double h = 0;
    for (int k = 2; k <= HMAX; k++) h += amp[k] * amp[k];
    /* residual (noise + all harmonics) vs the fundamental */
    double nd = sqrt(res * res + h / 2);
    *snr = 20 * log10(amp[1] / sqrt(2) / nd);
    *thd = 10 * log10(h / (amp[1] * amp[1]) + 1e-30);
    assert(fabs(amp[1] / a - 1.0) < 0.02);       /* unity gain at level 100 */
    guards_ok();
    free(x);
    free(y);
}

static void test_quality(void)
{
    static const double freqs[] = {55.0, 110.0, 440.0, 2000.0};
    for (int hq = 0; hq <= 1; hq++) {
        for (unsigned k = 0; k < 4; k++) {
            for (double a = 0.5; a > 0.004; a /= 10.0) {
                double snr, thd;
                quality(hq, freqs[k], a, &snr, &thd);
                printf("looper %s %6.1f Hz %5.1f dBFS: SNR %.1f dB, THD %.1f dB\n",
                       hq ? "hq   " : "22.05k", freqs[k], 20 * log10(a), snr, thd);
                /* ADPCM's noise follows the slope: -6 dB per octave */
                assert(snr > (freqs[k] < 500.0 ? 30.0 : 20.0));
                assert(thd < -30.0);
            }
        }
    }
}

/* ------------------------------------------------------------ timing */

/* A click recorded at a known time repeats every len samples exactly, over
 * many passes: sample-accurate loop points, no drift. */
static void test_loop_points(void)
{
    for (int hq = 0; hq <= 1; hq++) {
        fresh(hq);
        const size_t R = 30016, P = 10 * 32000;
        float *x = calloc(R, sizeof *x), *y = malloc(P * sizeof *y);
        for (int i = -20; i <= 20; i++)             /* band-limited burst at 1000 */
            x[1000 + i] = (float)(0.5 * (1 + cos(3.14159265358979 * i / 21.0)) * 0.5);
        assert(looper_cmd(&lp, LOOPER_REC_A) == 0);
        run(x, NULL, R);
        assert(looper_cmd(&lp, LOOPER_REC_A) == 0);   /* close */
        run(NULL, y, P);
        size_t len = (size_t)lp.len * (hq ? 1u : 2u);
        assert(len == R + (hq ? 1u : 2u));             /* the tap's block + one sample */
        size_t prev = 0;
        int peaks = 0;
        for (size_t k = 0; (k + 1) * len < P; k++) {
            size_t best = k * len;
            for (size_t i = k * len; i < (k + 1) * len; i++)
                if (fabsf(y[i]) > fabsf(y[best])) best = i;
            if (k) assert(best - prev == len);
            prev = best;
            peaks++;
        }
        assert(peaks >= 9);
        printf("loop points %s: len %zu samples, %d passes, period exact\n",
               hq ? "hq" : "22.05k", len, peaks);
        free(x);
        free(y);
    }
}

/* max |second difference| over [a, b) */
static double d2max(const float *y, size_t a, size_t b)
{
    double m = 0;
    for (size_t i = a + 2; i < b; i++) {
        double d = fabs((double)y[i] - 2.0 * y[i - 1] + y[i - 2]);
        if (d > m) m = d;
    }
    return m;
}

/* The first record closes on a sine at a random phase: the wrap (and the
 * fades of stop/play and of a punch in/out) must not click. The codec's own
 * noise sets the floor of the second difference; a hard edge of this sine
 * (a splice at the wrap, or a 0.2 step) is > 0.1. */
#define CLICK 0.02
static void test_no_click(void)
{
    const double f = 110.0, a = 0.3;
    for (int hq = 0; hq <= 1; hq++) {
        fresh(hq);
        const size_t R = 40000, T = 576, P = 4 * R;
        float *x = sine(R + P, f, a, 0.0), *y = malloc(P * sizeof *y);
        assert(looper_cmd(&lp, LOOPER_REC_A) == 0);
        run(x, NULL, R);
        assert(looper_cmd(&lp, LOOPER_PLAY_A) == 0);
        run(x + R, NULL, T);                        /* the tail: the sine goes on live */
        run(NULL, y, P);                            /* then the loop alone, 3+ wraps */
        size_t len = (size_t)lp.len * (hq ? 1u : 2u);
        double step = fabs(a * sin(TWO_PI_ * f * (double)len / FS) - a * sin(0.0));
        double mid = d2max(y, len - T + len / 4, len - T + 3 * len / 4), wrap = 0;
        for (size_t k = 1; k <= 3; k++) {
            double w = d2max(y, k * len - T - 400, k * len - T + 400);
            if (w > wrap) wrap = w;
        }
        printf("wrap %s: max |d2| at the wraps %.2e, mid-loop %.2e (a hard splice steps %.3f)\n",
               hq ? "hq" : "22.05k", wrap, mid, step);
        assert(step > 0.1);                        /* the test has a splice to hide */
        assert(wrap < 2.0 * mid && wrap < CLICK);
        /* stop and play: faded */
        assert(looper_cmd(&lp, LOOPER_STOP_A) == 0);
        run(NULL, y, 4096);
        assert(lp.state == LOOPER_STOP);
        assert(looper_cmd(&lp, LOOPER_PLAY_A) == 0);
        run(NULL, y + 4096, 4096);
        double m = d2max(y, 0, 8192);
        printf("stop/play %s: max |d2| %.2e\n", hq ? "hq" : "22.05k", m);
        assert(m < CLICK);
        /* punch in and out on a DC step of the input: the dub ramps */
        float *dc = malloc(8192 * sizeof *dc);
        for (int i = 0; i < 8192; i++) dc[i] = 0.2f;
        assert(looper_cmd(&lp, LOOPER_DUB_A) == 0);
        run(dc, NULL, 2048);
        assert(looper_cmd(&lp, LOOPER_PLAY_A) == 0);
        run(dc, NULL, 2048);
        run(NULL, y, P);                            /* the dubbed section plays back */
        m = d2max(y, 0, P);
        printf("punch in/out %s: max |d2| %.2e\n", hq ? "hq" : "22.05k", m);
        assert(m < CLICK);
        guards_ok();
        free(dc);
        free(x);
        free(y);
    }
}

/* ------------------------------------------------------------ states */

/* the amplitude of f in y[a, a + n) */
static double amp_at(const float *y, size_t a, size_t n, double f)
{
    double amp[HMAX + 1], res;
    fit(y + a, n, f, 1, amp, &res);
    return amp[1];
}

static void test_states(void)
{
    const double fa = 220.0, fb = 330.0, a = 0.2;
    fresh(0);
    assert(looper_cmd(&lp, LOOPER_PLAY_A) == -1);    /* nothing to play */
    assert(looper_cmd(&lp, LOOPER_UNDO_A) == -1);
    const size_t R = SEC, N = 3 * R;
    float *xa = sine(N, fa, a, 0), *xb = sine(N, fb, a, 1), *y = malloc(N * sizeof *y);
    assert(looper_cmd(&lp, LOOPER_TAP) == 0 && lp.state == LOOPER_REC);
    assert(looper_set_hq(&lp, 1) == -1);              /* not with a loop */
    run(xa, NULL, R);
    assert(looper_cmd(&lp, LOOPER_TAP) == 0);
    run(NULL, y, 2 * R);
    assert(lp.state == LOOPER_PLAY && lp.banks == 2);
    assert(looper_cmd(&lp, LOOPER_REC_A) == -1);      /* clear first */
    size_t len = lp.len * 2u;
    assert(fabs(amp_at(y, len + 4000, 22050, fa) - a) < 0.01);
    /* dub B over more than a pass */
    assert(looper_cmd(&lp, LOOPER_TAP) == 0 && lp.state == LOOPER_DUB && lp.alt);
    looper_info_t in;
    run(xb, NULL, B(len + 8192));
    assert(looper_cmd(&lp, LOOPER_TAP) == 0 && lp.state == LOOPER_PLAY);
    run(NULL, y, B(2 * len));
    double ga = amp_at(y, len, 22050, fa), gb = amp_at(y, len, 22050, fb);
    printf("dub: old layer %.3f (x %.3f), new %.3f\n", ga, ga / a, gb);
    /* the part dubbed twice (the first 8192 samples) was faded twice */
    assert(fabs(ga / a - LOOPER_FB) < 0.02 && fabs(gb - a) < 0.015);
    looper_info(&lp, &in);
    assert(in.undo == 1);
    /* undo: A alone, redo: both */
    assert(looper_cmd(&lp, LOOPER_UNDO_A) == 0);
    run(NULL, y, B(2 * len));
    looper_info(&lp, &in);
    assert(in.undo == 2 && lp.state == LOOPER_PLAY);
    ga = amp_at(y, len, 22050, fa);
    gb = amp_at(y, len, 22050, fb);
    printf("undo: A %.3f, B %.4f\n", ga, gb);
    assert(fabs(ga - a) < 0.01 && gb < 0.002);
    assert(looper_cmd(&lp, LOOPER_UNDO_A) == 0);       /* redo */
    run(NULL, y, B(2 * len));
    looper_info(&lp, &in);
    assert(in.undo == 1);
    assert(fabs(amp_at(y, len, 22050, fb) - a) < 0.015);
    /* stop: silent, position 0; play restarts */
    assert(looper_cmd(&lp, LOOPER_STOP_A) == 0);
    run(NULL, y, 8192);
    assert(lp.state == LOOPER_STOP && lp.pos == 0);
    for (int i = 4096; i < 8192; i++) assert(y[i] == 0.0f);
    assert(looper_cmd(&lp, LOOPER_UNDO_A) == 0);       /* undo while stopped: at once */
    looper_info(&lp, &in);
    assert(in.undo == 2);
    assert(looper_cmd(&lp, LOOPER_TAP) == 0 && lp.state == LOOPER_PLAY);
    run(NULL, y, B(len));
    assert(fabs(amp_at(y, 8000, 22050, fa) - a) < 0.01 && amp_at(y, 8000, 22050, fb) < 0.002);
    /* a new dub drops the redo */
    assert(looper_cmd(&lp, LOOPER_DUB_A) == 0);
    run(xb, NULL, 4096);
    assert(looper_cmd(&lp, LOOPER_PLAY_A) == 0);
    run(NULL, NULL, 4096);
    looper_info(&lp, &in);
    assert(in.undo == 1);
    /* level */
    looper_set_level(&lp, 50);
    run(NULL, y, B(2 * len));
    assert(fabs(amp_at(y, len + 8192 + 2000, 22050, fa) - 0.5 * a) < 0.006);
    looper_set_level(&lp, 100);
    /* clear: fades, then EMPTY; silent */
    assert(looper_cmd(&lp, LOOPER_CLEAR_A) == 0);
    run(NULL, y, 4096);
    assert(lp.state == LOOPER_EMPTY);
    for (int i = 2048; i < 4096; i++) assert(y[i] == 0.0f);
    assert(looper_set_hq(&lp, 1) == 0 && looper_set_hq(&lp, 0) == 0);
    /* rec -> dub at once (console `loop dub` while recording) */
    assert(looper_cmd(&lp, LOOPER_REC_A) == 0);
    run(xa, NULL, R);
    assert(looper_cmd(&lp, LOOPER_DUB_A) == 0);
    run(xb, NULL, 4096);
    assert(lp.state == LOOPER_DUB && lp.alt);
    /* clear while recording: at once */
    assert(looper_cmd(&lp, LOOPER_CLEAR_A) == 0);
    run(NULL, NULL, 4096);
    assert(looper_cmd(&lp, LOOPER_REC_A) == 0);
    assert(looper_cmd(&lp, LOOPER_CLEAR_A) == 0 && lp.state == LOOPER_EMPTY);
    /* a stop while recording closes the loop, stopped */
    assert(looper_cmd(&lp, LOOPER_REC_A) == 0);
    run(xa, NULL, R);
    assert(looper_cmd(&lp, LOOPER_STOP_A) == 0);
    run(NULL, y, 8192);
    assert(lp.state == LOOPER_STOP && lp.len > 0);
    guards_ok();
    printf("states: rec play dub undo redo stop clear OK\n");
    free(xa);
    free(xb);
    free(y);
}

/* A loop longer than half the memory: no undo, the dub goes in place; the
 * record closes by itself when the memory is full. */
static void test_long_loop(void)
{
    fresh(0);
    looper_info_t in;
    looper_info(&lp, &in);
    printf("memory: %u blocks, max %u ms (22.05k), undo up to %u ms\n", (unsigned)lp.total,
           in.max_ms, in.undo_max_ms);
    assert(in.max_ms > 16000 && in.undo_max_ms > 8000);
    size_t cap = (size_t)lp.total * LOOPER_BLK * 2u, N = cap + SEC;
    float *x = sine(N, 110.0, 0.2, 0), *y = malloc(N * sizeof *y);
    assert(looper_cmd(&lp, LOOPER_REC_A) == 0);
    run(x, NULL, N);                                 /* past the end: closes itself */
    assert(lp.state == LOOPER_PLAY && lp.len == lp.total * LOOPER_BLK && lp.banks == 1);
    assert(looper_cmd(&lp, LOOPER_UNDO_A) == -1);
    assert(looper_cmd(&lp, LOOPER_DUB_A) == 0 && !lp.alt);
    run(x, NULL, SEC);
    assert(looper_cmd(&lp, LOOPER_PLAY_A) == 0);
    run(NULL, y, SEC);
    assert(looper_cmd(&lp, LOOPER_UNDO_A) == -1);
    guards_ok();
    looper_set_hq(&lp, 1);                            /* refused: a loop exists */
    assert(!lp.hq);
    fresh(1);
    looper_info(&lp, &in);
    printf("memory hq: max %u ms, undo up to %u ms\n", in.max_ms, in.undo_max_ms);
    assert(in.max_ms > 8000 && in.undo_max_ms > 4000);
    free(x);
    free(y);
}

/* ------------------------------------------------------------ memory */

static delay_t dly;
static cab_t cab;

static void fill_ir(float *ir, unsigned taps)
{
    uint32_t seed = 7;
    for (unsigned i = 0; i < taps; i++) {
        seed = seed * 1664525u + 1013904223u;
        ir[i] = (float)(int32_t)seed * (1.0f / 2147483648.0f) * expf(-(float)i / 900.0f);
    }
}

/* the cab's impulse response over n samples (after silence) */
static void cab_ir(float *out, unsigned n)
{
    float z[DSP_BLOCK] = {0};
    for (int i = 0; i < 200; i++) { memset(z, 0, sizeof z); cab_process(&cab, z, DSP_BLOCK); }
    for (unsigned o = 0; o < n; o += DSP_BLOCK) {
        memset(z, 0, sizeof z);
        if (o == 0) z[0] = 1.0f;
        cab_process(&cab, z, DSP_BLOCK);
        memcpy(out + o, z, sizeof z);
    }
}

static int all_zero(const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) if (b[i]) return 0;
    return 1;
}

static void test_handover(void)
{
    int16_t *line = calloc(DELAY_LEN, sizeof *line);
    conv2_tail_t *tail = calloc(1, sizeof *tail);
    static float ir[CAB_MAX_TAPS], resp[8192];
    loop_mem_t m = {&lp, &dly, line, &cab, tail, 0};
    looper_init(&lp);
    delay_init(&dly, 44100.0f, line);
    delay_set_params(&dly, 500, 60, 50, 0, 100);
    cab_init_long(&cab, tail);
    fill_ir(ir, CAB_MAX_TAPS);
    assert(cab_set_ir_len(&cab, ir, CAB_MAX_TAPS, 1.0f) == 0);
    conv2_finish(&cab.conv);
    /* audio through both: the line and the tail hold signal */
    float *x = sine(44100, 110.0, 0.5, 0);
    for (size_t o = 0; o < 44100 - DSP_BLOCK; o += DSP_BLOCK) {
        float b[DSP_BLOCK];
        memcpy(b, x + o, sizeof b);
        cab_process(&cab, b, DSP_BLOCK);
        delay_process(&dly, b, DSP_BLOCK);
    }
    assert(!all_zero(line, DELAY_LEN * 2u) && !all_zero(tail->x, sizeof tail->x));
    cab_ir(resp, 8192);
    for (unsigned i = 600; i < 4096; i += 97)
        assert(fabsf(resp[i] - ir[i] * 1.15f) < 1e-4f);      /* the tail plays */

    /* take: the cab keeps its first 512 taps, long IRs fail, the looper
     * owns both areas */
    loop_mem_take(&m);
    assert(m.owned && lp.state == LOOPER_EMPTY && cab.conv.t == NULL && cab.conv.parts == 0);
    assert(lp.seg[0] == (uint8_t *)line && lp.seg[1] == (uint8_t *)tail);
    assert(lp.total == (DELAY_LEN * 2u) / LOOPER_BLK_BYTES + sizeof *tail / LOOPER_BLK_BYTES);
    assert(cab_set_ir_len(&cab, ir, CAB_MAX_TAPS, 1.0f) == -1);
    cab_ir(resp, 8192);
    for (unsigned i = 0; i < 512; i++) assert(fabsf(resp[i] - ir[i] * 1.15f) < 1e-4f);
    for (unsigned i = 512; i < 8192; i++) assert(fabsf(resp[i]) < 1e-6f);   /* FFT rounding */
    /* a loop over the whole memory: every byte of both areas is the loop's */
    assert(looper_cmd(&lp, LOOPER_REC_A) == 0);
    for (size_t k = 0; lp.state == LOOPER_REC; k++) {
        float l[DSP_BLOCK], r[DSP_BLOCK];
        for (int i = 0; i < DSP_BLOCK; i++) l[i] = r[i] = x[(k * DSP_BLOCK + (size_t)i) % 44000];
        looper_process(&lp, l, r, DSP_BLOCK);
        cab_process(&cab, l, DSP_BLOCK);        /* the cab runs on, head only */
    }
    assert(looper_cmd(&lp, LOOPER_CLEAR_A) == 0);
    for (int k = 0; k < 64 && lp.state != LOOPER_EMPTY; k++) {
        float l[DSP_BLOCK] = {0}, r[DSP_BLOCK] = {0};
        looper_process(&lp, l, r, DSP_BLOCK);
        looper_poll(&lp);
    }
    assert(lp.state == LOOPER_EMPTY);
    /* give: clean delay and tail, long IRs again */
    loop_mem_give(&m);
    assert(!m.owned && lp.state == LOOPER_OFF && cab.conv.t == tail);
    assert(all_zero(line, DELAY_LEN * 2u));
    assert(all_zero(tail->x, sizeof tail->x) && all_zero(tail->in, sizeof tail->in) &&
           all_zero(tail->acc, sizeof tail->acc) && all_zero(tail->y, sizeof tail->y));
    delay_set_params(&dly, 500, 60, 50, 0, 100);
    for (int k = 0; k < 2000; k++) {                 /* the delay on silence: exact zeros */
        float b[DSP_BLOCK] = {0};
        delay_process(&dly, b, DSP_BLOCK);
        for (int i = 0; i < DSP_BLOCK; i++) assert(b[i] == 0.0f);
    }
    assert(cab_set_ir_len(&cab, ir, CAB_MAX_TAPS, 1.0f) == 0);
    conv2_finish(&cab.conv);
    cab_ir(resp, 8192);
    for (unsigned i = 0; i < 4096; i += 7) assert(fabsf(resp[i] - ir[i] * 1.15f) < 1e-4f);
    for (unsigned i = 4096; i < 8192; i++) assert(fabsf(resp[i]) < 1e-6f);
    assert(looper_cmd(&lp, LOOPER_REC_A) == -2);       /* no memory */
    /* a long IR still loading when the looper takes the tail: dropped */
    assert(cab_set_ir_len(&cab, ir, 1024, 1.0f) == 0 && conv2_pending(&cab.conv));
    loop_mem_take(&m);
    assert(!conv2_pending(&cab.conv) && !cab.scale_due);
    loop_mem_give(&m);
    printf("handover: delay/long IR -> looper -> back, clean\n");
    free(x);
    free(line);
    free(tail);
}

int main(void)
{
    test_adpcm();
    test_quality();
    test_loop_points();
    test_no_click();
    test_states();
    test_long_loop();
    test_handover();
    free(mem0);
    free(mem1);
    printf("looper host tests OK\n");
    return 0;
}
