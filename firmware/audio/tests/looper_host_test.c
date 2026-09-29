/* Host tests for the flash looper: the frame codec (src/dsp/loopcodec.c),
 * the audio side (src/dsp/looper.c) and the flash side
 * (src/loopstore/loopstore.c, lsio.c) on a simulated W25Q64
 * (tests/loopflash_sim.c). Built by tests/test_looper.py; every check is an
 * assert.
 *
 * The main loop is simulated in time: audio blocks come due every 32/44100
 * s; the loop runs engine_task (one block), ls_task and looper_poll; the
 * flash side's busy waits run the audio (flash_pump). A block that runs
 * more than 3 blocks late (the DAC ring's target fill, sai.h) counts as a
 * skip, as engine.c's latency_skips would. Prints the measured quality and
 * the flash timing. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsp/looper.h"
#include "dsp/loopcodec.h"
#include "loopstore/loopstore.h"
#include "loopstore/lsio.h"
#include "loopflash_sim.h"

#define FS 44100.0
#define TWO_PI_ (2 * 3.14159265358979)
#define SEC 44096u                     /* ~1 s in whole blocks */
#define BLOCK_US (1e6 * DSP_BLOCK / FS)

static looper_t lp;
static loopstore_t *ls;
static loopio_t *io;

/* ------------------------------------------------------------ the main loop */

static const float *g_x;
static float *g_y;
static size_t g_n, g_i;
static uint64_t g_blk, g_t0;
static unsigned skips;
static uint64_t max_late, max_stall;
static int in_task;

static uint64_t due(uint64_t blk) { return g_t0 + (uint64_t)((double)(blk + 1u) * BLOCK_US); }

static void block(void)
{
    float l[DSP_BLOCK], r[DSP_BLOCK];
    for (unsigned i = 0; i < DSP_BLOCK; i++) l[i] = r[i] = g_x ? g_x[g_i + i] : 0.0f;
    looper_process(&lp, l, r, DSP_BLOCK);
    for (unsigned i = 0; i < DSP_BLOCK; i++) {
        float in = g_x ? g_x[g_i + i] : 0.0f;
        assert(l[i] == r[i]);
        assert(isfinite(l[i]) && fabsf(l[i] - in) < 4.0f);   /* never garbage */
        if (g_y) g_y[g_i + i] = l[i] - in;
    }
    g_i += DSP_BLOCK;
    g_blk++;
}

static int engine_task(void)
{
    if (g_i >= g_n || sim_us < due(g_blk)) return 0;
    uint64_t late = sim_us - due(g_blk);
    if (late > max_late) max_late = late;
    if (late > 3.0 * BLOCK_US) {
        skips++;
        if (getenv("LHT_DEBUG")) printf("skip at %.3f ms: late %.3f ms, in_task %d, pages %u slices %u reads %u\n", sim_us / 1000.0, late / 1000.0, in_task, g_lsio.pages, g_lsio.slices, g_lsio.reads);
    }
    block();
    sim_us += 15;                         /* the engine's own time */
    return 1;
}

static void pump(void) { (void)engine_task(); }

/* n samples (whole blocks) of x through the looper in real time; y = what
 * the looper added. x may be NULL (silence), y too. */
static void run(const float *x, float *y, size_t n)
{
    assert(n % DSP_BLOCK == 0);
    g_x = x;
    g_y = y;
    g_n = n;
    g_i = 0;
    /* the audio of this run starts now: the flash side may have run on
     * after the last run's blocks (no audio to pump there) */
    if (sim_us > due(g_blk)) g_t0 += sim_us - due(g_blk);
    while (g_i < n) {
        sim_us += 40;                     /* the rest of the main loop */
        (void)engine_task();
        uint64_t t = sim_us;
        in_task = 1;
        ls_task(ls);
        in_task = 0;
        if (sim_us - t > max_stall) max_stall = sim_us - t;
        looper_poll(&lp);
        /* sleep until the next block, unless the flash side has work
         * (main.c loop_work_pending) */
        if (g_i < n && sim_us < due(g_blk) && !ls_busy(ls)) sim_us = due(g_blk);
    }
    g_x = NULL;
    g_y = NULL;
}

