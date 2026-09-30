/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

/* NOR flash writes that keep the audio running: see flash_rmw.h. */
#include <string.h>
#include "debug/flash_rmw.h"

#define SR_WIP 0x01u

int flash_wait_idle(uint32_t timeout_ms)
{
    uint32_t t0 = flash_now_ms(), sr;
    for (;;) {
        if (!flash_read_status(&sr)) return 0;
        if (!(sr & SR_WIP)) return 1;
        flash_pump();   /* the flash is busy: only RAM work, no flash reads */
        if (flash_now_ms() - t0 >= timeout_ms) return 0;
    }
}

static uint32_t sector_buf[FLASH_SECTOR / 4];

uint32_t flash_loop_end(void)
{
    uint32_t cap = flash_capacity();
    if (cap > FLASH_LOOP_END) cap = FLASH_LOOP_END;
    return cap >= FLASH_LOOP_BASE + FLASH_LOOP_MIN ? cap : 0u;
}

/* [offset, offset + len), len > 0, inside the data store, the long IR store,
 * or the looper's area (with its meta sector) on a chip that holds it. */
static int in_region(uint32_t offset, uint32_t len)
{
    if (len == 0u) return 0;
    if (offset >= FLASH_STORE_BASE && offset < FLASH_STORE_LIMIT)
        return len <= FLASH_STORE_LIMIT - offset;
    if (offset >= FLASH_IRSTORE_BASE && offset < FLASH_IRSTORE_LIMIT)
        return len <= FLASH_IRSTORE_LIMIT - offset &&
               flash_capacity() >= FLASH_IRSTORE_LIMIT;
    uint32_t end = offset >= FLASH_LOOP_META ? flash_loop_end() : 0u;
    return offset < end && len <= end - offset;
}

static uint32_t staged_off, staged_len;

void *flash_rmw_stage(uint32_t offset, uint32_t len)
{
    uint32_t sector = offset & ~(FLASH_SECTOR - 1u);
    if (sector == FLASH_UPDATE_FLAG || !in_region(offset, len) ||
        len > sector + FLASH_SECTOR - offset) {
        return 0;
    }
    memcpy(sector_buf, flash_map(sector), FLASH_SECTOR);
    staged_off = offset;
    staged_len = len;
    return (uint8_t *)sector_buf + (offset - sector);
}

int flash_rmw_commit(void)
{
    uint32_t offset = staged_off, len = staged_len;
    uint32_t sector = offset & ~(FLASH_SECTOR - 1u);
    const uint8_t *slice = (const uint8_t *)sector_buf + (offset - sector);
    if (!flash_cmd_init() || !flash_write_enable() || !flash_cmd_erase(sector) ||
        !flash_wait_idle(2000u)) {
        return -2;
    }
    for (uint32_t p = 0; p < FLASH_SECTOR; p += FLASH_PAGE) {
        if (!flash_write_enable() || !flash_cmd_program(sector + p, sector_buf + p / 4u) ||
            !flash_wait_idle(100u)) {
            return -3;
        }
    }
    flash_refresh(sector, FLASH_SECTOR);
    return memcmp(flash_map(offset), slice, len) == 0 ? 0 : -4;
}

int flash_rmw(uint32_t offset, const void *data, uint32_t len)
{
    void *p = flash_rmw_stage(offset, len);
    if (!p) return -1;
    memcpy(p, data, len);
    return flash_rmw_commit();
}
