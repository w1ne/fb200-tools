/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

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
#include <string.h>
#include "dsp/looper.h"
#include "loopstore/loopstore.h"
#include "loopstore/lsio.h"

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

/* The looper (tools/engine_cycles.py --looper): the audio side per block
 * (bench_loop, profiled), its flash side on a RAM "flash" of 16 chunks
 * (bench_loop_flash, not profiled: main loop work). Then
 * bench_loop_cmd(LOOPER_*) as the console does, with the poll. */
#define AREA (16u * LS_CHUNK)
static uint8_t s_flash[AREA];
static uint32_t s_ms;
int flash_cmd_init(void) { return 1; }
int flash_write_enable(void) { return 1; }
int flash_cmd_erase(uint32_t sector) { memset(s_flash + sector, 0xFF, FLASH_SECTOR); return 1; }
int flash_cmd_erase_block(uint32_t block) { memset(s_flash + block, 0xFF, 0x10000u); return 1; }
int flash_cmd_program(uint32_t page, const uint32_t *data)
{
    const uint8_t *d = (const uint8_t *)data;
    for (uint32_t i = 0; i < FLASH_PAGE; i++) s_flash[page + i] &= d[i];
    return 1;
}
int flash_cmd_read(uint32_t off, void *dst, uint32_t len) { memcpy(dst, s_flash + off, len); return 1; }
int flash_read_status(uint32_t *sr) { *sr = 0; return 1; }
int flash_read_status2(uint32_t *sr2) { *sr2 = 0; return 1; }
int flash_cmd_suspend(void) { return 1; }
int flash_cmd_resume(void) { return 1; }
void flash_refresh(uint32_t offset, uint32_t len) { (void)offset; (void)len; }
const void *flash_map(uint32_t offset) { return s_flash + offset; }
uint32_t flash_now_ms(void) { return s_ms++; }
uint32_t flash_capacity(void) { return 0; }
void flash_pump(void) {}

static looper_t s_loop;
static loopio_t s_io;
static loopstore_t s_ls;
static uint16_t s_map[2][LS_MAX_CHUNKS];

void bench_loop_setup(unsigned mode)
{
    (void)mode;
    lsio_init(1);
    (void)ls_init(&s_ls, &s_io, s_map[0], s_map[1], 0, AREA);
    ls_arm(&s_ls);
    for (int i = 0; i < 8; i++) ls_task(&s_ls);   /* the whole "flash" erased */
    looper_init(&s_loop, &s_io, &s_ls);
    (void)looper_cmd(&s_loop, LOOPER_REC_A);
}

void bench_loop_cmd(int action)
{
    (void)looper_cmd(&s_loop, action);
    looper_poll(&s_loop);
}

/* L and R are the same buffer here: the cost is the same */
void bench_loop(void) { looper_process(&s_loop, bench_buf, bench_buf, DSP_BLOCK); }

void bench_loop_flash(void)
{
    ls_task(&s_ls);
    looper_poll(&s_loop);
}
