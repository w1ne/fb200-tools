/* USB self-update: see selfupdate.h.
 *
 * The flash controller stays exactly as the boot ROM and the vendor
 * bootloader configured it, so memory-mapped (AHB) reads keep working. We only
 * add five plain SPI-NOR sequences in LUT slots 11..15 and run them as IP
 * commands. (The ROM FlexSPI driver's init was tried first: after it, any AHB
 * read of flash, including a speculative one, hangs the core. Seen on
 * hardware 2026-09-27.)
 *
 * This file runs from ITCM and its data lives in DTCM, so flash can be
 * erased and programmed while the application runs. The app's cold code runs
 * from flash (XIP, linker.ld .xiptext, in the app slot's data area):
 * nothing that runs while the flash is busy may be there, and an app update
 * erases that very code. So fw_begin(FW_APP) never returns to its caller: it
 * ends in fw_session(), which runs only RAM code until the reset.
 * firmware/tools/hot_path.py checks both (flash-write roots).
 * While the flash is busy, flash_wait_idle (flash_rmw.c) keeps the audio
 * running (flash_pump below; hot_path.py flash-busy roots). */
#include <stdint.h>
#include <string.h>
#include "fsl_device_registers.h"
#include "fsl_flexspi.h"
#include "tusb.h"
#include "cdc_log.h"
#include "selfupdate.h"
#include "recovery.h"
#include "console.h"
#include "flash_rmw.h"
#ifndef FB200_RECOVERY
#include "audio/engine.h"
#include "audio/usb_audio.h"
#endif

#define FLASH_AHB    0x60000000u
/* Regions (flash offsets, docs/BOOTLOADER.md §4): the app slot, the
 * recovery image (block 0 start, which also holds the vendor loader) and the
 * stock data (dsp/stock_data.h). */
static const struct { uint32_t base, limit; } regions[] = {
    [FW_APP]      = {0x00020000u, 0x00061000u},   /* app slot + its tables */
    [FW_RECOVERY] = {0x00010000u, 0x00020000u},
    [FW_STOCK]    = {0x00061000u, 0x00071000u},   /* up to the presets */
};
#define FCB_TAG      0x42464346u   /* "FCFB" */
#define FCB_LUT      0x80u         /* FCB offset of the lookup table */
#define SECTOR       FLASH_SECTOR
#define PAGE         FLASH_PAGE
#define FW_IDLE_MS   3000u

#define SEQ_RDID     11u   /* JEDEC ID (FCB convention: chip erase, never used here) */
#define SEQ_WREN     12u
#define SEQ_RDSR     13u
#define SEQ_ERASE    14u
#define SEQ_PROGRAM  15u

extern uint32_t tusb_time_millis_api(void);

static uint32_t page_buf[PAGE / 4];
static uint32_t fw_off, fw_len, fw_crc, fw_rx, page_fill, last_rx_ms;
static int active;
static int lut_ready;
static volatile int xip_gone;   /* an app update has erased the cold code */

int fw_xip_gone(void) { return xip_gone; }

/* Read-command word 0 of FCB sequence 0: opcode0 = command, opcode1 = RADDR
 * with the address width in operand1. */
static int fcb_read_seq(uint32_t *pads, uint32_t *addr_bits)
{
    if (*(volatile uint32_t *)FLASH_AHB != FCB_TAG) return 0;
    uint32_t w = *(volatile uint32_t *)(FLASH_AHB + FCB_LUT);
    *pads = (w >> 8) & 3u;
    *addr_bits = (w >> 16) & 0xFFu;
    return 1;
}

