/* USB self-update: see selfupdate.h.
 *
 * The flash controller stays exactly as the boot ROM and the vendor
 * bootloader configured it, so memory-mapped (AHB) reads keep working. We only
 * add four plain SPI-NOR sequences in LUT slots 12..15 and run them as IP
 * commands. (The ROM FlexSPI driver's init was tried first: after it, any AHB
 * read of flash, including a speculative one, hangs the core. Seen on
 * hardware 2026-09-27.)
 *
 * All code runs from ITCM and all data lives in DTCM, so flash can be erased
 * and programmed while the application runs. */
#include <stdint.h>
#include <string.h>
#include "fsl_device_registers.h"
#include "fsl_flexspi.h"
#include "tusb.h"
#include "cdc_log.h"
#include "selfupdate.h"
#include "recovery.h"

#define FLASH_AHB    0x60000000u
/* Regions (flash offsets, docs/BOOTLOADER.md §4): the app slot, and the
 * recovery image (block 0 start, which also holds the vendor loader). */
#define APP_OFFSET   0x00020000u
#define APP_LIMIT    0x00041000u   /* model library starts here */
#define REC_OFFSET   0x00010000u
#define REC_LIMIT    0x00020000u
#define FCB_TAG      0x42464346u   /* "FCFB" */
#define FCB_LUT      0x80u         /* FCB offset of the lookup table */
#define SECTOR       4096u
#define PAGE         256u
#define FW_IDLE_MS   3000u

#define SEQ_WREN     12u
#define SEQ_RDSR     13u
#define SEQ_ERASE    14u
#define SEQ_PROGRAM  15u

extern uint32_t tusb_time_millis_api(void);

static uint32_t page_buf[PAGE / 4];
static uint32_t fw_off, fw_len, fw_crc, fw_rx, page_fill, last_rx_ms;
static int active;
static int lut_ready;

