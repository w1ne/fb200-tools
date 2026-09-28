/* Host test for the two-stage convolver (dsp/conv2.c): built by
 * tests/test_dsp_host.py. Reference: a direct FIR in double precision. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsp/conv2.h"

static float frand(void) { return (float)rand() / (float)RAND_MAX * 2.0f - 1.0f; }

enum { LEN = 16384 };
static conv2_tail_t s_tail;
static conv2_t s_c, s_c2;
static float s_x[LEN], s_y[LEN], s_ir[CONV2_MAX_TAPS], s_ir2[CONV2_MAX_TAPS];
static double s_ref[LEN];

/* Random IR with a slow decay: the tail carries real energy. */
static void make_ir(float *ir, size_t taps, float tau)
{
    for (size_t i = 0; i < taps; i++) ir[i] = frand() * expf(-(float)i / tau);
}

/* ref[n] = sum ir[k] x[n - k]; for n >= swap the second IR (FIR with
 * coefficients swapped at sample swap). */
static void fir(const float *x, size_t len, const float *ir, size_t taps,
                const float *ir2, size_t taps2, size_t swap, double *ref)
{
    for (size_t n = 0; n < len; n++) {
        const float *h = n < swap ? ir : ir2;
        size_t t = n < swap ? taps : taps2;
        double acc = 0;
        for (size_t k = 0; k < t && k <= n; k++) acc += (double)h[k] * x[n - k];
        ref[n] = acc;
    }
}

/* max |y - ref| / max |ref| in dB */
static double err_db(const float *y, const double *ref, size_t len)
{
    double e = 0, r = 0;
    for (size_t n = 0; n < len; n++) {
        e = fmax(e, fabs(y[n] - ref[n]));
        r = fmax(r, fabs(ref[n]));
    }
    return 20 * log10(e / r + 1e-300);
}

/* Chunk sizes: 0 = full DSP_BLOCK blocks; 1 = odd part blocks (1..37, with
 * some over a frame: 300, 517) so chunks cross frame boundaries. */
static size_t chunk(int mode, size_t i)
{
    static const size_t odd[] = {1, 7, 31, 32, 33, 5, 300, 13, 37, 2, 517, 19, 29, 3};
    return mode ? odd[i % (sizeof odd / sizeof odd[0])] : DSP_BLOCK;
}

/* Run x through c in chunks; set_ir to ir2 at sample swap (a chunk edge). */
static void run(conv2_t *c, int mode, const float *x, float *y, size_t len,
                const float *ir2, size_t taps2, size_t swap)
{
    for (size_t i = 0, j = 0; i < len; j++) {
        if (i == swap) assert(conv2_set_ir(c, ir2, taps2) == 0);
        size_t m = chunk(mode, j);
        if (m > len - i) m = len - i;
        if (i < swap && i + m > swap) m = swap - i;
        conv2_process(c, x + i, y + i, m);
        i += m;
    }
}

static void test_vs_fir(size_t taps, int mode)
{
    make_ir(s_ir, taps, 1500.0f);
    fir(s_x, LEN, s_ir, taps, s_ir, taps, LEN, s_ref);
    assert(conv2_init(&s_c, &s_tail) == 0);
    assert(conv2_set_ir(&s_c, s_ir, taps) == 0);
    memcpy(s_y, s_x, sizeof s_y);
    run(&s_c, mode, s_y, s_y, LEN, NULL, 0, LEN);    /* in place */
    double db = err_db(s_y, s_ref, LEN);
    printf("conv2 %4zu taps, %s blocks: err %.1f dB\n", taps, mode ? "odd" : "32", db);
    assert(db < -100);
}

/* IR <= 512 taps: the tail is off, the output is bit-identical to conv_t
 * with the same chunks (as cab.c runs it). */
static void test_bit_identical(size_t taps, int mode)
{
    static conv_t ref;
    static float h[CONV2_HEAD_PARTS][CONV_N], x[CONV2_HEAD_PARTS][CONV_N], y2[LEN];
    make_ir(s_ir, taps, 100.0f);
    assert(conv_init(&ref, h, x, CONV2_HEAD_PARTS) == 0);
    assert(conv_set_ir(&ref, s_ir, taps) == 0);
    assert(conv2_init(&s_c, &s_tail) == 0 && conv2_set_ir(&s_c, s_ir, taps) == 0);
    assert(conv2_init(&s_c2, NULL) == 0 && conv2_set_ir(&s_c2, s_ir, taps) == 0);
    assert(conv2_set_ir(&s_c2, s_ir, CONV2_HEAD_TAPS + 1) != 0);   /* no tail storage */
    for (size_t i = 0, j = 0; i < LEN; j++) {
        size_t m = chunk(mode, j);
        if (m > LEN - i) m = LEN - i;
        conv_process(&ref, s_x + i, y2 + i, m);
        conv2_process(&s_c, s_x + i, s_y + i, m);
        assert(memcmp(s_y + i, y2 + i, m * sizeof(float)) == 0);
        conv2_process(&s_c2, s_x + i, s_y + i, m);
        assert(memcmp(s_y + i, y2 + i, m * sizeof(float)) == 0);
        i += m;
    }
    printf("conv2 %4zu taps, %s blocks: bit-identical to conv_t\n", taps, mode ? "odd" : "32");
}

