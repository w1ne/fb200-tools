/* Host tests for the bass delay (src/dsp/delay.c) and the rule that decides
 * when a preset plays it (src/preset/preset.h preset_delay_on).
 * Built by tests/test_delay.py; every check is an assert. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsp/delay.h"
#include "preset/preset.h"

#define LSB (1.0f / DELAY_SCALE)

static delay_t dl;
static int16_t line[DELAY_LEN];

static void run(float *x, unsigned n)
{
    for (unsigned o = 0; o < n; o += DSP_BLOCK)
        delay_process(&dl, x + o, n - o < DSP_BLOCK ? n - o : DSP_BLOCK);
}

static double rms(const float *x, unsigned n)
{
    double s = 0;
    for (unsigned i = 0; i < n; i++) s += (double)x[i] * x[i];
    return sqrt(s / n);
}

/* First echo at exactly D = time * fs samples, then each echo = fb x the one
 * before; nothing between the echoes (no filters on). */
static void test_impulse(float fs)
{
    const unsigned ms = 100, D = (unsigned)(ms * fs / 1000.0f + 0.5f), N = 4 * D + 10;
    float *x = calloc(N, sizeof *x);
    delay_init(&dl, fs, line);
    delay_set_params(&dl, ms, 50, 100, 0, 100);        /* fb 0.475, mix 1, filters off */
    x[0] = 0.5f;
    run(x, N);
    assert(x[0] == 0.5f);                               /* the dry signal is untouched */
    for (unsigned i = 1; i < N; i++) {
        if (i % D == 0) continue;
        assert(x[i] == 0.0f);
    }
    assert(fabsf(x[D] - 0.5f) <= LSB);
    double r2 = x[2 * D] / x[D], r3 = x[3 * D] / x[2 * D];
    assert(fabs(r2 - 0.475) < 1e-3 && fabs(r3 - 0.475) < 1e-3);
    printf("impulse %.0f Hz: first echo at %u (%u ms), level %.5f, decay %.4f %.4f (fb 0.475)\n",
           fs, D, ms, x[D], r2, r3);
    free(x);

    /* DELAY_MS_MIN and DELAY_MS_MAX: the ends of the range; the line holds the
     * longest at DELAY_FS_MAX */
    const unsigned ends[2] = {DELAY_MS_MIN, DELAY_MS_MAX};
    for (int k = 0; k < 2; k++) {
        unsigned d = (unsigned)(ends[k] * fs / 1000.0f + 0.5f);
        float *y = calloc(d + 64, sizeof *y);
        delay_init(&dl, fs, line);
        delay_set_params(&dl, ends[k] + (k ? 500u : 0u), 0, 100, 0, 100);   /* max + 500 clamps */
        y[0] = 1.0f;
        run(y, d + 64);
        for (unsigned i = 1; i < d + 64; i++) assert(i == d ? fabsf(y[i] - 1.0f) <= LSB : y[i] == 0.0f);
        free(y);
    }
}

/* Low cut on the repeats: a 40 Hz sine through a 150 Hz cut (knob 63) comes
 * back at the 2nd-order Butterworth level (f/fc)^2 / sqrt(1 + (f/fc)^4). */
static double wet_gain_db(float fs, float hz, unsigned lowcut, unsigned tone)
{
    const unsigned N = (unsigned)fs * 2;
    float *x = malloc(N * sizeof *x), *in = malloc(N * sizeof *in);
    for (unsigned i = 0; i < N; i++) in[i] = x[i] = 0.25f * sinf(6.2831853f * hz * (float)i / fs);
    delay_init(&dl, fs, line);
    delay_set_params(&dl, 50, 0, 100, lowcut, tone);
    run(x, N);
    for (unsigned i = 0; i < N; i++) x[i] -= in[i];     /* wet = out - dry */
    double g = 20 * log10(rms(x + N / 2, N / 2) / rms(in + N / 2, N / 2));
    free(x);
    free(in);
    return g;
}

static void test_lowcut(float fs)
{
    float fc = delay_lowcut_hz(63);
    assert(fc > 145.0f && fc < 155.0f);
    assert(delay_lowcut_hz(0) == 0.0f);
    assert(fabsf(delay_lowcut_hz(1) - 20.6f) < 0.2f && fabsf(delay_lowcut_hz(100) - 500.0f) < 0.5f);
    double r = 40.0 / fc, want = 20 * log10(r * r / sqrt(1 + r * r * r * r));
    double got = wet_gain_db(fs, 40.0f, 63, 100);
    assert(fabs(got - want) < 0.3);
    double open = wet_gain_db(fs, 40.0f, 0, 100);          /* low cut off: full level */
    assert(fabs(open) < 0.05);
    double pass = wet_gain_db(fs, 1000.0f, 63, 100);       /* 1 kHz passes */
    assert(fabs(pass) < 0.1);
    double dark = wet_gain_db(fs, 8000.0f, 0, 0);          /* tone 0: 1 kHz low-pass */
    assert(dark < -15.0);
    printf("low cut %.0f Hz: 40 Hz wet %.2f dB (expected %.2f), off %.3f dB, 1 kHz %.3f dB; "
           "tone 0 at 8 kHz %.1f dB\n", fs, got, want, open, pass, dark);
}