static int lut_init(void)
{
    uint32_t pads, bits;
    if (!fcb_read_seq(&pads, &bits)) { log_printf("fw: no FCB\r\n"); return 0; }
    if (pads != 0u || (bits != 24u && bits != 32u)) {
        log_printf("fw: unsupported flash mode (cmd pads=%u addr=%u)\r\n",
                   (unsigned)pads, (unsigned)bits);
        return 0;
    }
    /* We overwrite LUT sequences SEQ_RDID..15: the AHB (XIP) read must not use them. */
    uint32_t cr2 = FLEXSPI->FLSHCR2[0];
    uint32_t rd = (cr2 & FLEXSPI_FLSHCR2_ARDSEQID_MASK) >> FLEXSPI_FLSHCR2_ARDSEQID_SHIFT;
    uint32_t rn = ((cr2 & FLEXSPI_FLSHCR2_ARDSEQNUM_MASK) >> FLEXSPI_FLSHCR2_ARDSEQNUM_SHIFT) + 1u;
    if (rd + rn > SEQ_RDID) {
        log_printf("fw: AHB read uses LUT seq %u..%u; not touched\r\n", (unsigned)rd,
                   (unsigned)(rd + rn - 1u));
        return 0;
    }
    /* 4-byte-address opcodes (0x21/0x12) work in either address mode. */
    uint8_t se = bits == 32u ? 0x21 : 0x20;
    uint8_t pp = bits == 32u ? 0x12 : 0x02;
    const uint32_t lut[20] = {
        [0]  = FLEXSPI_LUT_SEQ(kFLEXSPI_Command_SDR, kFLEXSPI_1PAD, 0x9F,
                               kFLEXSPI_Command_READ_SDR, kFLEXSPI_1PAD, 0x03),
        [4]  = FLEXSPI_LUT_SEQ(kFLEXSPI_Command_SDR, kFLEXSPI_1PAD, 0x06,
                               kFLEXSPI_Command_STOP, kFLEXSPI_1PAD, 0),
        [8]  = FLEXSPI_LUT_SEQ(kFLEXSPI_Command_SDR, kFLEXSPI_1PAD, 0x05,
                               kFLEXSPI_Command_READ_SDR, kFLEXSPI_1PAD, 0x04),
        [12] = FLEXSPI_LUT_SEQ(kFLEXSPI_Command_SDR, kFLEXSPI_1PAD, se,
                               kFLEXSPI_Command_RADDR_SDR, kFLEXSPI_1PAD, bits),
        [16] = FLEXSPI_LUT_SEQ(kFLEXSPI_Command_SDR, kFLEXSPI_1PAD, pp,
                               kFLEXSPI_Command_RADDR_SDR, kFLEXSPI_1PAD, bits),
        [17] = FLEXSPI_LUT_SEQ(kFLEXSPI_Command_WRITE_SDR, kFLEXSPI_1PAD, 0x04,
                               kFLEXSPI_Command_STOP, kFLEXSPI_1PAD, 0),
    };
    FLEXSPI_UpdateLUT(FLEXSPI, SEQ_RDID * 4u, lut, 20u);
    lut_ready = 1;
    return 1;
}

static status_t ip(uint32_t seq, uint32_t addr, flexspi_command_type_t type,
                   uint32_t *data, size_t len)
{
    flexspi_transfer_t x = {
        .deviceAddress = addr, .port = kFLEXSPI_PortA1, .cmdType = type,
        .seqIndex = (uint8_t)seq, .SeqNumber = 1, .data = data, .dataSize = len,
    };
    return FLEXSPI_TransferBlocking(FLEXSPI, &x);
}

static int read_status(uint32_t *sr)
{
    *sr = 0;
    return ip(SEQ_RDSR, 0, kFLEXSPI_Read, sr, 1) == kStatus_Success;
}

static int write_enable(void)
{
    uint32_t sr;
    if (ip(SEQ_WREN, 0, kFLEXSPI_Command, NULL, 0) != kStatus_Success) return 0;
    return read_status(&sr) && (sr & 0x02u);   /* WEL */
}

