#ifndef FB200_FLASH_RMW_H
#define FB200_FLASH_RMW_H
/* NOR flash writes that keep the audio running.
 *
 * A sector erase keeps the flash busy for tens of ms, a page program for
 * about 1 ms; an audio block is 0.73 ms. The main loop is blocked for that
 * time, so the busy wait runs the audio pump (flash_pump: engine_pump,
 * RAM code that reads no flash) between status polls.
 *
 * flash_rmw and flash_wait_idle are pure logic on top of the primitives
 * below: selfupdate.c implements them with FlexSPI IP commands, the host test
 * (tests/flash_rmw_host_test.c) with a fake flash. Everything here runs from
 * RAM (firmware/tools/hot_path.py: flash-write and flash-busy roots). */
#include <stdint.h>

#define FLASH_SECTOR 4096u
#define FLASH_PAGE   256u
/* The data store (presets, settings, rhythm, BT name, IR names/flags, the
 * 9 user IR slots): F:0x71000..0xA1800 (docs/UI_AND_STORAGE.md §5). */
#define FLASH_STORE_BASE  0x00071000u
#define FLASH_STORE_LIMIT 0x000A1800u
/* The vendor bootloader's update-flag sector: never written (the A+D
 * recovery depends on it). */
#define FLASH_UPDATE_FLAG 0x00086000u

/* Rewrite [offset, offset + len) inside one sector of the data store
 * (read-modify-write, as the stock does), then read it back.
 * 0 on success; -1 bad range, -2 erase failed, -3 program failed,
 * -4 read-back differs. */
int flash_rmw(uint32_t offset, const void *data, uint32_t len);
/* Poll the status register until WIP clears, running flash_pump() while the
 * flash is busy. 1 idle, 0 status read failed or timeout. */
int flash_wait_idle(uint32_t timeout_ms);

/* ---- primitives (selfupdate.c on the target) ---- */
int flash_cmd_init(void);                                   /* 1 ok */
int flash_write_enable(void);                               /* 1: WEL set */
int flash_cmd_erase(uint32_t sector);                       /* 1: command sent */
int flash_cmd_program(uint32_t page, const uint32_t *data); /* FLASH_PAGE bytes; 1: sent */
int flash_read_status(uint32_t *sr);                        /* 1 ok */
void flash_refresh(uint32_t offset, uint32_t len);          /* drop stale AHB/cache lines */
const void *flash_map(uint32_t offset);                     /* memory-mapped read */
uint32_t flash_now_ms(void);
/* The audio work that must go on while the flash is busy: watchdog, USB
 * audio rings, the engine. Never reads flash, never writes it. */
void flash_pump(void);
#endif
