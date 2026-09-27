/* USB self-update through the RT1062 ROM FlexSPI NOR driver. See
 * selfupdate.h. All code runs from ITCM and all data lives in DTCM, so the
 * flash can be erased and programmed while the application runs. */
#include <stdint.h>
#include <string.h>
#include "fsl_device_registers.h"
#include "fsl_romapi.h"
#include "tusb.h"
#include "cdc_log.h"
#include "selfupdate.h"

#define FLEXSPI_INSTANCE 0u
#define FLASH_AHB        0x60000000u
#define FW_OFFSET        0x00010000u   /* block 0, see docs/BOOTLOADER.md */
#define FW_LIMIT         0x00041000u   /* model library starts here */
#define FCB_TAG          0x42464346u   /* "FCFB" */
#define FW_IDLE_MS       3000u
#define PAGE_MAX         512u

extern uint32_t tusb_time_millis_api(void);

static flexspi_nor_config_t nor;
static uint32_t page_buf[PAGE_MAX / 4];
static uint32_t page_size;
static uint32_t fw_len, fw_crc, fw_rx, page_fill, last_rx_ms;
static int active;

uint32_t fw_crc32(const uint8_t *p, uint32_t len)
{
    uint32_t c = 0xFFFFFFFFu;
    while (len--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static int rom_init(void)
{
    if (*(volatile uint32_t *)FLASH_AHB != FCB_TAG) {
        log_printf("fw: no FCB at 0x60000000\r\n");
        return 0;
    }
    memcpy(&nor, (const void *)FLASH_AHB, sizeof nor);
    __disable_irq();
    status_t st = ROM_FLEXSPI_NorFlash_Init(FLEXSPI_INSTANCE, &nor);
    __enable_irq();
    page_size = nor.pageSize;
    if (st != kStatus_Success || page_size == 0u || page_size > PAGE_MAX ||
        nor.sectorSize == 0u) {
        log_printf("fw: ROM init status=%d page=%u sector=%u\r\n", (int)st,
                   (unsigned)nor.pageSize, (unsigned)nor.sectorSize);
        return 0;
    }
    return 1;
}

static void flush_ahb(uint32_t offset, uint32_t len)
{
    ROM_FLEXSPI_NorFlash_ClearCache(FLEXSPI_INSTANCE);
    SCB_InvalidateDCache_by_Addr((void *)(FLASH_AHB + offset), (int32_t)len);
}

void fw_info(void)
{
    if (!rom_init()) return;
    log_printf("fw: page=%u sector=%u block=%u region 0x%08x..0x%08x\r\n",
               (unsigned)nor.pageSize, (unsigned)nor.sectorSize,
               (unsigned)nor.blockSize, (unsigned)(FLASH_AHB + FW_OFFSET),
               (unsigned)(FLASH_AHB + FW_LIMIT));
}

void fw_begin(uint32_t len, uint32_t crc)
{
    if (len == 0u || len > FW_LIMIT - FW_OFFSET) {
        log_printf("fw: bad length %u (max %u)\r\n", (unsigned)len,
                   (unsigned)(FW_LIMIT - FW_OFFSET));
        return;
    }
    if (!rom_init()) return;
    uint32_t sector = nor.sectorSize;
    uint32_t end = FW_OFFSET + ((len + sector - 1u) / sector) * sector;
    if (end > FW_LIMIT) { log_printf("fw: erase exceeds region\r\n"); return; }
    log_printf("fw: erasing 0x%08x..0x%08x\r\n", (unsigned)(FLASH_AHB + FW_OFFSET),
               (unsigned)(FLASH_AHB + end));
    cdc_log_task();
    for (uint32_t a = FW_OFFSET; a < end; a += sector) {
        __disable_irq();
        status_t st = ROM_FLEXSPI_NorFlash_Erase(FLEXSPI_INSTANCE, &nor, a, sector);
        __enable_irq();
        if (st != kStatus_Success) {
            log_printf("fw: erase FAILED at 0x%08x status=%d\r\n", (unsigned)a, (int)st);
            return;
        }
        tud_task();
        cdc_log_task();
    }
    flush_ahb(FW_OFFSET, end - FW_OFFSET);
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
    uint32_t page_off = FW_OFFSET + ((fw_rx - page_fill) / page_size) * page_size;
    if (page_fill < page_size)
        memset((uint8_t *)page_buf + page_fill, 0xFF, page_size - page_fill);
    __disable_irq();
    status_t st = ROM_FLEXSPI_NorFlash_ProgramPage(FLEXSPI_INSTANCE, &nor, page_off, page_buf);
    __enable_irq();
    page_fill = 0;
    if (st != kStatus_Success) {
        log_printf("fw: program FAILED at 0x%08x status=%d\r\n", (unsigned)page_off, (int)st);
        return 0;
    }
    return 1;
}

static void finish(void)
{
    active = 0;
    flush_ahb(FW_OFFSET, fw_len);
    uint32_t got = fw_crc32((const uint8_t *)(FLASH_AHB + FW_OFFSET), fw_len);
    log_printf("fw done crc=%08x %s\r\n", (unsigned)got, got == fw_crc ? "ok" : "BAD");
}

void fw_rx_task(void)
{
    uint32_t now = tusb_time_millis_api();
    while (active && tud_cdc_available()) {
        uint32_t want = page_size - page_fill;
        if (want > fw_len - fw_rx) want = fw_len - fw_rx;
        uint32_t n = tud_cdc_read((uint8_t *)page_buf + page_fill, want);
        if (n == 0u) break;
        page_fill += n;
        fw_rx += n;
        last_rx_ms = now;
        if (page_fill == page_size || fw_rx == fw_len) {
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