/* ---- flash_rmw.h primitives ---- */
int flash_cmd_init(void) { return lut_init(); }
int flash_write_enable(void) { return write_enable(); }
int flash_cmd_erase(uint32_t sector)
{
    return ip(SEQ_ERASE, sector, kFLEXSPI_Command, NULL, 0) == kStatus_Success;
}
int flash_cmd_program(uint32_t page, const uint32_t *data)
{
    return ip(SEQ_PROGRAM, page, kFLEXSPI_Write, (uint32_t *)data, PAGE) == kStatus_Success;
}
int flash_read_status(uint32_t *sr) { return read_status(sr); }
const void *flash_map(uint32_t offset) { return (const void *)(FLASH_AHB + offset); }
uint32_t flash_now_ms(void) { return tusb_time_millis_api(); }

/* While the flash is busy (flash_wait_idle): the watchdog and the audio.
 * engine_pump is RAM code that reads no flash (hot_path.py flash-busy
 * roots); the drums, whose samples are in flash, keep time silently. Not
 * tud_task: the USB audio endpoints run in the USB ISR (audiod_xfer_isr),
 * and a flash write can itself run inside tud_task (HID -> proto_feed). */
void flash_pump(void)
{
    wdog_feed();
#ifndef FB200_RECOVERY
    engine_pump();
#endif
}

/* Drop stale AHB prefetch/cache lines so reads see the new contents. While
 * the flash was busy, a speculative fetch or prefetch of any flash address
 * (XIP code included) may have cached garbage: reset the FlexSPI AHB
 * buffers, and drop the whole I-cache and the D-cache lines of the XIP code
 * as well as of the written range. */
void flash_refresh(uint32_t offset, uint32_t len)
{
#ifndef FB200_RECOVERY
    extern uint8_t __xiptext_start__[], __xiptext_end__[];
#endif
    FLEXSPI->MCR0 |= FLEXSPI_MCR0_SWRESET_MASK;
    while (FLEXSPI->MCR0 & FLEXSPI_MCR0_SWRESET_MASK) {
    }
    SCB_InvalidateDCache_by_Addr((void *)(FLASH_AHB + offset), (int32_t)len);
#ifndef FB200_RECOVERY
    SCB_InvalidateDCache_by_Addr(__xiptext_start__, (int32_t)(__xiptext_end__ - __xiptext_start__));
#endif
    SCB_InvalidateICache();
}

/* JEDEC ID (0x9F): manufacturer, memory type, capacity code. 1 on success. */
int flash_read_id(uint8_t id[3])
{
    uint32_t w = 0;
    if (!lut_init() || ip(SEQ_RDID, 0, kFLEXSPI_Read, &w, 3) != kStatus_Success) return 0;
    id[0] = (uint8_t)w;
    id[1] = (uint8_t)(w >> 8);
    id[2] = (uint8_t)(w >> 16);
    return 1;
}

/* The flash size port A1 is configured for (FLSHA1CR0, from the FCB): AHB
 * reads and IP commands past it do not reach this chip. */
uint32_t flash_window(void)
{
    return (FLEXSPI->FLSHCR0[0] & FLEXSPI_FLSHCR0_FLSHSZ_MASK) * 1024u;
}

uint32_t flash_chip_size(const uint8_t id[3])
{
    /* capacity code n = 2^n bytes (Winbond, GigaDevice, Macronix, ISSI, ...) */
    if (id[0] == 0x00u || id[0] == 0xFFu || id[2] < 0x10u || id[2] > 0x1Fu) return 0;
    return 1u << id[2];
}

uint32_t flash_capacity(void)
{
    static uint32_t cap;   /* 0: not read yet (or the read failed: try again) */
    if (cap == 0u) {
        uint8_t id[3];
        uint32_t chip = flash_read_id(id) ? flash_chip_size(id) : 0u;
        uint32_t win = flash_window();
        cap = chip < win ? chip : win;
    }
    return cap;
}

void fw_info(void)
{
    uint32_t pads = 0, bits = 0, sr = 0;
    int fcb = fcb_read_seq(&pads, &bits);
    int st = lut_init() && read_status(&sr);
    log_printf("fw: fcb=%s cmd-pads=%u addr=%u status=%s sr=%02x app 0x%08x rec 0x%08x stock 0x%08x\r\n",
               fcb ? "ok" : "BAD", (unsigned)pads, (unsigned)bits, st ? "ok" : "FAILED",
               (unsigned)sr, (unsigned)(FLASH_AHB + regions[FW_APP].base),
               (unsigned)(FLASH_AHB + regions[FW_RECOVERY].base),
               (unsigned)(FLASH_AHB + regions[FW_STOCK].base));
}

