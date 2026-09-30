/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#ifndef FB200_SELFUPDATE_H
#define FB200_SELFUPDATE_H
#include <stdint.h>
/* USB self-update: rewrites the app slot (flash 0x60020000..0x60061000),
 * with `fwrec` the recovery image (0x60010000..0x60020000), or with `fwstock`
 * the stock data (0x60061000..0x60071000, dsp/stock_data.h), through FlexSPI
 * IP commands (see selfupdate.c). The vendor bootloader (0x60000000..
 * 0x60010000) and the model library are never touched, so A+D stays the
 * last-resort recovery path.
 *
 * Protocol (console):
 *   fwbegin|fwrec|fwstock <len> <crc32>  erase, then read exactly <len> raw bytes
 *   -> "fw ready"          host streams the bytes (no echo)
 *   -> "fw done crc=.. ok" (or "BAD"), then the host sends `reset`
 * Idle for FW_IDLE_MS during the stream aborts back to line mode. */
#include "crc32.h"
typedef enum { FW_APP, FW_RECOVERY, FW_STOCK } fw_target_t;
/* Stock data versions this release's app accepts (dsp/stock_data.h), printed
 * by `fwstock` with no arguments; host tools write the newest one listed.
 * Older firmware prints only the usage line: host tools then write version 1. */
#define FW_STOCK_FORMATS "1 2"
void fw_begin(fw_target_t target, uint32_t len, uint32_t crc);
int fw_active(void);
void fw_rx_task(void);   /* call instead of the line reader while active */
void fw_info(void);
/* 1 once an app update started erasing the app slot: the app's cold code
 * (XIP) is gone, only RAM code may run (fw_session, selfupdate.c). */
int fw_xip_gone(void);
void fw_session(void) __attribute__((noreturn));
void fw_test(void);   /* non-destructive: WREN must set WEL */
/* Preset/settings/IR store: RMW of one 4 KB sector (flash_rmw.c) in
 * F:0x71000..0xA1800, the long IR store (irstore.h), or the looper's area,
 * when the chip holds it. -5 while blocked. -6: a looper record or dub is
 * writing (dsp/looper.h looper_writing): try again later. */
int flash_store(uint32_t offset, const void *data, uint32_t len);
/* 1: flash_store refuses (-5), battery critical (ui/power.c) */
void flash_store_block(int on);
/* Flash chip: JEDEC ID (console `jedec`; 1 on success), the window FlexSPI
 * port A1 maps, the size from the capacity code (0: unknown).
 * flash_capacity (flash_rmw.h) is the smaller of the two, filled by
 * flash_probe at boot. */
int flash_read_id(uint8_t id[3]);
uint32_t flash_window(void);
uint32_t flash_chip_size(const uint8_t id[3]);
int flash_suspend_ok(void);   /* Erase Suspend: Winbond, GigaDevice */
uint32_t flash_probe(void);   /* reads the JEDEC ID once: flash_capacity() from then on */
#endif
