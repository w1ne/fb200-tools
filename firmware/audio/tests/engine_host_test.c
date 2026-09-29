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
#define SECS 30

static int16_t ring[CAP * 2];
static int16_t outbuf[(SECS + 1) * 44100 * 2];

/* Worst 10 ms window of out (mono L) vs a fitted sine near f (free
 * amplitude, phase, DC, and a linear drift of amplitude and phase: a
 * frequency a few ppm off while the loop settles is no error), after `from`
 * frames: dB re the signal. A repeated or dropped frame is a step: it stays. */
#define NB 5
static double worst_window_db(const int16_t *o, size_t n, size_t from, double f)
{
    const size_t w = 441;
    double worst = -300;
    for (size_t s = from; s + w <= n; s += w) {
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

/* The host sends a sine at f (its clock: FS * (1 + ppm)) in 1 ms packets;
 * the engine pulls 32-frame blocks at FS. Returns the output frames. */
static size_t simulate(double ppm, double f, uint32_t *ins, uint32_t *drops, uint32_t *fmin,
                       uint32_t *fmax, float *ratio)
{
    drift_rs_t rs;
    drift_rs_init(&rs, (float)FS);
    uint32_t head = 0, tail = 0;
    double host_fs = FS * (1.0 + ppm * 1e-6), sent = 0;
    uint64_t hk = 0;                                   /* host frames sent */
    size_t nout = 0;
    *ins = *drops = 0;
    *fmin = CAP; *fmax = 0;
    const double blk = 32.0 / FS;
    double next_pkt = 0.0;
    for (double t = 0; t < SECS; t += blk) {
        while (next_pkt <= t) {                        /* the packets due by now */
            sent += host_fs * 0.001;
            while ((double)hk < sent) {
                int16_t v = (int16_t)lrint(9830.0 * sin(2 * M_PI * f * (double)hk / host_fs));
                ring[head * 2] = ring[head * 2 + 1] = v;
                head = (head + 1u) % CAP;
                hk++;
            }
            next_pkt += 0.001;
        }
        drift_rs_pull(&rs, ring, CAP, &tail, head, &outbuf[nout * 2], 32, MAXF, ins, drops);
        nout += 32;
        uint32_t fill = (head + CAP - tail) % CAP;
        if (t > 8.0) {                                 /* settled */
            if (fill < *fmin) *fmin = fill;
            if (fill > *fmax) *fmax = fill;
        }
    }
    *ratio = rs.ratio;
    return nout;
}

int main(void)
{
    const double ppms[] = {-500, -100, -8, 0, 8, 100, 500};
    for (unsigned k = 0; k < sizeof ppms / sizeof ppms[0]; k++) {
        uint32_t ins, drops, fmin, fmax;
        float ratio;
        size_t n = simulate(ppms[k], 1000.0, &ins, &drops, &fmin, &fmax, &ratio);
        /* the output runs at the codec clock: the host's 1 kHz, as played */
        double worst = worst_window_db(outbuf, n, (size_t)(3 * FS), 1000.0);
        printf("drift_rs %+5.0f ppm: inserts %u drops %u, fill %u..%u (target %u), ratio %+.1f ppm, "
               "worst 10 ms window %.1f dB\n", ppms[k], ins, drops, fmin, fmax, DRIFT_RS_TARGET,
               (ratio - 1.0f) * 1e6f, worst);
        assert(ins == 0 && drops == 0);
        assert(fmin > 16 && fmax < DRIFT_RS_TARGET + 64);
        assert(fabs((ratio - 1.0) * 1e6 - ppms[k]) < 20.0);
        assert(worst < -75.0);
    }
    {   /* a high tone: the interpolation error */
        uint32_t ins, drops, fmin, fmax;
        float ratio;
        size_t n = simulate(100, 8000.0, &ins, &drops, &fmin, &fmax, &ratio);
        double worst = worst_window_db(outbuf, n, (size_t)(3 * FS), 8000.0);
        printf("drift_rs 8 kHz +100 ppm: worst 10 ms window %.1f dB\n", worst);
        assert(ins == 0 && drops == 0 && worst < -40.0);
    }
    {   /* fallbacks: prefill silence, ring dry -> repeat, overfull -> restart */
        drift_rs_t rs;
        int16_t o[64];
        uint32_t head = 10, tail = 0, ins = 0, drops = 0;
        memset(ring, 0x11, sizeof ring);
        drift_rs_init(&rs, (float)FS);
        drift_rs_pull(&rs, ring, CAP, &tail, head, o, 32, MAXF, &ins, &drops);
        assert(tail == 0 && o[0] == 0 && o[63] == 0 && !rs.running);     /* below the target */
        head = 100;
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
