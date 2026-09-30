/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#ifndef FB200_LOOPSTORE_LSIO_H
#define FB200_LOOPSTORE_LSIO_H
/* Flash operations for the looper (loopstore.c) that keep the audio and the
 * UI running. RAM code (ITCM): each call returns only with the flash idle or
 * with an erase suspended, so the caller may be XIP code (COLD).
 *
 *   lsio_program: one page, then waits (flash_wait_idle: the audio pump
 *     runs) and reads the page back.
 *   lsio_erase_begin: a 4 kB sector or a 64 kB block. It returns only once
 *     that erase is suspended or finished. The caller is XIP code, so a
 *     return while the erase is still running fetches instructions from a
 *     busy flash (the chip then comes up in the ROM serial loader).
 *   lsio_erase_run: resumes a suspended erase, runs the audio pump while it
 *     is busy, and after slice_ms suspends it again (Erase Suspend 75h,
 *     Winbond and GigaDevice; SUS in status register 2) so the main loop
 *     can run. While it is suspended the flash reads and programs other
 *     sectors: the looper's streams never wait for an erase. A chip without
 *     suspend runs the erase to its end (the pump still runs).
 *   lsio_quiesce: resume a suspended erase and wait for its end. Every other
 *     flash writer calls it first (flash_cmd_init, fw_begin): an erase or a
 *     status write is not allowed while an erase is suspended.
 *
 * Pure logic on the flash_rmw.h primitives: the host test runs it on a
 * simulated flash (tests/loopflash_sim.c). */
#include <stdint.h>

enum { LSIO_DONE, LSIO_SUSPENDED, LSIO_FAIL };

typedef struct {
    uint32_t pages, prog_slow, prog_max_ms;   /* page programs; over 1 ms; the longest */
    uint32_t verify_errs, prog_errs;
    uint32_t erases, erase_errs, erase_ms, erase_max_ms;   /* units; busy time */
    uint32_t slices, suspends, reads;
} lsio_stats_t;

extern lsio_stats_t g_lsio;

void lsio_init(int suspend_ok);      /* suspend_ok: the chip has Erase Suspend */
int lsio_read(uint32_t off, void *dst, uint32_t len);            /* 1 ok */
int lsio_program(uint32_t off, const uint32_t *page);            /* 1 ok (verified) */
int lsio_erase_begin(uint32_t off, int block64); /* 1: started; idle or suspended */
int lsio_erase_run(uint32_t slice_ms);                           /* LSIO_* */
int lsio_erase_active(void);         /* started, not finished (maybe suspended) */
void lsio_quiesce(void);
#endif
