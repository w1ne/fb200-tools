/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

/* Target glue for the long IR store (irstore.h): flash through the XIP
 * window and flash_store() (sector read-modify-write, RAM code), the chip
 * size from flash_capacity(), console bytes from the CDC, the stock gain
 * rule and the engine's IR staging buffer. */
#include "irstore/irstore.h"
#include "debug/selfupdate.h"
#include "debug/flash_rmw.h"
#include "audio/engine.h"
#include "audio/usb_audio.h"
#include "dsp/cab.h"
#include "tusb.h"

#define FLASH_AHB 0x60000000u
/* flash_rmw accepts the store's range (and flash_store is its only writer) */
_Static_assert(IRSTORE_BASE == FLASH_IRSTORE_BASE && IRSTORE_END == FLASH_IRSTORE_LIMIT,
               "flash_rmw.h FLASH_IRSTORE_*");

const uint8_t *irstore_map(uint32_t off) { return (const uint8_t *)(FLASH_AHB + off); }

int irstore_flash_write(uint32_t off, const void *data, uint32_t len)
{
    return flash_store(off, data, len);
}

uint32_t irstore_capacity(void) { return flash_capacity(); }

uint32_t irstore_rx(uint8_t *dst, uint32_t max)
{
    return tud_cdc_available() ? tud_cdc_read(dst, max) : 0u;
}

extern uint32_t tusb_time_millis_api(void);
uint32_t irstore_now_ms(void) { return tusb_time_millis_api(); }

float irstore_gain(const float *ir) { return cab_user_ir_gain(ir); }

void irstore_pump(void)
{
    usb_audio_task();
    engine_task();
}

float *irstore_buf_get(void) { return engine_ir_borrow(); }
void irstore_buf_put(void) { engine_ir_release(); }