static size_t B(size_t n) { return (n + DSP_BLOCK - 1) / DSP_BLOCK * DSP_BLOCK; }

static void fresh(const sim_timing_t *t)
{
    sim_flash_init(t);
    sim_pump = pump;
    sim_loop_setup(&lp);
    ls = sim_loop_store();
    io = sim_loop_io();
    g_t0 = sim_us;
    g_blk = 0;
    skips = 0;
    max_late = max_stall = 0;
    memset(&g_lsio, 0, sizeof g_lsio);
    assert(lp.state == LOOPER_EMPTY);
}

/* the erase ahead until a record may start (or n chunks are ready) */
static void prep_n(uint32_t n)
{
    for (int i = 0; i < 200000 && io->pool < n; i++) run(NULL, NULL, 1024);
    assert(io->pool >= n);
}

static void prep(void) { prep_n(LOOPER_READY_CHUNKS); }

static void idle_flash(void)   /* the flash side done with every write */
{
    for (int i = 0; i < 400 && !ls_writer_idle(ls); i++) run(NULL, NULL, 1024);
    assert(ls_writer_idle(ls));
}

static float *sine(size_t n, double f, double a, double ph)
{
    float *x = malloc(n * sizeof *x);
    for (size_t i = 0; i < n; i++) x[i] = (float)(a * sin(TWO_PI_ * f * (double)i / FS + ph));
    return x;
}

/* least squares: y ~ sum_h (a_h cos + b_h sin)(h f) over [0, n): the
 * amplitude of each harmonic 1..H, the residual rms */
#define HMAX 5
static void fit(const float *y, size_t n, double f, int H, double amp[HMAX + 1], double *res)
{
    double *e = malloc(n * sizeof *e);
    for (size_t i = 0; i < n; i++) e[i] = y[i];
    double A[HMAX + 1] = {0}, Bc[HMAX + 1] = {0};
    for (int pass = 0; pass < 3; pass++) {
        for (int h = 1; h <= H; h++) {
            double sa = 0, sb = 0, cc = 0, ss = 0;
            for (size_t i = 0; i < n; i++) {
                double w = TWO_PI_ * f * h * (double)i / FS, c = cos(w), s = sin(w);
                sa += e[i] * c; sb += e[i] * s; cc += c * c; ss += s * s;
            }
            double da = sa / cc, db = sb / ss;
            A[h] += da; Bc[h] += db;
            for (size_t i = 0; i < n; i++) {
                double w = TWO_PI_ * f * h * (double)i / FS;
                e[i] -= da * cos(w) + db * sin(w);
            }
        }
    }
    double r = 0;
    for (size_t i = 0; i < n; i++) r += e[i] * e[i];
    *res = sqrt(r / (double)n);
    for (int h = 1; h <= H; h++) amp[h] = hypot(A[h], Bc[h]);
    free(e);
}

static double amp_at(const float *y, size_t a, size_t n, double f)
{
    double amp[HMAX + 1], res;
    fit(y + a, n, f, 1, amp, &res);
    return amp[1];
}

/* ------------------------------------------------------------ codec */

