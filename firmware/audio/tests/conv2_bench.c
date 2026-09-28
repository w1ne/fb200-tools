/* conv2 cost per 32-sample block. Built for the Cortex-M7 and run in Unicorn
 * by tools/conv2_cycles.py (instruction counts per call); built for the
 * host with -DBENCH_HOST_MAIN it prints wall-clock times (rough check only). */
#include <stdint.h>
#include "dsp/conv2.h"

static conv2_tail_t s_tail;
static conv2_t s_c;
static conv_t s_cab;                           /* today's 512-tap cab convolver */
static float s_ch[CONV2_HEAD_PARTS][CONV_N], s_cx[CONV2_HEAD_PARTS][CONV_N];
static float s_ir[CONV2_MAX_TAPS], s_buf[DSP_BLOCK];
static uint32_t s_seed = 1;

static float lcg(void)
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return (float)(int32_t)s_seed * (1.0f / 2147483648.0f);
}

void bench_setup(void)
{
    for (unsigned i = 0; i < CONV2_MAX_TAPS; i++) s_ir[i] = lcg() * (1.0f - (float)i / CONV2_MAX_TAPS);
    (void)conv2_init(&s_c, &s_tail);
    (void)conv_init(&s_cab, s_ch, s_cx, CONV2_HEAD_PARTS);
    (void)conv_set_ir(&s_cab, s_ir, CONV2_HEAD_TAPS);
}

void bench_set_ir(unsigned taps) { (void)conv2_set_ir(&s_c, s_ir, taps); }

/* One block through conv2 (next slice of the frame) */
void bench_block(void)
{
    for (unsigned i = 0; i < DSP_BLOCK; i++) s_buf[i] = lcg();
    conv2_process(&s_c, s_buf, s_buf, DSP_BLOCK);
}

/* One block through the lone conv_t (the cab today) */
void bench_cab_block(void)
{
    for (unsigned i = 0; i < DSP_BLOCK; i++) s_buf[i] = lcg();
    conv_process(&s_cab, s_buf, s_buf, DSP_BLOCK);
}

void bench_fill(void)                          /* the lcg loop alone, subtracted */
{
    for (unsigned i = 0; i < DSP_BLOCK; i++) s_buf[i] = lcg();
}

#ifdef BENCH_HOST_MAIN
#include <stdio.h>
#include <time.h>

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

int main(void)
{
    enum { FRAMES = 4000 };
    static double t[CONV2_SLICES];
    bench_setup();
    bench_set_ir(CONV2_MAX_TAPS);
    for (unsigned f = 0; f < FRAMES; f++)
        for (unsigned k = 0; k < CONV2_SLICES; k++) {
            double t0 = now();
            bench_block();
            t[k] += now() - t0;
        }
    double c0 = now();
    for (unsigned f = 0; f < FRAMES * CONV2_SLICES; f++) bench_cab_block();
    double cab = (now() - c0) / (FRAMES * CONV2_SLICES);
    printf("host ns per block: cab (conv_t 512) %.0f\n", cab * 1e9);
    for (unsigned k = 0; k < CONV2_SLICES; k++) printf("  conv2 4096 slice %u: %.0f\n", k, t[k] / FRAMES * 1e9);
    return 0;
}
#endif
