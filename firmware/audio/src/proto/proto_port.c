/* Target glue for the app protocol: flash access through the XIP window and
 * flash_store() (sector read-modify-write), battery from the power monitor. */
#include <string.h>
#include "proto/proto.h"
#include "debug/selfupdate.h"
#include "ui/power.h"

#define FLASH_AHB 0x60000000u
#define SECTOR 0x1000u

void proto_flash_read(uint32_t off, void *dst, uint32_t n)
{
    memcpy(dst, (const void *)(FLASH_AHB + off), n);
}

int proto_flash_write(uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *p = src;
    while (n) {   /* flash_store works inside one 4 KB sector */
        uint32_t k = SECTOR - (off & (SECTOR - 1u));
        if (k > n) k = n;
        int r = flash_store(off, p, k);
        if (r) return r;
        off += k;
        p += k;
        n -= k;
    }
    return 0;
}

void proto_battery(uint8_t *percent, uint8_t *charging)
{
    const power_state_t *s = power_state();
    *percent = (uint8_t)(s->level * 25u);   /* stock: level 0..4 x 25 */
    *charging = s->charging;
}
