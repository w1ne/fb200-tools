#ifndef FB200_RECOVERY_H
#define FB200_RECOVERY_H
#include <stdint.h>
/* Two-stage boot (docs/BOOTLOADER.md §4):
 *
 *   vendor bootloader -> recovery (block 0, 0x60010000..0x60020000)
 *                     -> app     (slot,    0x60020000..0x60041000)
 *
 * Recovery is the USB console plus self-update; the app slot is rewritten
 * over USB and recovery never is (except by an explicit `fwrec`). Recovery
 * stays in charge when the slot is invalid, the app faulted, the watchdog
 * fired, or the app asked for it (`recovery`). */

#define SLOT_FLASH      0x60020000u
#define SLOT_MAGIC      0x50414246u   /* "FBAP" */
#define SLOT_HDR_SIZE   0x100u
#define SLOT_VECTORS    (SLOT_FLASH + SLOT_HDR_SIZE)
#define SLOT_BLOB       (SLOT_VECTORS + 0x400u)
#define APP_ITCM_LIMIT  0x1F000u      /* copier lives above */

typedef struct {
    uint32_t magic;
    uint32_t blob_len;    /* bytes loaded to ITCM 0x400 */
    uint32_t crc;         /* CRC32 of vectors (0x400) + blob */
    uint32_t version;
} slot_header_t;

void wdog_feed(void);
void crumbs_print(void);
void crashdump_print(void);   /* full dump of the last fault (survives reset) */
void crashdump_clear(void);
void crumb_alive(void);   /* app: mark running (see recovery.c) */
void crumb_clear(void);   /* before a deliberate reset */
void recovery_request(void) __attribute__((noreturn));   /* app -> recovery */
const char *recovery_boot(void);   /* recovery: returns the stay reason or launches the app */
void recovery_launch_app(int usb_up) __attribute__((noreturn));   /* 1 = console running */
int slot_valid(const char **why);
#endif
