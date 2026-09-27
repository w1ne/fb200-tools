#ifndef FB200_SELFUPDATE_H
#define FB200_SELFUPDATE_H
#include <stdint.h>
/* USB self-update: rewrites the application region (block 0, flash
 * 0x60010000..0x60041000) from the console, through the i.MX RT1062 ROM
 * FlexSPI NOR driver. The vendor bootloader (0x60000000..0x60010000) and the
 * model library are never touched, so A+D stays the recovery path.
 *
 * Protocol (console):
 *   fwbegin <len> <crc32>  erase, then read exactly <len> raw bytes
 *   -> "fw ready"          host streams the bytes (no echo)
 *   -> "fw done crc=.. ok" (or "BAD"), then the host sends `reset`
 * Idle for FW_IDLE_MS during the stream aborts back to line mode. */
uint32_t fw_crc32(const uint8_t *p, uint32_t len);
void fw_begin(uint32_t len, uint32_t crc);
int fw_active(void);
void fw_rx_task(void);   /* call instead of the line reader while active */
void fw_info(void);
#endif
