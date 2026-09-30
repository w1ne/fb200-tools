/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

/* Flash operations for the looper: see lsio.h. RAM code (ITCM, like
 * flash_rmw.c): hot_path.py flash-write roots. */
#include <string.h>
#include "loopstore/lsio.h"
#include "debug/flash_rmw.h"

#define SR_WIP   0x01u
#define SR2_SUS  0x80u
#define ERASE_TIMEOUT_MS 6000u   /* busy time: 3 x the 64 kB block max (2 s, W25Q64JV) */
#define SUSPEND_WAIT_MS  2u      /* tSUS 20 us; more means no suspend */

lsio_stats_t g_lsio;

static struct {
    uint8_t on;          /* an erase is started and not finished */
    uint8_t susp;        /* ... and suspended */
    uint8_t susp_ok;
    uint32_t busy_ms;    /* busy time of this erase so far */
} er;

void lsio_init(int suspend_ok)
{
    memset(&er, 0, sizeof er);
    er.susp_ok = suspend_ok ? 1u : 0u;
}

int lsio_erase_active(void) { return er.on; }

static int finish(int r);
int lsio_erase_run(uint32_t slice_ms);

int lsio_read(uint32_t off, void *dst, uint32_t len)
{
    g_lsio.reads++;
    return flash_cmd_read(off, dst, len);
}

/* the read-back, in its own frame: the buffer is not on the stack while the
 * wait above runs the audio */
__attribute__((noinline)) static int verify(uint32_t off, const uint32_t *page)
{
    uint32_t back[FLASH_PAGE / 4];
    if (!flash_cmd_read(off, back, FLASH_PAGE)) return 0;
    return memcmp(back, page, FLASH_PAGE) == 0;
}

int lsio_program(uint32_t off, const uint32_t *page)
{
    uint32_t t0 = flash_now_ms();
    int ok = flash_write_enable() && flash_cmd_program(off, page) && flash_wait_idle(100u);
    uint32_t dt = flash_now_ms() - t0;
    flash_refresh(off, 0);
    g_lsio.pages++;
    if (dt > 1u) g_lsio.prog_slow++;
    if (dt > g_lsio.prog_max_ms) g_lsio.prog_max_ms = dt;
    if (!ok) {
        g_lsio.prog_errs++;
        return 0;
    }
    if (!verify(off, page)) {
        g_lsio.verify_errs++;
        return 0;
    }
    return 1;
}

int lsio_erase_begin(uint32_t off, int block64)
{
    if (er.on) return 0;
    if (!flash_write_enable() || !(block64 ? flash_cmd_erase_block(off) : flash_cmd_erase(off))) {
        g_lsio.erase_errs++;
        return 0;
    }
    er.on = 1;
    er.susp = 0;
    er.busy_ms = 0;
    g_lsio.erases++;
    /* slice 0: suspend on the first poll (or run the erase out on a chip
     * with no suspend). Do not return to the XIP caller while WIP is set. */
    return lsio_erase_run(0) != LSIO_FAIL;
}

static int finish(int r)
{
    if (r == LSIO_DONE || r == LSIO_FAIL) {
        if (r == LSIO_FAIL) g_lsio.erase_errs++;
        g_lsio.erase_ms += er.busy_ms;
        if (er.busy_ms > g_lsio.erase_max_ms) g_lsio.erase_max_ms = er.busy_ms;
        er.on = er.susp = 0;
    }
    flash_refresh(0, 0);   /* the flash was busy: drop stale AHB/cache lines */
    return r;
}

int lsio_erase_run(uint32_t slice_ms)
{
    uint32_t sr;
    if (!er.on) return LSIO_DONE;
    if (er.susp) {
        if (!flash_cmd_resume()) return finish(LSIO_FAIL);
        er.susp = 0;
    }
    g_lsio.slices++;
    uint32_t t0 = flash_now_ms();
    for (;;) {
        if (!flash_read_status(&sr)) return finish(LSIO_FAIL);
        uint32_t dt = flash_now_ms() - t0;
        if (!(sr & SR_WIP)) {
            er.busy_ms += dt;
            return finish(LSIO_DONE);
        }
        if (er.busy_ms + dt > ERASE_TIMEOUT_MS) {
            er.busy_ms += dt;
            return finish(LSIO_FAIL);
        }
        if (er.susp_ok && dt >= slice_ms) {
            er.busy_ms += dt;
            break;
        }
        flash_pump();
    }
    /* suspend: WIP clears within tSUS, SUS tells whether it ended first */
    if (!flash_cmd_suspend()) return finish(LSIO_FAIL);
    uint32_t t1 = flash_now_ms();
    for (;;) {
        if (!flash_read_status(&sr)) return finish(LSIO_FAIL);
        if (!(sr & SR_WIP)) break;
        uint32_t dt = flash_now_ms() - t1;
        if (dt > SUSPEND_WAIT_MS) {
            er.susp_ok = 0;    /* no suspend on this chip: run erases to the end */
            if (er.busy_ms + dt > ERASE_TIMEOUT_MS) return finish(LSIO_FAIL);
        }
        flash_pump();
    }
    er.busy_ms += flash_now_ms() - t1;
    uint32_t sr2;
    if (!flash_read_status2(&sr2)) return finish(LSIO_FAIL);
    if (!(sr2 & SR2_SUS)) return finish(LSIO_DONE);   /* it ended before the suspend */
    er.susp = 1;
    g_lsio.suspends++;
    return finish(LSIO_SUSPENDED);
}

void lsio_quiesce(void)
{
    while (er.on) (void)lsio_erase_run(ERASE_TIMEOUT_MS + 1u);   /* to its end (or the timeout) */
}
