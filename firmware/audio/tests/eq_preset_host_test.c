/* Host tests for the bass EQ in the preset (src/preset/preset.h P_EQ_MARK,
 * src/dsp/eq.c eq_load/eq_save). Built by tests/test_eq_preset.py; every
 * check is an assert.
 *
 * 1. The rule: no marker (0, erased flash, byte-swapped) = EQ off with the
 *    default settings; the marker + on byte = on.
 * 2. The record: the byte layout, save -> load -> save is the identity,
 *    off-grid values round to the grid, garbage clamps (no NaN, no inf).
 * 3. Loading glides (like the setters); loading the same record again does
 *    not restart the glide; a preset without the marker glides to off and
 *    then runs bit-exact.
 * --presets: stock preset records (0x100 each) on stdin, e.g. the factory
 * presets from the stock image: none has the marker. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsp/eq.h"
#include "preset/preset.h"

static eq_t e;

static void test_rule(void)
{
    preset_t p;
    memset(&p, 0, sizeof p);
    assert(!preset_eq(&p) && !preset_eq_on(&p));
    memset(&p, 0xff, sizeof p);                         /* erased flash */
    assert(!preset_eq(&p) && !preset_eq_on(&p));
    memset(&p, 0, sizeof p);
    p.b[P_EQ_DATA] = 1;                                 /* on byte, no marker */
    assert(!preset_eq(&p) && !preset_eq_on(&p));
    pset(&p, P_EQ_MARK, 0x4551);                        /* byte-swapped */
    assert(!preset_eq(&p));
    pset(&p, P_EQ_MARK, EQ_MARK);
    assert(p.b[0xc4] == 'E' && p.b[0xc5] == 'Q');
    assert(preset_eq(&p) == &p.b[0xc6] && preset_eq_on(&p));
    p.b[P_EQ_DATA] = 0;
    assert(preset_eq(&p) && !preset_eq_on(&p));
    /* after the module order (0xbc..0xc3), inside the record */
    assert(P_EQ_MARK == 0xbc + 8 && P_EQ_DATA + EQ_REC <= PRESET_SIZE && EQ_REC == 24);

    /* no marker: off and the defaults of eq_init, whatever was loaded before */
    eq_t ref;
    eq_init(&ref, 44100);
    eq_init(&e, 44100);
    eq_set_on(&e, 1);
    eq_set_hpf(&e, 80);
    eq_set_lpf(&e, 5000);
    eq_set_band(&e, 2, 300, -6, 2);
    eq_load(&e, NULL);
    assert(!e.on && e.hpf == 0.0f && e.lpf == 0.0f);
    assert(memcmp(e.f, ref.f, sizeof e.f) == 0 && memcmp(e.g, ref.g, sizeof e.g) == 0 &&
           memcmp(e.q, ref.q, sizeof e.q) == 0);
    printf("preset rule OK\n");
}

static void test_record(void)
{
    eq_init(&e, 44100);
    eq_set_on(&e, 1);
    eq_set_hpf(&e, 45);
    eq_set_lpf(&e, 6000);
    eq_set_band(&e, 0, 40, 3.5f, 0.7f);
    eq_set_band(&e, 1, 100, -4.5f, 1.4f);
    eq_set_band(&e, 2, 250, 15, 4);
    eq_set_band(&e, 3, 800, -15, 0.3f);
    eq_set_band(&e, 4, 10000, 0.125f, 1);
    uint8_t r[EQ_REC], r2[EQ_REC];
    eq_save(&e, r);
    static const uint8_t want[EQ_REC] = {
        1, 45, 0x70, 0x17,                              /* on, HPF 45 Hz, LPF 6000 Hz */
        40, 0, 28, 35,                                  /* 40 Hz, +3.5 dB x 8, Q 0.7 x 50 */
        100, 0, (uint8_t)-36, 70,                       /* 100 Hz, -4.5 dB, Q 1.4 */
        250, 0, 120, 200,                               /* +15 dB, Q 4 */
        0x20, 3, (uint8_t)-120, 15,                     /* 800 Hz, -15 dB, Q 0.3 */
        0x10, 0x27, 1, 50};                             /* 10 kHz, +0.125 dB, Q 1 */
    assert(memcmp(r, want, sizeof r) == 0);
    eq_t a = e;
    eq_init(&e, 48000);
    eq_load(&e, r);                                     /* on the grid: exact */
    assert(e.on && e.hpf == a.hpf && e.lpf == a.lpf);
    for (unsigned b = 0; b < EQ_BANDS; b++)
        assert(e.f[b] == a.f[b] && e.g[b] == a.g[b] && fabsf(e.q[b] - a.q[b]) < 1e-6f);
    eq_save(&e, r2);
    assert(memcmp(r, r2, sizeof r) == 0);

    /* off the grid: the nearest step */
    eq_set_band(&e, 1, 123.4f, -4.3f, 0.71f);
    eq_set_hpf(&e, 57.6f);
    eq_save(&e, r);
    eq_load(&e, r);
    assert(e.f[1] == 123.0f && e.g[1] == -4.25f && fabsf(e.q[1] - 0.72f) < 1e-6f && e.hpf == 58.0f);
    eq_set_lpf(&e, 0);
    eq_set_hpf(&e, 0);
    eq_set_on(&e, 0);
    eq_save(&e, r);
    assert(r[0] == 0 && r[1] == 0 && r[2] == 0 && r[3] == 0);

    /* garbage (erased flash with our marker): clamped, finite */
    memset(r, 0xff, sizeof r);
    eq_load(&e, r);
    assert(e.on && e.hpf == EQ_HPF_MAX && e.lpf == EQ_LPF_MAX);
    for (unsigned b = 0; b < EQ_BANDS; b++)
        assert(e.f[b] == EQ_BAND_MAX && e.g[b] == -0.125f && e.q[b] == EQ_Q_MAX);
    memset(r, 0, sizeof r);                             /* 0 Hz, Q 0: the minimums */
    eq_load(&e, r);
    for (unsigned b = 0; b < EQ_BANDS; b++)
        assert(e.f[b] == EQ_BAND_MIN && e.g[b] == 0.0f && e.q[b] == EQ_Q_MIN);
    float x[4 * DSP_BLOCK];
    for (unsigned i = 0; i < 4 * DSP_BLOCK; i++) x[i] = (float)(i % 7) * 0.1f - 0.3f;
    memset(r, 0xff, sizeof r);
    eq_load(&e, r);
    for (unsigned k = 0; k < 40; k++) {
        eq_process(&e, x, 4 * DSP_BLOCK);
        for (unsigned i = 0; i < 4 * DSP_BLOCK; i++) assert(isfinite(x[i]) && fabsf(x[i]) < 100.0f);
    }
    printf("record OK: layout, round trip, grid, clamps\n");
}