static void test_codec(void)
{
    static const double freqs[] = {100.0, 300.0, 1000.0, 2000.0, 3000.0, 5000.0};
    double worst = 1e9;
    for (unsigned k = 0; k < sizeof freqs / sizeof freqs[0]; k++) {
        for (double lvl = 0.9; lvl > 1e-4; lvl /= 10.0) {
            double s2 = 0, n2 = 0;
            float x[LC_N], y[LC_N];
            uint8_t f[LC_BYTES];
            for (int b = 0; b < 22050 / LC_N; b++) {
                for (int i = 0; i < LC_N; i++)
                    x[i] = (float)(lvl * sin(TWO_PI_ * freqs[k] * (b * LC_N + i) / 22050.0 + 0.3));
                lc_encode(x, f);
                assert(f[0] != 0xFFu);
                lc_decode(f, y);
                for (int i = 0; i < LC_N; i++) {
                    s2 += (double)x[i] * x[i];
                    n2 += ((double)y[i] - x[i]) * ((double)y[i] - x[i]);
                }
            }
            double snr = 10 * log10(s2 / n2);
            if (snr < worst) worst = snr;
            if (lvl > 0.5 || lvl < 2e-3)
                printf("codec %6.1f Hz %5.1f dBFS: SNR %.1f dB\n", freqs[k], 20 * log10(lvl), snr);
            assert(snr > 58.0);
        }
    }
    printf("codec: SNR >= %.1f dB (100 Hz..5 kHz, 0..-80 dBFS)\n", worst);
    /* silence, erased flash, NaN, huge values */
    float x[LC_N] = {0}, y[LC_N];
    uint8_t f[LC_BYTES];
    lc_encode(x, f);
    for (int i = 0; i < LC_BYTES; i++) assert(f[i] == 0);
    memset(f, 0xFF, sizeof f);
    lc_decode(f, y);
    for (int i = 0; i < LC_N; i++) assert(y[i] == 0.0f);
    x[3] = NAN;
    lc_encode(x, f);
    lc_decode(f, y);
    for (int i = 0; i < LC_N; i++) assert(y[i] == 0.0f);
    for (int i = 0; i < LC_N; i++) x[i] = (i & 1) ? 1e6f : -3e5f;
    lc_encode(x, f);
    lc_decode(f, y);
    for (int i = 0; i < LC_N; i++) assert(fabsf(y[i] - x[i]) <= 1e6f / 511.0f);
    x[0] = 1.0f;
    for (int i = 1; i < LC_N; i++) x[i] = 1e-9f;   /* tiny next to full scale */
    lc_encode(x, f);
    lc_decode(f, y);
    assert(fabsf(y[0] - 1.0f) < 2e-3f);
    printf("codec: round trip OK\n");
}

static void report_flash(const char *what);

/* ------------------------------------------------------------ quality */

/* Record a sine (1.5 s), close, play the next pass with no input; fit the
 * loop output away from the wrap: SNR (all but the fundamental) and THD. */
static void quality(double f, double a, double *snr, double *thd)
{
    fresh(&SIM_TYPICAL);
    prep();
    const size_t R = 66144, P = R + SEC;
    float *x = sine(R, f, a, 0.3), *y = malloc(P * sizeof *y);
    assert(looper_cmd(&lp, LOOPER_TAP) == 0);
    run(x, NULL, R);
    assert(looper_cmd(&lp, LOOPER_TAP) == 0 && lp.state == LOOPER_REC);   /* closes at once */
    run(NULL, y, P);
    assert(lp.state == LOOPER_PLAY && lp.passes >= 1);
    size_t len = (size_t)lp.len * 2u, off = len + 13230, n = 22050;
    assert(off + n <= P);
    double amp[HMAX + 1], res;
    fit(y + off, n, f, HMAX, amp, &res);
    double h = 0;
    for (int k = 2; k <= HMAX; k++) h += amp[k] * amp[k];
    double nd = sqrt(res * res + h / 2);
    *snr = 20 * log10(amp[1] / sqrt(2) / nd);
    *thd = 10 * log10(h / (amp[1] * amp[1]) + 1e-30);
    assert(fabs(amp[1] / a - 1.0) < 0.01);       /* unity gain at level 100 */
    if (sim_errors || skips || io->rd_under || io->wr_over) report_flash("quality");
    assert(sim_errors == 0 && skips == 0 && io->rd_under == 0 && io->wr_over == 0);
    free(x);
    free(y);
}

static void test_quality(void)
{
    static const double freqs[] = {100.0, 440.0, 1000.0, 2000.0, 5000.0};
    double worst = 1e9;
    for (unsigned k = 0; k < sizeof freqs / sizeof freqs[0]; k++) {
        for (double a = 0.5; a > 0.004; a /= 10.0) {
            double snr, thd;
            quality(freqs[k], a, &snr, &thd);
            printf("looper %6.1f Hz %5.1f dBFS: SNR %.1f dB, THD %.1f dB\n", freqs[k],
                   20 * log10(a), snr, thd);
            if (snr < worst) worst = snr;
            assert(snr > 55.0 && thd < -60.0);
        }
    }
    printf("looper: SNR >= %.1f dB (record, flash, play)\n", worst);
}

/* ------------------------------------------------------------ timing */