uint32_t fw_crc32(const uint8_t *p, uint32_t len)
{
    uint32_t c = 0xFFFFFFFFu;
    while (len--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

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
    /* 4-byte-address opcodes (0x21/0x12) work in either address mode. */
    uint8_t se = bits == 32u ? 0x21 : 0x20;
    uint8_t pp = bits == 32u ? 0x12 : 0x02;
    const uint32_t lut[16] = {
        [0]  = FLEXSPI_LUT_SEQ(kFLEXSPI_Command_SDR, kFLEXSPI_1PAD, 0x06,
                               kFLEXSPI_Command_STOP, kFLEXSPI_1PAD, 0),
        [4]  = FLEXSPI_LUT_SEQ(kFLEXSPI_Command_SDR, kFLEXSPI_1PAD, 0x05,
                               kFLEXSPI_Command_READ_SDR, kFLEXSPI_1PAD, 0x04),
        [8]  = FLEXSPI_LUT_SEQ(kFLEXSPI_Command_SDR, kFLEXSPI_1PAD, se,
                               kFLEXSPI_Command_RADDR_SDR, kFLEXSPI_1PAD, bits),
        [12] = FLEXSPI_LUT_SEQ(kFLEXSPI_Command_SDR, kFLEXSPI_1PAD, pp,
                               kFLEXSPI_Command_RADDR_SDR, kFLEXSPI_1PAD, bits),
        [13] = FLEXSPI_LUT_SEQ(kFLEXSPI_Command_WRITE_SDR, kFLEXSPI_1PAD, 0x04,
                               kFLEXSPI_Command_STOP, kFLEXSPI_1PAD, 0),
    };
    FLEXSPI_UpdateLUT(FLEXSPI, SEQ_WREN * 4u, lut, 16u);
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

static int wait_idle(uint32_t timeout_ms)
{
    uint32_t t0 = tusb_time_millis_api(), sr;
    do {
        if (!read_status(&sr)) return 0;
        if (!(sr & 0x01u)) return 1;           /* WIP clear */
    } while (tusb_time_millis_api() - t0 < timeout_ms);
    return 0;
}

/* Drop stale AHB prefetch/cache lines so reads see the new contents. */
static void refresh_ahb(uint32_t offset, uint32_t len)
{
    FLEXSPI->MCR0 |= FLEXSPI_MCR0_SWRESET_MASK;
    while (FLEXSPI->MCR0 & FLEXSPI_MCR0_SWRESET_MASK) {
    }
    SCB_InvalidateDCache_by_Addr((void *)(FLASH_AHB + offset), (int32_t)len);
}

void fw_info(void)
{
    uint32_t pads = 0, bits = 0, sr = 0;
    int fcb = fcb_read_seq(&pads, &bits);
    int st = lut_init() && read_status(&sr);
    log_printf("fw: fcb=%s cmd-pads=%u addr=%u status=%s sr=%02x app 0x%08x rec 0x%08x\r\n",
               fcb ? "ok" : "BAD", (unsigned)pads, (unsigned)bits, st ? "ok" : "FAILED",
               (unsigned)sr, (unsigned)(FLASH_AHB + APP_OFFSET), (unsigned)(FLASH_AHB + REC_OFFSET));
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

void fw_begin(int recovery, uint32_t len, uint32_t crc)
{
    uint32_t base = recovery ? REC_OFFSET : APP_OFFSET;
    uint32_t limit = recovery ? REC_LIMIT : APP_LIMIT;
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
    for (uint32_t a = base; a < end; a += SECTOR) {
        wdog_feed();
        if (!write_enable() || ip(SEQ_ERASE, a, kFLEXSPI_Command, NULL, 0) != kStatus_Success ||
            !wait_idle(2000u)) {
            log_printf("fw: erase FAILED at 0x%08x\r\n", (unsigned)a);
            return;
        }
        tud_task();
        cdc_log_task();
    }
    refresh_ahb(base, end - base);
    fw_off = base;
    fw_len = len;
    fw_crc = crc;
    fw_rx = 0;
    page_fill = 0;
    last_rx_ms = tusb_time_millis_api();
    active = 1;
    log_printf("fw ready\r\n");
}

int fw_active(void) { return active; }

static int program_page(void)
{
    uint32_t off = fw_off + ((fw_rx - page_fill) / PAGE) * PAGE;
    if (page_fill < PAGE) memset((uint8_t *)page_buf + page_fill, 0xFF, PAGE - page_fill);
    page_fill = 0;
    if (!write_enable() ||
        ip(SEQ_PROGRAM, off, kFLEXSPI_Write, page_buf, PAGE) != kStatus_Success ||
        !wait_idle(100u)) {
        log_printf("fw: program FAILED at 0x%08x\r\n", (unsigned)off);
        return 0;
    }
    return 1;
}

static void finish(void)
{
    active = 0;
    refresh_ahb(fw_off, fw_len);
    uint32_t got = fw_crc32((const uint8_t *)(FLASH_AHB + fw_off), fw_len);
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
        log_printf("fw aborted at %u/%u bytes (app region is now invalid)\r\n",
                   (unsigned)fw_rx, (unsigned)fw_len);
    }
}

/* Data store: rewrite part of one 4 KB sector (read-modify-write, as the
 * stock does) inside the preset/settings region F:0x71000..0x89000
 * (docs/UI_AND_STORAGE.md §5). Verifies by reading back. 0 on success. */
#define STORE_BASE  0x00071000u
#define STORE_LIMIT 0x00089000u
static uint32_t sector_buf[SECTOR / 4];

int flash_store(uint32_t offset, const void *data, uint32_t len)
{
    uint32_t sector = offset & ~(SECTOR - 1u);
    if (offset < STORE_BASE || offset + len > STORE_LIMIT || len == 0u ||
        offset + len > sector + SECTOR || active) {
        return -1;
    }
    memcpy(sector_buf, (const void *)(FLASH_AHB + sector), SECTOR);
    memcpy((uint8_t *)sector_buf + (offset - sector), data, len);
    if (!lut_init() || !write_enable() ||
        ip(SEQ_ERASE, sector, kFLEXSPI_Command, NULL, 0) != kStatus_Success || !wait_idle(2000u)) {
        return -2;
    }
    for (uint32_t p = 0; p < SECTOR; p += PAGE) {
        if (!write_enable() ||
            ip(SEQ_PROGRAM, sector + p, kFLEXSPI_Write, sector_buf + p / 4u, PAGE) != kStatus_Success ||
            !wait_idle(100u)) {
            return -3;
        }
    }
    refresh_ahb(sector, SECTOR);
    return memcmp((const void *)(FLASH_AHB + offset), data, len) == 0 ? 0 : -4;
}
