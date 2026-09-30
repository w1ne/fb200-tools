/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

/* Host test: the adaptive resampler for the host playback (drift_rs: no
 * frame repeated or dropped while the host clock is off by up to +-500 ppm,
 * no step in a sine, the fill near the target), the fallbacks, and the USB
 * playback -> chain input mix (`usb in|mix`). */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "audio/drift.h"

#define FS 44100.0
#define CAP 1024u
#define MAXF 480u
#define SECS 40

static int16_t ring[CAP * 2];
static int16_t outbuf[(SECS + 1) * 44100 * 2];

/* Worst 10 ms window of out (mono L) vs a fitted sine near f (free
 * amplitude, phase, DC, and a linear drift of amplitude and phase: a
 * frequency a few ppm off is no error, as in tools/sound_check.py where the
 * host's own 1 kHz comes back): dB re the signal. A step (a repeated or
 * dropped frame) stays in the residual. */
#define NB 5
static double worst_window_db(const int16_t *o, size_t from, size_t to, double f)
{
    const size_t w = 441;
    double worst = -300;
    for (size_t s = from; s + w <= to; s += w) {
        double m[NB][NB] = {{0}}, v[NB] = {0}, b[NB];
        for (size_t i = 0; i < w; i++) {
            double t = 2 * M_PI * f * (double)(s + i) / FS, u = (double)i / w - 0.5;
            b[0] = sin(t); b[1] = cos(t); b[2] = 1.0; b[3] = u * sin(t); b[4] = u * cos(t);
            for (int r = 0; r < NB; r++) {
                v[r] += b[r] * o[(s + i) * 2];
                for (int c = 0; c < NB; c++) m[r][c] += b[r] * b[c];
            }
        }
        for (int k = 0; k < NB; k++)                   /* Gauss (SPD: no pivoting) */
            for (int r = k + 1; r < NB; r++) {
                double q = m[r][k] / m[k][k];
                for (int c = k; c < NB; c++) m[r][c] -= q * m[k][c];
                v[r] -= q * v[k];
            }
        double x[NB];
        for (int r = NB - 1; r >= 0; r--) {
            x[r] = v[r];
            for (int c = r + 1; c < NB; c++) x[r] -= m[r][c] * x[c];
            x[r] /= m[r][r];
        }
        double e = 0, p = 0;
        for (size_t i = 0; i < w; i++) {
            double t = 2 * M_PI * f * (double)(s + i) / FS, u = (double)i / w - 0.5;
            double fit = x[0] * sin(t) + x[1] * cos(t) + x[2] + x[3] * u * sin(t) + x[4] * u * cos(t);
            e += (o[(s + i) * 2] - fit) * (o[(s + i) * 2] - fit);
            p += fit * fit;
        }
        double db = 10 * log10(e / p + 1e-30);
        if (db > worst) worst = db;
    }
    return worst;
}

/* THD+N as tools/sound_check.py `tone`: 65536 frames from s, Blackman
 * window, the power in 20 Hz..20 kHz outside f +-8 Hz re the power inside.
 * A phase modulation of the playback (a wobbling ratio) shows as sidebands. */
#define NF 65536
static double re_[NF], im_[NF];
static double thdn_db(const int16_t *o, size_t s, double f)
{
    for (size_t i = 0; i < NF; i++) {
        double w = 0.42 - 0.5 * cos(2 * M_PI * i / (NF - 1)) + 0.08 * cos(4 * M_PI * i / (NF - 1));
        re_[i] = w * o[(s + i) * 2];
        im_[i] = 0;
    }
    for (size_t i = 1, j = 0; i < NF; i++) {           /* bit reversal */
        size_t bit = NF >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { double t = re_[i]; re_[i] = re_[j]; re_[j] = t; }
    }
    for (size_t len = 2; len <= NF; len <<= 1) {       /* radix-2 FFT */
        double a = -2 * M_PI / (double)len;
        for (size_t i = 0; i < NF; i += len)
            for (size_t k = 0; k < len / 2; k++) {
                double wr = cos(a * k), wi = sin(a * k);
                size_t p = i + k, q = p + len / 2;
                double tr = re_[q] * wr - im_[q] * wi, ti = re_[q] * wi + im_[q] * wr;
                re_[q] = re_[p] - tr; im_[q] = im_[p] - ti;
                re_[p] += tr; im_[p] += ti;
            }
    }
    double in = 0, out = 0;
    for (size_t k = 1; k < NF / 2; k++) {
        double hz = k * FS / NF, pw = re_[k] * re_[k] + im_[k] * im_[k];
        if (hz < 20 || hz > 20000) continue;
        if (fabs(hz - f) < 8) in += pw; else out += pw;
    }
    return 10 * log10(out / in + 1e-30);
}

