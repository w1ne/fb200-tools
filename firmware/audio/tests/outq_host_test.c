/* Host test of the 16-bit output stage (src/dsp/outq.h): truncation (v0.9.1)
 * vs rounding vs TPDF dither on a low-level sine. Run by tests/test_dsp_host.py
 * (test_outq_suite); every check is an assert. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "dsp/outq.h"

#define N 65536
#define FS 44100.0

static float x[N];
static int16_t y[N];

/* power of y at DFT bin k (exact bin: the sine is periodic in N) */
static double bin_power(const int16_t *s, unsigned k)
{
    double re = 0, im = 0;
    for (unsigned i = 0; i < N; i++) {
        double ph = 2.0 * M_PI * (double)k * (double)i / N;
        re += s[i] * cos(ph);
        im -= s[i] * sin(ph);
    }
    return re * re + im * im;
}

/* THD (harmonics 2..9) and THD+N (everything but the fundamental and DC, by
 * Parseval) in dB re the fundamental */
static void measure(const int16_t *s, unsigned k, double *thd, double *thdn, double *dc)
{
    double total = 0, mean = 0;
    for (unsigned i = 0; i < N; i++) mean += s[i];
    mean /= N;
    for (unsigned i = 0; i < N; i++) total += ((double)s[i] - mean) * ((double)s[i] - mean);
    double p1 = bin_power(s, k), ph = 0;
    for (unsigned h = 2; h <= 9; h++) ph += bin_power(s, k * h);
    double fund = 2.0 * p1 / N;                     /* Parseval: sum s^2 = 2|X_k|^2/N */
    *thd = 10.0 * log10(ph / p1 + 1e-30);
    *thdn = 10.0 * log10((total - fund) / fund + 1e-30);
    *dc = mean;
}

static void sine(double dbfs, unsigned k)
{
    double a = pow(10.0, dbfs / 20.0);
    for (unsigned i = 0; i < N; i++) x[i] = (float)(a * sin(2.0 * M_PI * k * (double)i / N + 0.3));
}

int main(void)
{
    const unsigned k = 1486;                         /* ~1000 Hz at 44.1 kHz */
    double thd_t, thdn_t, dc_t, thd_r, thdn_r, dc_r, thd_d, thdn_d, dc_d;
    outq_t q;

    for (int level = -60; level <= -40; level += 20) {
        sine(level, k);
        for (unsigned i = 0; i < N; i++) y[i] = (int16_t)(x[i] * 32767.0f);   /* v0.9.1 */
        measure(y, k, &thd_t, &thdn_t, &dc_t);
        for (unsigned i = 0; i < N; i++) y[i] = outq_round(x[i]);
        measure(y, k, &thd_r, &thdn_r, &dc_r);
        outq_init(&q, 1);
        q.dither = 1;
        for (unsigned i = 0; i < N; i++) y[i] = outq_sample(&q, x[i]);
        measure(y, k, &thd_d, &thdn_d, &dc_d);
        printf("outq %d dBFS 1 kHz: THD / THD+N  trunc %.1f / %.1f dB  round %.1f / %.1f dB  "
               "dither %.1f / %.1f dB\n", level, thd_t, thdn_t, thd_r, thdn_r, thd_d, thdn_d);
        assert(thd_r < thd_t - 10.0);                /* the dead zone is gone */
        assert(thdn_r < thdn_t - 1.0);                /* and its extra error power */
        assert(thd_d < -65.0);                       /* harmonics into the noise */
        assert(thdn_d < thdn_r + 6.0);               /* dither costs < 6 dB of THD+N */
    }

    /* full scale and beyond: clamped, symmetric */
    assert(outq_round(1.0f) == 32767 && outq_round(-1.0f) == -32767);
    assert(outq_round(2.0f) == 32767 && outq_round(-2.0f) == -32767);
    assert(outq_round(0.49f / 32767.0f) == 0 && outq_round(0.51f / 32767.0f) == 1);
    assert(outq_round(-0.51f / 32767.0f) == -1);
    outq_init(&q, 7);
    q.dither = 1;
    for (int i = 0; i < 100000; i++) {
        assert(outq_sample(&q, 0.0f) == 0);           /* digital silence stays silent */
        int16_t v = outq_sample(&q, 1.0f), w = outq_sample(&q, -1.0f);
        assert(v >= 32765 && w <= -32765);
        int16_t z = outq_sample(&q, 1e-9f);            /* dither: at most +-1 LSB */
        assert(z >= -1 && z <= 1);
    }
    printf("outq host tests OK\n");
    return 0;
}
