/* Amp (+ tone stack), cab and looper cost per 32-sample block, built for the
 * Cortex-M7 with the firmware's flags and run in Unicorn by
 * tools/engine_cycles.py (instruction counts and a cycle estimate per
 * function; bit-exact old/new comparison). The Python side writes the stock
 * data blob to memory and the input block into bench_buf.
 * Built with -ffp-contract=off like the firmware's stock objects. */
#include <stdint.h>
#include "dsp/amp.h"
#include "dsp/cab.h"
#include "dsp/stock_data.h"
#include "dsp/looper.h"

const stock_data_t *g_stock;
static amp_t s_amp;
static cab_t s_cab;
static conv2_tail_t s_tail;
static float s_ir[CAB_MAX_TAPS];
float bench_buf[DSP_BLOCK];
/* model, gain, bass, mid, midfreq, treble, volume, cab, long-IR taps (0:
 * the stock 512-tap cab): written by Python */
volatile int32_t bench_args[9];

void bench_setup(const stock_data_t *blob)
{
    g_stock = blob;
    amp_init(&s_amp, 44100.0f);
    (void)amp_set_model(&s_amp, bench_args[0]);
    amp_set_params(&s_amp, bench_args[1], bench_args[2], bench_args[3], bench_args[4],
                   bench_args[5], bench_args[6]);
    unsigned taps = (unsigned)bench_args[8];
    cab_init_long(&s_cab, &s_tail);          /* as the engine (ENGINE_LONG_IR) */
    if (!taps) {
        if (bench_args[7]) (void)cab_set_model(&s_cab, bench_args[7]);
        return;
    }
    /* long IR (as `cab long` on the pedal): the stock cab, then a decaying tail */
    const float *h = g_stock->cab_taps[bench_args[7] > 0 ? bench_args[7] - 1 : 0];
    for (unsigned i = 0; i < taps && i < CAB_MAX_TAPS; i++)
        s_ir[i] = h[i % CAB_TAPS] * (1.0f - (float)i / CAB_MAX_TAPS);
    (void)cab_set_ir_len(&s_cab, s_ir, taps, 1.0f);
    conv2_finish(&s_cab.conv);
}

void bench_amp(void) { amp_process(&s_amp, bench_buf, DSP_BLOCK); }

void bench_cab(void) { cab_process(&s_cab, bench_buf, DSP_BLOCK); }

/* a short block (a SAI hiccup on the pedal): shifts the partition phase */
void bench_cab_n(unsigned n) { cab_process(&s_cab, bench_buf, n); }

/* The looper (tools/engine_cycles.py --looper): 400 blocks of memory (the
 * engine lends it ~1400; the cost does not depend on the size). mode bit 0:
 * hq. Then bench_loop_cmd(LOOPER_*) as the console does, with the poll. */
static looper_t s_loop;
static uint8_t s_loop_mem[LOOPER_BLK_BYTES * 400];

void bench_loop_setup(unsigned mode)
{
    looper_init(&s_loop);
    (void)looper_set_hq(&s_loop, (int)(mode & 1u));
    looper_attach(&s_loop, s_loop_mem, sizeof s_loop_mem, 0, 0);
    (void)looper_cmd(&s_loop, LOOPER_REC_A);
}

void bench_loop_cmd(int action)
{
    (void)looper_cmd(&s_loop, action);
    looper_poll(&s_loop);
}

/* L and R are the same buffer here: the cost is the same */
void bench_loop(void)
{
    looper_process(&s_loop, bench_buf, bench_buf, DSP_BLOCK);
    looper_poll(&s_loop);
}