static uint32_t rng = 12345u;
static double urand(void)                              /* 0..1 */
{
    rng = rng * 1664525u + 1013904223u;
    return (double)(rng >> 8) / 16777216.0;
}

/* The host sends a sine at f (its clock: FS * (1 + ppm)) in 1 ms packets
 * that arrive +-0.3 ms late or early; the engine
 * pulls 32-frame blocks at FS, every 16th block late together with the
 * next (a main-loop stall). Returns the output frames; *start = the first
 * frame of sound. */
static size_t simulate(double ppm, double f, uint32_t *ins, uint32_t *drops, uint32_t *fmin,
                       uint32_t *fmax, float *ratio, size_t *start)
{
    drift_rs_t rs;
    drift_rs_init(&rs, (float)FS);
    uint32_t head = 0, tail = 0;
    double host_fs = FS * (1.0 + ppm * 1e-6), sent = 0;
    uint64_t hk = 0;                                   /* host frames sent */
    size_t nout = 0;
    *ins = *drops = 0;
    *fmin = CAP; *fmax = 0;
    *start = 0;
    const double blk = 32.0 / FS;
    unsigned pkt = 0, b = 0;
    double next_pkt = 0.05;
    for (double t = 0; t < SECS; t += blk, b++) {
        if (b % 16 == 7) continue;                     /* stalled: runs with the next */
        while (next_pkt <= t) {                        /* the packets arrived by now */
            sent += host_fs * 0.001;
            while ((double)hk < sent) {
                int16_t v = (int16_t)lrint(9830.0 * sin(2 * M_PI * f * (double)hk / host_fs));
                ring[head * 2] = ring[head * 2 + 1] = v;
                head = (head + 1u) % CAP;
                hk++;
            }
            pkt++;
            next_pkt = 0.05 + 0.001 * pkt + 0.0006 * (urand() - 0.5);
        }
        for (unsigned rep = b % 16 == 8 ? 2u : 1u; rep > 0; rep--) {
            drift_rs_pull(&rs, ring, CAP, &tail, head, &outbuf[nout * 2], 32, MAXF, ins, drops);
            nout += 32;
        }
        uint32_t fill = (head + CAP - tail) % CAP;
        if (!*start && rs.running) *start = nout;
        if (t > 20.0) {                                /* settled */
            if (fill < *fmin) *fmin = fill;
            if (fill > *fmax) *fmax = fill;
        }
    }
    *ratio = rs.ratio;
    return nout;
}