/* IR swap mid-stream = a FIR whose coefficients change at that sample. */
static void test_swap(size_t taps, size_t taps2, size_t swap, int mode)
{
    make_ir(s_ir, taps, 1500.0f);
    make_ir(s_ir2, taps2, 1000.0f);
    fir(s_x, LEN, s_ir, taps, s_ir2, taps2, swap, s_ref);
    assert(conv2_init(&s_c, &s_tail) == 0 && conv2_set_ir(&s_c, s_ir, taps) == 0);
    run(&s_c, mode, s_x, s_y, LEN, s_ir2, taps2, swap);
    double db = err_db(s_y, s_ref, LEN);
    printf("conv2 swap %4zu -> %4zu at %5zu (frame +%3zu), %s blocks: err %.1f dB\n",
           taps, taps2, swap, swap % CONV2_B, mode ? "odd" : "32", db);
    assert(db < -100);
}

/* Unit impulse in: the IR out from sample 0 (no latency, the tail joins
 * at tap 512 on time). Also: init = unit impulse, reset clears history. */
static void test_impulse(void)
{
    make_ir(s_ir, CONV2_MAX_TAPS, 1500.0f);
    assert(conv2_init(&s_c, &s_tail) == 0);
    for (size_t i = 0; i < 1000; i++) s_y[i] = s_x[i];
    conv2_process(&s_c, s_y, s_y, 1000);
    for (size_t i = 0; i < 1000; i++) assert(fabsf(s_y[i] - s_x[i]) < 1e-6f);   /* init: unit impulse */

    assert(conv2_set_ir(&s_c, s_ir, CONV2_MAX_TAPS) == 0);
    conv2_reset(&s_c);
    memset(s_y, 0, sizeof s_y);
    s_y[0] = 1.0f;
    run(&s_c, 0, s_y, s_y, 2 * CONV2_MAX_TAPS, NULL, 0, LEN);
    double e = 0, r = 0;
    for (size_t n = 0; n < 2 * CONV2_MAX_TAPS; n++) {
        double ref = n < CONV2_MAX_TAPS ? s_ir[n] : 0;
        e = fmax(e, fabs(s_y[n] - ref));
        r = fmax(r, fabs(ref));
    }
    printf("conv2 impulse: out[n] = ir[n] from n = 0, err %.1f dB\n", 20 * log10(e / r));
    assert(20 * log10(e / r) < -100);
    assert(fabsf(s_y[0] - s_ir[0]) < 1e-6f && fabsf(s_y[512] - s_ir[512]) < 1e-6f);
}

int main(void)
{
    for (size_t i = 0; i < LEN; i++) s_x[i] = frand();
    assert(conv2_init(&s_c, &s_tail) == 0);
    assert(conv2_set_ir(&s_c, s_ir, 0) != 0 && conv2_set_ir(&s_c, s_ir, CONV2_MAX_TAPS + 1) != 0);

    static const size_t taps[] = {513, 1024, 2048, 4096};
    for (int mode = 0; mode < 2; mode++)
        for (size_t i = 0; i < 4; i++) test_vs_fir(taps[i], mode);

    static const size_t short_taps[] = {1, 300, 512};
    for (int mode = 0; mode < 2; mode++)
        for (size_t i = 0; i < 3; i++) test_bit_identical(short_taps[i], mode);

    /* swap points: mid frame before and after slice 0, at a frame edge,
     * the last block of a frame; long -> long, short -> long (tail off -> on),
     * long -> short (on -> off) */
    test_swap(4096, 2048, 5000, 0);          /* frame +136: slices 0..3 done */
    test_swap(2048, 4096, 5 * CONV2_B, 0);   /* frame edge */
    test_swap(1024, 4096, 5 * CONV2_B + 16, 1);   /* before slice 0 */
    test_swap(300, 4096, 6000, 0);           /* tail off -> on */
    test_swap(4096, 300, 7 * CONV2_B + 224, 0);   /* on -> off, last block */
    test_swap(4096, 3000, 9 * CONV2_B + 235, 1);
    test_impulse();
    printf("conv2 host tests OK\n");
    return 0;
}