/* A click recorded at a known time repeats every len samples exactly, over
 * many passes: sample-accurate loop points, no drift. */
static void test_loop_points(void)
{
    fresh(&SIM_TYPICAL);
    prep();
    const size_t R = 30016, P = 10 * 32000;
    float *x = calloc(R, sizeof *x), *y = malloc(P * sizeof *y);
    for (int i = -20; i <= 20; i++)
        x[1000 + i] = (float)(0.5 * (1 + cos(3.14159265358979 * i / 21.0)) * 0.5);
    assert(looper_cmd(&lp, LOOPER_REC_A) == 0);
    run(x, NULL, R);
    assert(looper_cmd(&lp, LOOPER_REC_A) == 0);   /* close */
    run(NULL, y, P);
    size_t len = (size_t)lp.len * 2u;
    assert(len == R + 2u);                         /* the tap's block + one sample */
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
    printf("loop points: len %zu samples, %d passes, period exact\n", len, peaks);
    free(x);
    free(y);
}

static double d2max(const float *y, size_t a, size_t b)
{
    double m = 0;
    for (size_t i = a + 2; i < b; i++) {
        double d = fabs((double)y[i] - 2.0 * y[i - 1] + y[i - 2]);
        if (d > m) m = d;
    }
    return m;
}

#define CLICK 0.02
static void test_no_click(void)
{
    const double f = 110.0, a = 0.3;
    fresh(&SIM_TYPICAL);
    prep();
    const size_t R = 40000, T = 576, P = 4 * R;
    float *x = sine(R + P, f, a, 0.0), *y = malloc(P * sizeof *y);
    assert(looper_cmd(&lp, LOOPER_REC_A) == 0);
    run(x, NULL, R);
    assert(looper_cmd(&lp, LOOPER_PLAY_A) == 0);
    run(x + R, NULL, T);
    run(NULL, y, P);
    size_t len = (size_t)lp.len * 2u;
    double step = fabs(a * sin(TWO_PI_ * f * (double)len / FS) - a * sin(0.0));
    double mid = d2max(y, len - T + len / 4, len - T + 3 * len / 4), wrap = 0;
    for (size_t k = 1; k <= 3; k++) {
        double w = d2max(y, k * len - T - 400, k * len - T + 400);
        if (w > wrap) wrap = w;
    }
    printf("wrap: max |d2| at the wraps %.2e, mid-loop %.2e (a hard splice steps %.3f)\n", wrap,
           mid, step);
    assert(step > 0.1);
    assert(wrap < 2.0 * mid && wrap < CLICK);
    assert(looper_cmd(&lp, LOOPER_STOP_A) == 0);
    run(NULL, y, 4096);
    assert(lp.state == LOOPER_STOP);
    assert(looper_cmd(&lp, LOOPER_PLAY_A) == 0);
    run(NULL, y + 4096, 4096);
    double m = d2max(y, 0, 8192);
    printf("stop/play: max |d2| %.2e\n", m);
    assert(m < CLICK);
    float *dc = malloc(8192 * sizeof *dc);
    for (int i = 0; i < 8192; i++) dc[i] = 0.2f;
    assert(looper_cmd(&lp, LOOPER_DUB_A) == 0);
    run(dc, NULL, 2048);
    assert(looper_cmd(&lp, LOOPER_PLAY_A) == 0);
    run(dc, NULL, 2048);
    run(NULL, y, P);
    m = d2max(y, 0, P);
    printf("punch in/out: max |d2| %.2e\n", m);
    assert(m < CLICK);
    assert(sim_errors == 0 && skips == 0 && io->rd_under == 0);
    free(dc);
    free(x);
    free(y);
}

/* ------------------------------------------------------------ states */