/* Non-destructive probe: write-enable must set WEL, then write-disable. */
void fw_test(void)
{
    uint32_t sr = 0;
    int ok = lut_init() && write_enable();
    read_status(&sr);
    log_printf("fw test %s (sr=%02x)\r\n", ok ? "ok" : "FAILED", (unsigned)sr);
    /* WEL stays set until the next erase/program; harmless. */
}

void fw_begin(fw_target_t target, uint32_t len, uint32_t crc)
{
    uint32_t base = regions[target].base;
    uint32_t limit = regions[target].limit;
    if (len == 0u || len > limit - base) {
        log_printf("fw: bad length %u (max %u)\r\n", (unsigned)len, (unsigned)(limit - base));
        return;
    }
    uint32_t sr;
    if (!lut_init() || !read_status(&sr)) { log_printf("fw: status read FAILED\r\n"); return; }
    if (sr & 0x3Cu) {   /* BP0..BP3: block protection would silently drop writes */
        log_printf("fw: flash is write-protected (sr=%02x); nothing erased\r\n", (unsigned)sr);
        return;
    }
    if (!write_enable()) { log_printf("fw: write-enable FAILED\r\n"); return; }
    uint32_t end = base + ((len + SECTOR - 1u) / SECTOR) * SECTOR;
    log_printf("fw: erasing 0x%08x..0x%08x\r\n", (unsigned)(FLASH_AHB + base),
               (unsigned)(FLASH_AHB + end));
    cdc_log_task();
#ifndef FB200_RECOVERY
    if (target == FW_APP) xip_gone = 1;   /* from here on: fw_session(), never return */
#endif
    for (uint32_t a = base; a < end; a += SECTOR) {
        wdog_feed();
        if (!write_enable() || ip(SEQ_ERASE, a, kFLEXSPI_Command, NULL, 0) != kStatus_Success ||
            !flash_wait_idle(2000u)) {
            log_printf("fw: erase FAILED at 0x%08x\r\n", (unsigned)a);
            flash_refresh(base, end - base);
#ifndef FB200_RECOVERY
            if (xip_gone) fw_session();
#endif
            return;
        }
        tud_task();
        cdc_log_task();
    }
    flash_refresh(base, end - base);
    fw_off = base;
    fw_len = len;
    fw_crc = crc;
    fw_rx = 0;
    page_fill = 0;
    last_rx_ms = tusb_time_millis_api();
    active = 1;
    log_printf("fw ready\r\n");
#ifndef FB200_RECOVERY
    if (xip_gone) fw_session();
#endif
}

#ifndef FB200_RECOVERY
/* The rest of an app update: the stream, then `reset`. Runs only RAM code
 * (hot_path.py): the main loop's cold tasks are gone with the old slot, and
 * the new slot's cold code belongs to the new app. The audio keeps running
 * (engine_task and usb_audio_task are ITCM code). HID reports are dropped
 * (usb_hid.c). */
/* Once the stream has ended (done or aborted), only `reset` leaves: the old
 * app's cold code is gone, so its UI is dead while the audio still runs. If
 * the host is not there to send it (USB unplugged for FW_GONE_MS, or no
 * console input for FW_IDLE_RESET_MS), reset anyway: the new app starts, or
 * recovery if the slot is invalid. The watchdog cannot do it: this loop
 * feeds it. */
#define FW_GONE_MS       5000u
#define FW_IDLE_RESET_MS 120000u

