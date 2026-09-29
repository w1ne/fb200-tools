#ifndef FB200_DSP_LOOP_MEM_H
#define FB200_DSP_LOOP_MEM_H
/* The looper borrows its memory (RAM is full, docs/FIRMWARE_BRINGUP.md
 * "Memory map"): the delay line (dsp/delay.h, ~88 kB) and the long-IR tail
 * (dsp/conv2.h conv2_tail_t, ~96 kB). Never both users at once:
 *
 *   take: the cab drops to its first 512 taps (cab_detach_tail), the delay
 *         must not run (the engine keeps it off while `owned`), the looper
 *         gets both areas (EMPTY).
 *   give: the looper is detached (OFF, the loop is gone), the delay line is
 *         cleared (delay_clear: silence, filters reset) and the tail is
 *         cleared and attached again. The engine re-applies the preset: the
 *         delay plays again from a silent line.
 *
 * Main loop only, between audio blocks. */
#include <stdint.h>
#include "looper.h"
#include "delay.h"
#include "cab.h"

typedef struct {
    looper_t *lp;
    delay_t *dly;
    int16_t *line;              /* the delay line, DELAY_LEN samples */
    cab_t *cab;
    conv2_tail_t *tail;         /* NULL: a build without long IRs */
    int owned;                  /* the looper has the memory */
} loop_mem_t;

void loop_mem_take(loop_mem_t *m);
void loop_mem_give(loop_mem_t *m);
#endif