static void test_states(void)
{
    const double fa = 220.0, fb = 330.0, a = 0.2;
    fresh(&SIM_TYPICAL);
    assert(looper_cmd(&lp, LOOPER_PLAY_A) == -1);    /* nothing to play */
    assert(looper_cmd(&lp, LOOPER_UNDO_A) == -1);
    prep();
    const size_t R = SEC, N = 3 * R;
    float *xa = sine(N, fa, a, 0), *xb = sine(N, fb, a, 1), *y = malloc(N * sizeof *y);
    assert(looper_cmd(&lp, LOOPER_TAP) == 0 && lp.state == LOOPER_REC);
    run(xa, NULL, R);
    assert(looper_cmd(&lp, LOOPER_TAP) == 0);
    run(NULL, y, 2 * R);
    assert(lp.state == LOOPER_PLAY);
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
    assert(fabs(ga / a - LOOPER_FB) < 0.02 && fabs(gb - a) < 0.015);
    looper_info(&lp, &in);
    assert(in.undo == 1);
    /* undo: A alone (the whole dub, both passes), redo: both */
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
    /* rec -> dub at once (console `loop dub` while recording) */
    prep();
    assert(looper_cmd(&lp, LOOPER_REC_A) == 0);
    run(xa, NULL, R);
    assert(looper_cmd(&lp, LOOPER_DUB_A) == 0);
    run(xb, NULL, 4096);
    assert(lp.state == LOOPER_DUB && lp.alt);
    /* clear while recording: at once */
    assert(looper_cmd(&lp, LOOPER_CLEAR_A) == 0);
    run(NULL, NULL, 4096);
    prep();
    assert(looper_cmd(&lp, LOOPER_REC_A) == 0);
    assert(looper_cmd(&lp, LOOPER_CLEAR_A) == 0 && lp.state == LOOPER_EMPTY);
    /* a stop while recording closes the loop, stopped */
    prep();
    assert(looper_cmd(&lp, LOOPER_REC_A) == 0);
    run(xa, NULL, R);
    assert(looper_cmd(&lp, LOOPER_STOP_A) == 0);
    run(NULL, y, 8192);
    assert(lp.state == LOOPER_STOP && lp.len > 0);
    assert(sim_errors == 0 && skips == 0);
    printf("states: rec play dub undo redo stop clear OK\n");
    free(xa);
    free(xb);
    free(y);
}

/* ------------------------------------------------------------ flash */

/* a test signal with no period near the loop length */
static float *music(size_t n, unsigned seed)
{
    float *x = malloc(n * sizeof *x);
    double f[3] = {97.0 + seed, 441.0 + 3 * seed, 1733.0};
    for (size_t i = 0; i < n; i++) {
        double t = (double)i / FS;
        x[i] = (float)(0.2 * sin(TWO_PI_ * f[0] * t) + 0.1 * sin(TWO_PI_ * f[1] * t) *
                       (0.5 + 0.5 * sin(TWO_PI_ * 0.7 * t)) + 0.05 * sin(TWO_PI_ * f[2] * t));
    }
    return x;
}

/* the looper's output over one pass, by loop position (22.05 kHz frames
 * interpolated: take the even samples) */
static void capture_pass(float *out)
{
    size_t len = lp.len, n = B(2 * len + 4096);
    float *y = malloc(n * sizeof *y);
    uint32_t p0 = lp.pos;
    run(NULL, y, n);
    for (size_t i = 0; i < len; i++) out[(p0 + i) % len] = y[2 * i + 2048];   /* past the fades */
    (void)p0;
    free(y);
}

static void report_flash(const char *what)
{
    ls_info_t li;
    ls_info(ls, &li);
    printf("%s: pages %u (verify errors %u, slow %u, max %u ms), erases %u (%u ms busy, max %u ms), "
           "slices %u, suspends %u, stall max %.1f ms, late max %.2f ms, skips %u, underruns %u, "
           "overruns %u, drops %u, cut %u, sim errors %u\n",
           what, g_lsio.pages, g_lsio.verify_errs, g_lsio.prog_slow, g_lsio.prog_max_ms,
           g_lsio.erases, g_lsio.erase_ms, g_lsio.erase_max_ms, g_lsio.slices, g_lsio.suspends,
           (double)max_stall / 1000.0, (double)max_late / 1000.0, skips, io->rd_under,
           io->wr_over, li.drops, lp.cut, sim_errors);
}

/* Record, dub, undo: the loop after undo is the loop before the dub, bit
 * for bit (the same frames); redo brings the dub back. Under a timing. */
