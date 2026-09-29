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

int flash_rmw(uint32_t offset, const void *data, uint32_t len)
{
    uint32_t sector = offset & ~(FLASH_SECTOR - 1u);
    if (sector == FLASH_UPDATE_FLAG || offset < FLASH_STORE_BASE ||
        offset >= FLASH_STORE_LIMIT || len == 0u || len > FLASH_STORE_LIMIT - offset ||
        len > sector + FLASH_SECTOR - offset) {
        return -1;
    }
    memcpy(sector_buf, flash_map(sector), FLASH_SECTOR);
    memcpy((uint8_t *)sector_buf + (offset - sector), data, len);
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
    return memcmp(flash_map(offset), data, len) == 0 ? 0 : -4;
}