__attribute__((noreturn)) void fw_session(void)
{
    static char line[16];
    unsigned n = 0;
    uint32_t idle_since = 0, gone_since = 0;
    for (;;) {
        wdog_feed();
        tud_task();
        if (active) {
            fw_rx_task();
            idle_since = gone_since = 0;
        } else {
            uint32_t now = tusb_time_millis_api();
            if (!idle_since) idle_since = now;
            if (tud_mounted()) gone_since = 0;
            else if (!gone_since) gone_since = now;
            if ((gone_since && now - gone_since > FW_GONE_MS) || now - idle_since > FW_IDLE_RESET_MS) {
                log_printf("fw: no host after the update: rebooting\r\n");
                console_reboot();
            }
            if (tud_cdc_available()) idle_since = now;
            while (tud_cdc_available()) {
                char c = (char)tud_cdc_read_char();
                if (c != '\r' && c != '\n') {
                    if (n < sizeof line - 1u) line[n++] = c;
                    continue;
                }
                line[n] = 0;
                if ((n == 5u && memcmp(line, "reset", 5) == 0) || (n == 6u && memcmp(line, "reboot", 6) == 0)) {
                    log_printf("rebooting\r\n");
                    cdc_log_task();
                    console_reboot();
                }
                if (n) log_printf("fw: app update in progress: only `reset` works now\r\n");
                n = 0;
            }
        }
        cdc_log_task();
        usb_audio_task();
        engine_task();
    }
}
#endif

int fw_active(void) { return active; }

static int program_page(void)
{
    uint32_t off = fw_off + ((fw_rx - page_fill) / PAGE) * PAGE;
    if (page_fill < PAGE) memset((uint8_t *)page_buf + page_fill, 0xFF, PAGE - page_fill);
    page_fill = 0;
    if (!write_enable() ||
        ip(SEQ_PROGRAM, off, kFLEXSPI_Write, page_buf, PAGE) != kStatus_Success ||
        !flash_wait_idle(100u)) {
        log_printf("fw: program FAILED at 0x%08x\r\n", (unsigned)off);
        return 0;
    }
    return 1;
}

static void finish(void)
{
    active = 0;
    flash_refresh(fw_off, fw_len);
    uint32_t got = crc32_ieee((const uint8_t *)(FLASH_AHB + fw_off), fw_len);
    log_printf("fw done crc=%08x %s\r\n", (unsigned)got, got == fw_crc ? "ok" : "BAD");
}

void fw_rx_task(void)
{
    uint32_t now = tusb_time_millis_api();
    while (active && tud_cdc_available()) {
        uint32_t want = PAGE - page_fill;
        if (want > fw_len - fw_rx) want = fw_len - fw_rx;
        uint32_t n = tud_cdc_read((uint8_t *)page_buf + page_fill, want);
        if (n == 0u) break;
        page_fill += n;
        fw_rx += n;
        last_rx_ms = now;
        if (page_fill == PAGE || fw_rx == fw_len) {
            if (!program_page()) { active = 0; return; }
        }
        if (fw_rx == fw_len) { finish(); return; }
    }
    if (active && now - last_rx_ms > FW_IDLE_MS) {
        active = 0;
        log_printf("fw aborted at %u/%u bytes (the region is now invalid)\r\n",
                   (unsigned)fw_rx, (unsigned)fw_len);
    }
}

/* Data store: rewrite part of one 4 KB sector (flash_rmw.c) inside the
 * preset/settings/IR region F:0x71000..0xA1800: presets, settings, rhythm,
 * BT name, update flag, IR names/flags and the 9 user IR slots at 0x89000 +
 * slot * 0x2800 (docs/UI_AND_STORAGE.md §5, docs/PROTOCOL.md), or the long
 * IR store (irstore.h) when the chip holds it. Not during an update stream
 * or after an app update erased the cold code. 0 on success. */
static int store_blocked;
void flash_store_block(int on) { store_blocked = on; }

int flash_store(uint32_t offset, const void *data, uint32_t len)
{
    if (active || xip_gone) return -1;
    if (store_blocked) return -5;   /* battery critical: no erase at brown-out risk */
    return flash_rmw(offset, data, len);
}
