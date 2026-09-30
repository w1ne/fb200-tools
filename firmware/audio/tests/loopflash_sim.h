/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#ifndef FB200_TESTS_LOOPFLASH_SIM_H
#define FB200_TESTS_LOOPFLASH_SIM_H
/* A simulated W25Q64 behind the flash_rmw.h primitives, for the looper's
 * host tests (looper_host_test.c) and the harnesses that run the real
 * looper (proto, fuzz). Time is simulated (sim_us): every command and
 * status read costs a little, a program or an erase keeps the chip busy for
 * its time. It checks what the real chip forbids or what would corrupt the
 * loop:
 *   - any command but status/suspend while busy (reads return garbage);
 *   - during an erase suspend: another erase, or a read/program inside the
 *     sector or block being erased;
 *   - a program that would need a 0 -> 1 bit (not erased: NOR);
 *   - a program or erase without write enable, outside the looper's area;
 *   - a read of bytes not programmed since the last erase (at boot the
 *     area is unknown: nothing may be read before the looper wrote it).
 * Violations are counted in sim_errors (and printed). */
#include <stdint.h>

typedef struct {
    uint32_t prog_us, sector_us, block_us;   /* busy times */
    int suspend;                             /* Erase Suspend works */
} sim_timing_t;

extern const sim_timing_t SIM_TYPICAL;   /* W25Q64JV typical: 0.4 / 45 / 150 ms */
extern const sim_timing_t SIM_WORST;     /* datasheet max: 3 / 400 / 2000 ms */
extern const sim_timing_t SIM_INSTANT;   /* for the UI harnesses */

extern uint64_t sim_us;          /* simulated time */
extern unsigned sim_errors;
extern uint32_t sim_capacity;    /* flash_capacity() (8 MB) */
extern void (*sim_pump)(void);   /* what flash_pump runs (the audio) */

void sim_flash_init(const sim_timing_t *t);   /* the area holds garbage, never written */
void sim_flash_free(void);

/* the whole looper on the simulated flash: io, store, maps (static) */
struct loopstore;
struct loopstore *sim_loop_store(void);
void *sim_loop_io(void);
void sim_loop_setup(void *looper);   /* ls_init + looper_init + ls_arm */
#endif