/* Max |second difference| of y: a sine's is A w^2; a click is a spike. */
static float d2max(const float *y, unsigned n)
{
    float m = 0.0f;
    for (unsigned i = 2; i < n; i++) {
        float d = fabsf(y[i] - 2.0f * y[i - 1] + y[i - 2]);
        if (d > m) m = d;
    }
    return m;
}

/* A preset change while a 100 Hz sine plays: from `from` to `to` (NULL =
 * a stock preset) in the middle. Returns max |d2 y| around the change over
 * the larger of the settled ones before and after (1.0: no step at all).
 * hard: clear the filter state at the change as well (the negative control:
 * that clicks). */
static float change(const uint8_t *from, const uint8_t *to, int hard)
{
    enum { B = 256, N = B * DSP_BLOCK, Q = N / 4 };
    static float y[N];
    for (unsigned i = 0; i < N; i++) y[i] = 0.25f * sinf(2.0f * 3.14159265f * 100.0f * (float)i / 44100.0f);
    eq_init(&e, 44100);
    eq_load(&e, from);
    for (unsigned o = 0; o < N; o += DSP_BLOCK) {
        if (o == N / 2) {
            eq_load(&e, to);
            if (hard) eq_reset(&e);
        }
        eq_process(&e, y + o, DSP_BLOCK);
    }
    float before = d2max(y + Q, Q), after = d2max(y + 3 * Q, Q);
    return d2max(y + 2 * Q - 2, Q + 2) / (before > after ? before : after);
}

static void test_glide(void)
{
    uint8_t r[EQ_REC];
    eq_init(&e, 44100);
    eq_set_on(&e, 1);
    eq_set_hpf(&e, 60);
    eq_set_band(&e, 1, 100, 9, 1.4f);
    eq_save(&e, r);

    eq_init(&e, 44100);
    eq_load(&e, r);                                     /* off -> on: a glide */
    assert(e.ramp[0] == EQ_RAMP_BLOCKS && e.ramp[2] == EQ_RAMP_BLOCKS && !e.bypass);
    float z[DSP_BLOCK] = {0};
    for (unsigned b = 0; b < EQ_RAMP_BLOCKS; b++) eq_process(&e, z, DSP_BLOCK);
    eq_load(&e, r);                                     /* again (every edit re-applies) */
    for (unsigned s = 0; s < EQ_STAGES; s++) assert(e.ramp[s] == 0);

    float on = change(NULL, r, 0), off = change(r, NULL, 0), hard = change(r, NULL, 1);
    printf("preset change, max |d2 y| over the settled sine's: off->on %.3f, on->off %.3f, "
           "control (state cleared) %.1f\n", on, off, hard);
    assert(on < 1.05f && off < 1.05f);
    assert(hard > 10.0f);

    /* after the glide to a stock preset: bypassed, then bit-exact */
    eq_init(&e, 44100);
    eq_load(&e, r);
    float x[DSP_BLOCK], y[DSP_BLOCK];
    unsigned k = 0, off_at = 0;
    for (; k < 4000 && !(off_at && e.bypass); k++) {
        for (unsigned i = 0; i < DSP_BLOCK; i++) y[i] = 0.1f * sinf((float)(k * DSP_BLOCK + i) * 0.01f);
        if (k == 20) { eq_load(&e, NULL); off_at = k; }
        eq_process(&e, y, DSP_BLOCK);
    }
    assert(e.bypass);
    for (unsigned i = 0; i < DSP_BLOCK; i++) x[i] = y[i] = 0.1f * sinf((float)(k * DSP_BLOCK + i) * 0.01f);
    eq_process(&e, y, DSP_BLOCK);
    assert(memcmp(x, y, sizeof x) == 0);
    printf("to a stock preset: bypassed after %.1f ms, then bit-exact\n",
           (double)((k - off_at) * DSP_BLOCK) / 44.1);
    assert(k - off_at < 44100 / DSP_BLOCK);
    printf("glide OK\n");
}

static int check_presets(void)
{
    preset_t p;
    unsigned n = 0, marked = 0, on = 0;
    while (fread(p.b, 1, sizeof p.b, stdin) == sizeof p.b) {
        n++;
        if (preset_eq(&p)) marked++;
        if (preset_eq_on(&p)) on++;
        for (unsigned i = P_EQ_MARK; i < P_EQ_DATA + EQ_REC; i++) if (p.b[i]) marked++;
    }
    printf("presets: %u checked, %u with EQ bytes, %u play the EQ\n", n, marked, on);
    return marked != 0 || on != 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--presets") == 0) return check_presets();
    test_rule();
    test_record();
    test_glide();
    printf("eq preset host tests OK\n");
    return 0;
}