int main(void)
{
    const double ppms[] = {-500, -200, -100, -8, 0, 8, 100, 200, 500};
    for (unsigned k = 0; k < sizeof ppms / sizeof ppms[0]; k++) {
        uint32_t ins, drops, fmin, fmax;
        float ratio;
        size_t s0, n = simulate(ppms[k], 1000.0, &ins, &drops, &fmin, &fmax, &ratio, &s0);
        /* the output runs at the codec clock: the host's 1 kHz, as played.
         * As tools/sound_check.py: THD+N over 0.4..1.9 s after the start
         * (its `dry` window), the worst 10 ms window from 0.3 s on */
        double thdn = thdn_db(outbuf, s0 + (size_t)(0.4 * FS), 1000.0);
        double late = thdn_db(outbuf, n - NF, 1000.0);
        double worst = worst_window_db(outbuf, s0 + (size_t)(0.3 * FS), n, 1000.0);
        printf("drift_rs %+5.0f ppm: inserts %u drops %u, fill %u..%u (target %u), ratio %+.1f ppm, "
               "THD+N %.1f dB at the start, %.1f dB at the end, worst 10 ms window %.1f dB\n",
               ppms[k], ins, drops, fmin, fmax, DRIFT_RS_TARGET, (ratio - 1.0f) * 1e6f, thdn, late,
               worst);
        /* settled: at every rate; from the start: up to +-200 ppm (a Mac
         * is off by tens of ppm) */
        assert(drops == 0 && fmin > 16 && fmax < DRIFT_RS_TARGET + 64);
        assert(fabs((ratio - 1.0) * 1e6 - ppms[k]) < 20.0);
        assert(late < -80.0);
        if (fabs(ppms[k]) <= 200) assert(ins == 0 && worst < -80.0 && thdn < -70.0);
    }
    {   /* a high tone: the interpolation error */
        uint32_t ins, drops, fmin, fmax;
        float ratio;
        size_t s0, n = simulate(100, 8000.0, &ins, &drops, &fmin, &fmax, &ratio, &s0);
        double worst = worst_window_db(outbuf, s0 + (size_t)(0.3 * FS), n, 8000.0);
        printf("drift_rs 8 kHz +100 ppm: worst 10 ms window %.1f dB\n", worst);
        assert(ins == 0 && drops == 0 && worst < -50.0);
    }
    {   /* fallbacks: prefill silence, ring dry -> repeat, overfull -> restart */
        drift_rs_t rs;
        int16_t o[64];
        uint32_t head = 10, tail = 0, ins = 0, drops = 0;
        memset(ring, 0x11, sizeof ring);
        drift_rs_init(&rs, (float)FS);
        drift_rs_pull(&rs, ring, CAP, &tail, head, o, 32, MAXF, &ins, &drops);
        assert(tail == 0 && o[0] == 0 && o[63] == 0 && !rs.running);     /* below the target */
        head = DRIFT_RS_TARGET + 40;
        drift_rs_pull(&rs, ring, CAP, &tail, head, o, 32, MAXF, &ins, &drops);
        assert(rs.running && ins == 0 && tail > 0);
        tail = head;                                   /* dry */
        drift_rs_pull(&rs, ring, CAP, &tail, head, o, 32, MAXF, &ins, &drops);
        assert(ins > 25 && o[62] == o[60]);
        head = (tail + 700) % CAP;                     /* overfull */
        drift_rs_pull(&rs, ring, CAP, &tail, head, o, 32, MAXF, &ins, &drops);
        assert(drops == 700 - DRIFT_RS_TARGET);
    }

    /* USB playback into the chain input: mono mean of L/R. Replace drops
     * the instrument (R cleared, so the engine's L + R is the playback);
     * mix adds it to the instrument. */
    {
        int16_t play[3 * 2] = {16384, 16384, 16384, -16384, -32768, 0};
        float l[3] = {0.25f, 0.25f, 0.25f}, r[3] = {0.1f, 0.1f, 0.1f};
        drift_play_to_input(l, r, play, 3, 1);
        assert(l[0] == 0.5f && l[1] == 0.0f && l[2] == -0.5f);
        assert(r[0] == 0.0f && r[1] == 0.0f && r[2] == 0.0f);
        float ml[3] = {0.25f, 0.25f, 0.25f}, mr[3] = {0.1f, 0.1f, 0.1f};
        drift_play_to_input(ml, mr, play, 3, 0);
        assert(ml[0] == 0.75f && ml[1] == 0.25f && ml[2] == -0.25f);
        assert(mr[0] == 0.1f && mr[2] == 0.1f);
        /* host not playing: the engine passes zeros -> silence / instrument */
        int16_t zero[3 * 2] = {0};
        drift_play_to_input(l, r, zero, 3, 1);
        assert(l[0] == 0.0f && l[2] == 0.0f);
        drift_play_to_input(ml, mr, zero, 3, 0);
        assert(ml[0] == 0.75f);
    }

    printf("drift host tests OK\n");
    return 0;
}