/* Full feedback, then silence: the tail dies to exact zeros, never NaN,
 * never a subnormal (in the output or the filter state). */
static void test_silence(float fs)
{
    const unsigned N = (unsigned)fs * 30;
    delay_init(&dl, fs, line);
    delay_set_params(&dl, DELAY_MS_MIN, 100, 100, 30, 50);
    float blk[DSP_BLOCK];
    unsigned last_nonzero = 0;
    for (unsigned o = 0; o < N; o += DSP_BLOCK) {
        memset(blk, 0, sizeof blk);
        if (o == 0) blk[0] = 0.9f;
        delay_process(&dl, blk, DSP_BLOCK);
        for (unsigned i = 0; i < DSP_BLOCK; i++) {
            assert(isfinite(blk[i]));
            assert(fpclassify(blk[i]) != FP_SUBNORMAL);
            if (blk[i] != 0.0f) last_nonzero = o + i;
        }
        const float st[5] = {dl.hx1, dl.hx2, dl.hy1, dl.hy2, dl.lz};
        for (int k = 0; k < 5; k++) assert(isfinite(st[k]) && fpclassify(st[k]) != FP_SUBNORMAL);
    }
    for (unsigned i = 0; i < DELAY_LEN; i++) assert(line[i] == 0);
    assert(last_nonzero < N - (unsigned)fs);            /* silent well before the end */
    printf("silence %.0f Hz: fb 100 tail silent after %.2f s, no NaN/subnormal\n", fs,
           last_nonzero / fs);
}

/* A time change glides (no jump), and settles on the new time exactly. */
static void test_glide(float fs)
{
    delay_init(&dl, fs, line);
    delay_set_params(&dl, 100, 0, 100, 0, 100);
    float blk[DSP_BLOCK];
    for (int b = 0; b < 50; b++) { for (int i = 0; i < DSP_BLOCK; i++) blk[i] = 0.1f; delay_process(&dl, blk, DSP_BLOCK); }
    delay_set_params(&dl, 300, 0, 100, 0, 100);
    assert(dl.d != dl.d_t);                            /* not a jump */
    for (unsigned b = 0; b < (unsigned)fs / DSP_BLOCK; b++) {
        memset(blk, 0, sizeof blk);
        delay_process(&dl, blk, DSP_BLOCK);
        for (int i = 0; i < DSP_BLOCK; i++) assert(isfinite(blk[i]) && fabsf(blk[i]) <= 0.1f + LSB);
    }
    assert(dl.d == (float)(unsigned)(0.3f * fs + 0.5f));
    printf("glide %.0f Hz: 100 -> 300 ms settles on %u samples\n", fs, (unsigned)dl.d);
}

/* Stock presets never play the delay: every factory preset has the stock
 * delay block on (1, 0, 9, 18, 490) and no marker. */
static void test_preset_rule(void)
{
    preset_t p;
    memset(&p, 0, sizeof p);
    const uint16_t stock[5] = {1, 0, 9, 18, 490};
    for (int k = 0; k < 5; k++) pset(&p, P_DLY_EN + 2u * k, stock[k]);
    assert(!preset_delay_on(&p));
    pset(&p, P_DLY_MARK, 0xffff);                      /* erased flash */
    assert(!preset_delay_on(&p));
    pset(&p, P_DLY_MARK, 0x444c);                      /* byte-swapped */
    assert(!preset_delay_on(&p));
    pset(&p, P_DLY_MARK, DLY_MARK);
    assert(p.b[0x96] == 'D' && p.b[0x97] == 'L');
    assert(preset_delay_on(&p));
    pset(&p, P_DLY_EN, 0);
    assert(!preset_delay_on(&p));
    printf("preset rule OK\n");
}

/* --presets: stock preset records (0x100 each) on stdin, e.g. the factory
 * presets from the user's stock image (tests/test_delay.py). */
static int check_presets(void)
{
    preset_t p;
    unsigned n = 0, on = 0;
    while (fread(p.b, 1, sizeof p.b, stdin) == sizeof p.b) {
        n++;
        if (preset_delay_on(&p)) on++;
    }
    printf("presets: %u checked, %u play the delay\n", n, on);
    return on != 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--presets") == 0) return check_presets();
    test_preset_rule();
    const float rates[2] = {(float)DELAY_FS_MAX, 32000.0f};   /* the pedal runs 44.1 kHz */
    for (int k = 0; k < 2; k++) {
        test_impulse(rates[k]);
        test_lowcut(rates[k]);
        test_silence(rates[k]);
        test_glide(rates[k]);
    }
    printf("delay host tests OK\n");
    return 0;
}
