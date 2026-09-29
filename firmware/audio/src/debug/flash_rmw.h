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
/* The long IR store (irstore/irstore.h IRSTORE_BASE..IRSTORE_END): only on
 * a chip that holds all of it (flash_capacity). */
#define FLASH_IRSTORE_BASE  0x00400000u
#define FLASH_IRSTORE_LIMIT 0x00502000u
/* The looper's area (loopstore/loopstore.h): F:0x510000 (64 kB aligned,
 * after the long IR store) to the end of the chip (flash_capacity), at most
 * FLASH_LOOP_END. FLASH_LOOP_META (one sector below it) holds `loop save`.
 * Only on a chip that holds at least FLASH_LOOP_MIN of it. */
#define FLASH_LOOP_META  0x0050F000u
#define FLASH_LOOP_BASE  0x00510000u
#define FLASH_LOOP_END   0x00800000u
#define FLASH_LOOP_MIN   0x00100000u
/* The vendor bootloader's update-flag sector: never written (the A+D
 * recovery depends on it). */
#define FLASH_UPDATE_FLAG 0x00086000u

/* The end of the looper's area on this chip (flash_capacity), or 0: no
 * area (a chip smaller than FLASH_LOOP_BASE + FLASH_LOOP_MIN). */
uint32_t flash_loop_end(void);

/* Rewrite [offset, offset + len) inside one sector of the data store, the
 * long IR store, or the looper's area with its meta sector (read-modify-write,
 * as the stock does), then read it back.
 * 0 on success; -1 bad range, -2 erase failed, -3 program failed,
 * -4 read-back differs. */
int flash_rmw(uint32_t offset, const void *data, uint32_t len);
/* The same write, split so the caller can fill the bytes in the sector
 * buffer (there is no second buffer). stage copies the sector and returns
 * the slice; commit erases and programs it. NULL / -1 on a bad range.
 * Not re-entered. The slice is valid until commit. */
void *flash_rmw_stage(uint32_t offset, uint32_t len);
int flash_rmw_commit(void);
/* Poll the status register until WIP clears, running flash_pump() while the
 * flash is busy. 1 idle, 0 status read failed or timeout. */
int flash_wait_idle(uint32_t timeout_ms);

/* ---- primitives (selfupdate.c on the target) ---- */
int flash_cmd_init(void);                                   /* 1 ok */
int flash_write_enable(void);                               /* 1: WEL set */
int flash_cmd_erase(uint32_t sector);                       /* 1: command sent */
int flash_cmd_program(uint32_t page, const uint32_t *data); /* FLASH_PAGE bytes; 1: sent */
int flash_read_status(uint32_t *sr);                        /* 1 ok */
int flash_read_status2(uint32_t *sr2);                      /* 1 ok (35h: SUS = bit 7) */
int flash_cmd_read(uint32_t offset, void *dst, uint32_t len); /* IP fast read (0Bh); 1 ok */
int flash_cmd_erase_block(uint32_t block);                  /* 64 kB (D8h); 1: command sent */
int flash_cmd_suspend(void);                                /* 75h; 1: sent */
int flash_cmd_resume(void);                                 /* 7Ah; 1: sent */
/* Usable flash bytes (the chip and the FlexSPI window), 0 unknown. A RAM
 * read: flash_probe fills it at boot, never while the flash is busy. */
uint32_t flash_capacity(void);
void flash_refresh(uint32_t offset, uint32_t len);          /* drop stale AHB/cache lines */
const void *flash_map(uint32_t offset);                     /* memory-mapped read */
uint32_t flash_now_ms(void);
/* The audio work that must go on while the flash is busy: watchdog, USB
 * audio rings, the engine. Never reads flash, never writes it. */
void flash_pump(void);
#endif
