/* Host harness for dsp/drums.c and dsp/tuner.c.
 *
 *   drums_tuner_host_test selftest
 *   drums_tuner_host_test drums <bank.bin> <rhythm> <bpm> <n_samples> <out.f32> [block]
 *   drums_tuner_host_test tuner <in.f32> <a4_hz> [block]
 *
 * `drums` needs the generated stock rhythm data (FB200_STOCK_DRUMS); tests/
 * test_drums_tuner.py compares both modes against the emulated stock code. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsp/drums.h"
#include "dsp/tuner.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void *load(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *p = malloc((size_t)*size);
    if (fread(p, 1, (size_t)*size, f) != (size_t)*size) { perror(path); exit(2); }
    fclose(f);
    return p;
}

static drums_t dr;
static tuner_t tu;

static int selftest(void)
{
    /* no bank / no data: silent, no crash */
    CHECK(drums_init(&dr, NULL, NULL) == -1, "init without bank must fail");
    drums_start(&dr);
    float buf[37];
    for (int i = 0; i < 37; i++) buf[i] = 1.0f;
    drums_process(&dr, buf, 37);
    for (int i = 0; i < 37; i++) CHECK(buf[i] == 1.0f, "silent drums must not touch the mix");
    drums_set_tempo(&dr, 10);
    CHECK(dr.bpm == DRUMS_BPM_MIN, "tempo clamp low %u", dr.bpm);
    drums_set_tempo(&dr, 999);
    CHECK(dr.bpm == DRUMS_BPM_MAX, "tempo clamp high %u", dr.bpm);
    dr.last_tap_ms = 0;
    drums_tap(&dr, 1000);
    drums_tap(&dr, 1500);
    CHECK(dr.bpm == 120, "tap 500 ms -> 120 bpm, got %u", dr.bpm);
    drums_tap(&dr, 9000);                        /* > 3 s: restart, keep tempo */
    CHECK(dr.bpm == 120, "slow tap must not change tempo, got %u", dr.bpm);

    /* fake 2-entry bank: header truth, page-aligned entries */
    static uint32_t bank[1024];
    memset(bank, 0, sizeof bank);
    bank[0] = 2; bank[1] = 0;
    bank[2] = 1; bank[3] = 16;                   /* page 1: 4 floats */
    bank[4] = 2; bank[5] = 8;                    /* page 2: 2 floats */
    float *e0 = (float *)((uint8_t *)bank + 512), *e1 = (float *)((uint8_t *)bank + 1024);
    e0[0] = 0.5f; e1[0] = 0.25f;
    CHECK(drums_init(&dr, bank, NULL) != 0 || dr.data != NULL, "init result");
    CHECK(dr.n_samples == 2 && dr.smp[1] == e1 && dr.smp_len[0] == 4, "bank header parse");

    /* tuner: 110 Hz sine -> A (12), in tune; silence -> silent */
    tuner_init(&tu, 440);
    tuner_result_t r = {0};
    int got = 0;
    for (int i = 0; i < 44100; i++) {
        float x = 0.3f * __builtin_sinf(2.0f * 3.14159265f * 110.0f * (float)i / 44100.0f);
        tuner_feed(&tu, &x, 1);
        if (tuner_poll(&tu, &r)) got++;
    }
    CHECK(got >= 9, "analyses per second %d", got);
    CHECK(r.valid && r.note == 12 && r.deviation == 50, "110 Hz: valid %d note %d dev %d f %f",
          r.valid, r.note, r.deviation, (double)r.freq);
    CHECK(r.freq > 109.9f && r.freq < 110.1f && r.cents > -1.0f && r.cents < 1.0f,
          "110 Hz: f %f cents %f", (double)r.freq, (double)r.cents);
    for (int i = 0; i < 44100; i++) {
        float x = 0.0f;
        tuner_feed(&tu, &x, 1);
        tuner_poll(&tu, &r);
    }
    CHECK(r.silent && r.note == 0, "silence: silent %d note %d", r.silent, r.note);
    printf(fails ? "drums tuner host tests FAILED\n" : "drums tuner host tests OK\n");
    return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "selftest")) return selftest();
    if (argc >= 7 && !strcmp(argv[1], "drums")) {
        long sz;
        void *bank = load(argv[2], &sz);
        if (drums_init(&dr, bank, NULL)) { fprintf(stderr, "drums_init failed\n"); return 2; }
        drums_set_rhythm(&dr, (unsigned)atoi(argv[3]));
        drums_set_tempo(&dr, (unsigned)atoi(argv[4]));
        drums_start(&dr);
        long n = atol(argv[5]);
        size_t blk = argc >= 8 ? (size_t)atoi(argv[7]) : 32;
        float *out = calloc((size_t)n + blk, sizeof(float));
        for (long i = 0; i < n; i += (long)blk) drums_render(&dr, out + i, blk);
        FILE *f = fopen(argv[6], "wb");
        fwrite(out, sizeof(float), (size_t)n, f);
        fclose(f);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "yin")) {          /* one analysis of a 1024-sample buffer */
        long sz;
        float *x = load(argv[2], &sz), conf;
        tuner_init(&tu, 440);
        printf("%.6f %.6f\n", (double)tuner_yin(&tu, x, TUNER_FS, &conf), (double)conf);
        if (argc >= 4) {                                /* dump acf | d | dn for debugging */
            FILE *f = fopen(argv[3], "wb");
            fwrite(tu.acf, 4, TUNER_WIN, f); fwrite(tu.d, 4, TUNER_WIN, f);
            fwrite(tu.dn, 4, TUNER_WIN, f); fclose(f);
        }
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "tuner")) {
        long sz;
        float *x = load(argv[2], &sz);
        long n = sz / 4;
        size_t blk = argc >= 5 ? (size_t)atoi(argv[4]) : 1;
        tuner_init(&tu, atoi(argv[3]));
        tuner_result_t r;
        for (long i = 0; i < n; i += (long)blk) {
            size_t m = (size_t)(n - i) < blk ? (size_t)(n - i) : blk;
            for (size_t k = 0; k < m; k++) {
                tuner_feed(&tu, x + i + k, 1);
                /* poll right after the sample that filled a buffer, like the
                 * emulated stock main loop does */
                if (tuner_poll(&tu, &r))
                    printf("%ld %d %d %d %.6f %.3f %.4f %d %d\n", i + (long)k, r.note, r.deviation,
                           r.octave, (double)r.freq, (double)r.cents, (double)r.confidence,
                           r.silent, r.valid);
            }
        }
        return 0;
    }
    fprintf(stderr, "usage: see source\n");
    return 2;
}