static void streaming(const char *name, const sim_timing_t *t, double rec_s, int strict)
{
    fresh(t);
    uint64_t t0 = sim_us;
    prep_n(ls_chunks(ls));                       /* looper mode on: the whole area */
    printf("%s: the area erased in %.1f s\n", name, (double)(sim_us - t0) / 1e6);
    size_t R = B((size_t)(rec_s * FS)), D = B(R / 2);
    float *x = music(R, 1), *xd = music(D, 7);
    assert(looper_cmd(&lp, LOOPER_REC_A) == 0);
    run(x, NULL, R);
    assert(looper_cmd(&lp, LOOPER_PLAY_A) == 0);
    run(NULL, NULL, 8192);
    assert(lp.state == LOOPER_PLAY && lp.cut == 0);
    idle_flash();
    float *p0 = malloc(lp.len * sizeof *p0), *p1 = malloc(lp.len * sizeof *p1);
    capture_pass(p0);
    assert(looper_cmd(&lp, LOOPER_DUB_A) == 0);
    run(xd, NULL, D);
    assert(looper_cmd(&lp, LOOPER_PLAY_A) == 0);
    run(NULL, NULL, 8192);
    capture_pass(p1);
    double diff = 0;
    for (size_t i = 0; i < lp.len; i++) diff = fmax(diff, fabs((double)p1[i] - p0[i]));
    assert(diff > 0.05);                         /* the dub is there */
    assert(looper_cmd(&lp, LOOPER_UNDO_A) == 0);
    run(NULL, NULL, 8192);
    capture_pass(p1);
    diff = 0;
    for (size_t i = 0; i < lp.len; i++) diff = fmax(diff, fabs((double)p1[i] - p0[i]));
    report_flash(name);
    printf("%s: %.1f s loop, undo exact (max diff %.1e)\n", name, lp.len / 22050.0, diff);
    assert(diff < 1e-6);
    assert(sim_errors == 0 && skips == 0);
    if (strict) assert(io->rd_under == 0 && io->wr_over == 0 && lp.cut == 0);
    free(p0);
    free(p1);
    free(x);
    free(xd);
}

static void test_streaming(void)
{
    streaming("typical flash", &SIM_TYPICAL, 20.0, 1);
    assert(max_stall < 5000);                    /* the UI waits at most a few ms */
    streaming("worst-case flash", &SIM_WORST, 20.0, 1);
    assert(max_stall < 12000);
    sim_timing_t ns = SIM_TYPICAL;
    ns.suspend = 0;
    streaming("no erase suspend", &ns, 10.0, 0);   /* works, the UI waits for erases */
}

/* The erase falls behind (a chip slower than the 24 kB/s record): the
 * record closes by itself at a chunk end, with the crossfade, and plays. */
static void test_margin(void)
{
    sim_timing_t slow = SIM_WORST;
    slow.block_us = 3500000;                     /* 18 kB/s */
    slow.sector_us = 400000;
    fresh(&slow);
    prep();
    size_t R = B(60 * 44100);
    float *x = music(R, 3), *y = malloc(B(8 * SEC) * sizeof *y);
    assert(looper_cmd(&lp, LOOPER_REC_A) == 0);
    run(x, NULL, R);
    assert(lp.state == LOOPER_PLAY && lp.cut >= 1);
    looper_info_t in;
    looper_info(&lp, &in);
    run(NULL, y, B(8 * SEC));
    double rms = 0;
    for (size_t i = 0; i < B(8 * SEC); i++) rms += (double)y[i] * y[i];
    rms = sqrt(rms / (double)B(8 * SEC));
    report_flash("slow erase");
    printf("slow erase: the record closed by itself at %u ms (of %u), plays at rms %.3f\n",
           in.len_ms, in.max_ms, rms);
    assert(in.len_ms > 1000 && in.len_ms < in.max_ms && rms > 0.05);
    assert(sim_errors == 0 && skips == 0 && io->rd_under == 0);
    free(x);
    free(y);
}

/* The whole area: a record fills it and closes by itself; a dub over all
 * of it runs out of room and fades out at a chunk end; undo is exact. A
 * loop of undo_max dubs whole. */
