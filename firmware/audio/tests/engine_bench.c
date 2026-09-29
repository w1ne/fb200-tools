/* Amp (+ tone stack) and cab cost per 32-sample block, built for the
 * Cortex-M7 with the firmware's flags and run in Unicorn by
 * tools/engine_cycles.py (instruction counts and a cycle estimate per
 * function; bit-exact old/new comparison). The Python side writes the stock
 * data blob to memory and the input block into bench_buf.
 * Built with -ffp-contract=off like the firmware's stock objects. */
#include <stdint.h>
#include "dsp/amp.h"
#include "dsp/cab.h"
#include "dsp/stock_data.h"

const stock_data_t *g_stock;
static amp_t s_amp;
static cab_t s_cab;
static conv2_tail_t s_tail;
static float s_ir[CAB_MAX_TAPS];
float bench_buf[DSP_BLOCK];
/* model, gain, bass, mid, midfreq, treble, volume, cab, long-IR taps (0:
 * the stock cab, head only): written by Python */
volatile int32_t bench_args[9];

void bench_setup(const stock_data_t *blob)
{
    g_stock = blob;
    amp_init(&s_amp, 44100.0f);
    (void)amp_set_model(&s_amp, bench_args[0]);
    amp_set_params(&s_amp, bench_args[1], bench_args[2], bench_args[3], bench_args[4],
                   bench_args[5], bench_args[6]);
    unsigned taps = (unsigned)bench_args[8];
    if (!taps) {
        cab_init(&s_cab);
        if (bench_args[7]) (void)cab_set_model(&s_cab, bench_args[7]);
        return;
    }
    /* long IR (as `cab long` on the pedal): the stock cab, then a decaying tail */
    cab_init_long(&s_cab, &s_tail);
    const float *h = g_stock->cab_taps[bench_args[7] > 0 ? bench_args[7] - 1 : 0];
    for (unsigned i = 0; i < taps && i < CAB_MAX_TAPS; i++)
        s_ir[i] = h[i % CAB_TAPS] * (1.0f - (float)i / CAB_MAX_TAPS);
    (void)cab_set_ir_len(&s_cab, s_ir, taps, 1.0f);
    conv2_finish(&s_cab.conv);
}

void bench_amp(void) { amp_process(&s_amp, bench_buf, DSP_BLOCK); }

void bench_cab(void) { cab_process(&s_cab, bench_buf, DSP_BLOCK); }