static void test_full_area(void)
{
    fresh(&SIM_TYPICAL);
    looper_info_t in;
    looper_info(&lp, &in);
    printf("area: %u chunks of %u frames, max %u ms, dub whole up to %u ms\n",
           (unsigned)ls_chunks(ls), (unsigned)LS_FPC, in.max_ms, in.undo_max_ms);
    assert(ls_chunks(ls) == LS_MAX_CHUNKS && in.max_ms > 108000 && in.undo_max_ms > 54000);
    prep();
    size_t R = B(112 * 44100);
    float *x = music(R, 5);
    assert(looper_cmd(&lp, LOOPER_REC_A) == 0);
    run(x, NULL, R);
    assert(lp.state == LOOPER_PLAY);
    looper_info(&lp, &in);
    printf("area: the record closed by itself at %u ms\n", in.len_ms);
    assert(in.len_ms + 400 >= in.max_ms && in.len_ms <= in.max_ms);
    idle_flash();
    float *p0 = malloc(lp.len * sizeof *p0), *p1 = malloc(lp.len * sizeof *p1);
    capture_pass(p0);
    assert(looper_cmd(&lp, LOOPER_DUB_A) == 0);
    run(x, NULL, B(20 * 44100));                  /* the free flash lasts ~1 chunk */
    assert(lp.state == LOOPER_PLAY && lp.cut >= 1);
    assert(looper_cmd(&lp, LOOPER_UNDO_A) == 0);
    run(NULL, NULL, 8192);
    capture_pass(p1);
    double diff = 0;
    for (size_t i = 0; i < lp.len; i++) diff = fmax(diff, fabs((double)p1[i] - p0[i]));
    printf("area: a dub over the full loop ends when the flash is full; undo exact (%.1e)\n", diff);
    assert(diff < 1e-6);
    free(p0);
    free(p1);
    /* a loop of undo_max: the dub runs whole, two passes */
    assert(looper_cmd(&lp, LOOPER_CLEAR_A) == 0);
    run(NULL, NULL, 8192);
    assert(lp.state == LOOPER_EMPTY);
    prep();
    size_t L = B((size_t)((in.undo_max_ms - 300) * 44.1));
    uint32_t cut0 = lp.cut;
    assert(looper_cmd(&lp, LOOPER_REC_A) == 0);
    run(x, NULL, L);
    assert(looper_cmd(&lp, LOOPER_DUB_A) == 0);   /* closes, then dubs */
    run(x, NULL, B(L + L / 4));
    assert(looper_cmd(&lp, LOOPER_PLAY_A) == 0);
    run(NULL, NULL, 8192);
    looper_info(&lp, &in);
    report_flash("area");
    printf("area: a %u ms loop dubs whole (cut %u)\n", in.len_ms, lp.cut - cut0);
    assert(lp.cut == cut0 && in.undo == 1);
    assert(sim_errors == 0 && skips == 0 && io->rd_under == 0 && io->wr_over == 0);
    free(x);
}

/* Nothing reads the area at boot, and a record right after boot waits for
 * the first erases (not ready), then works. A chip too small: no looper. */
static void test_boot(void)
{
    fresh(&SIM_TYPICAL);
    ls->armed = 0;
    run(NULL, NULL, 8192);
    assert(g_lsio.reads == 0 && g_lsio.erases == 0);   /* not armed: the flash is untouched */
    assert(looper_cmd(&lp, LOOPER_REC_A) == -3);       /* arms; not ready yet */
    prep();
    assert(looper_cmd(&lp, LOOPER_REC_A) == 0);
    assert(sim_errors == 0);
    sim_capacity = 0x400000u;                          /* 4 MB: no area */
    sim_loop_setup(&lp);
    assert(lp.state == LOOPER_OFF && looper_cmd(&lp, LOOPER_REC_A) == -2);
    looper_info_t in;
    looper_info(&lp, &in);
    assert(!in.flash && in.max_ms == 0);
    sim_capacity = 0x800000u;
    printf("boot: nothing read before written, not ready until erased, no area on 4 MB\n");
}

int main(void)
{
    test_codec();
    test_quality();
    test_loop_points();
    test_no_click();
    test_states();
    test_streaming();
    test_margin();
    test_full_area();
    test_boot();
    sim_flash_free();
    printf("looper host tests OK\n");
    return 0;
}
