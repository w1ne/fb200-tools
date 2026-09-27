/* Two-stage boot, crumbs and the watchdog. See recovery.h. */
#include <stdint.h>
#include "fsl_device_registers.h"
#include "tusb.h"
#include "cdc_log.h"
#include "selfupdate.h"
#include "recovery.h"

/* SRC_GPR3..6 survive a warm reset (not power-on), so they carry what
 * happened across the reboot. [0] = what, [1..3] = details. (SNVS LPGPR was
 * tried first: on this board it ignores writes.) */
#define CRUMB             ((volatile uint32_t *)0x400F8028u)
#define CRUMB_FAULT       0xFA000000u
#define CRUMB_REQUEST     0x5EC0FEEDu

#define SRC_SRSR          (*(volatile uint32_t *)0x400F8008u)
#define SRSR_WDOG         (1u << 4)
#define WDOG1_WCR         (*(volatile uint16_t *)0x400B8000u)
#define WDOG1_WSR         (*(volatile uint16_t *)0x400B8002u)
#define WDOG1_WRSR        (*(volatile uint16_t *)0x400B8004u)
#define WDOG1_WMCR        (*(volatile uint16_t *)0x400B8008u)
#define WDOG_TIMEOUT_HALF_S 15u       /* (15 + 1) * 0.5 s = 8 s */

#define COPIER_FLASH      0x6001F000u /* block 0 offset 0xF000 */
#define BSS_START         0x20018B44u /* the vendor loader's memset range */
#define BSS_LEN           0x358A4u

extern uint8_t __copier_start__[], __copier_end__[];

static uint32_t snap[4];

void wdog_feed(void)
{
    WDOG1_WSR = 0x5555u;
    WDOG1_WSR = 0xAAAAu;
}

static void wdog_start(void)
{
    WDOG1_WMCR = 0;   /* power-down counter off */
    WDOG1_WCR = (uint16_t)((WDOG_TIMEOUT_HALF_S << 8) | (1u << 5) | (1u << 4) | (1u << 2));
    wdog_feed();
}

void crumbs_print(void)
{
    log_printf("crumbs %08x %08x %08x %08x\r\n", (unsigned)snap[0], (unsigned)snap[1],
               (unsigned)snap[2], (unsigned)snap[3]);
    if ((snap[0] & 0xFF000000u) == CRUMB_FAULT)
        log_printf("  fault: IPSR=%u CFSR=%08x HFSR=%08x PC=%08x\r\n",
                   (unsigned)(snap[0] & 0x1FFu), (unsigned)snap[1], (unsigned)snap[2],
                   (unsigned)snap[3]);
}

static void snapshot_crumbs(void)
{
    for (int i = 0; i < 4; i++) { snap[i] = CRUMB[i]; CRUMB[i] = 0; }
}

/* Called from Default_Handler with the stacked exception frame. */
__attribute__((noreturn, used)) void fault_record(const uint32_t *frame)
{
    CRUMB[0] = CRUMB_FAULT | __get_IPSR();
    CRUMB[1] = SCB->CFSR;
    CRUMB[2] = SCB->HFSR;
    CRUMB[3] = frame[6];   /* stacked PC */
    NVIC_SystemReset();
}

void recovery_request(void)
{
    CRUMB[0] = CRUMB_REQUEST;
    tud_disconnect();
    for (volatile uint32_t i = 0; i < 4000000u; i++) {
    }
    NVIC_SystemReset();
}

int slot_valid(const char **why)
{
    const slot_header_t *h = (const slot_header_t *)SLOT_FLASH;
    if (h->magic != SLOT_MAGIC) { *why = "no app in the slot"; return 0; }
    if (h->blob_len == 0u || h->blob_len > APP_ITCM_LIMIT - 0x400u) {
        *why = "app slot length invalid";
        return 0;
    }
    if (fw_crc32((const uint8_t *)SLOT_VECTORS, 0x400u + h->blob_len) != h->crc) {
        *why = "app slot CRC mismatch";
        return 0;
    }
    return 1;
}

/* Runs from ITCM 0x1F000, above anything the app occupies. It overwrites the
 * recovery image in ITCM with the app, so it must not call out of .copier;
 * volatile accesses keep the compiler from emitting memcpy/memset calls. */
__attribute__((section(".copier"), noreturn, noinline, used))
static void copier(uint32_t blob_len)
{
    uintptr_t itcm;
    __asm volatile ("mov %0, #0" : "=r" (itcm));   /* ITCM base 0x0, laundered */
    volatile uint32_t *dst = (volatile uint32_t *)itcm;
    const volatile uint32_t *src = (const volatile uint32_t *)SLOT_VECTORS;
    for (uint32_t i = 0; i < 0x100u; i++) dst[i] = src[i];
    dst = (volatile uint32_t *)(itcm + 0x400u);
    src = (const volatile uint32_t *)SLOT_BLOB;
    for (uint32_t i = 0; i < (blob_len + 3u) / 4u; i++) dst[i] = src[i];
    volatile uint32_t *bss = (volatile uint32_t *)BSS_START;
    for (uint32_t i = 0; i < BSS_LEN / 4u; i++) bss[i] = 0;
    __asm volatile ("dsb 0xF\n isb 0xF" ::: "memory");
    uint32_t sp = ((const volatile uint32_t *)SLOT_VECTORS)[0];
    __asm volatile (
        "msr msp, %0\n"
        "bx %1\n" :: "r" (sp), "r" (0x4D7u) : "memory");   /* app stage2, Thumb */
    for (;;) {
    }
}

void recovery_launch_app(int usb_up)
{
    const char *why;
    if (!slot_valid(&why)) {   /* callers check; never jump into a bad slot */
        log_printf("boot refused: %s\r\n", why);
        for (;;) {
        }
    }
    uint32_t blob_len = ((const slot_header_t *)SLOT_FLASH)->blob_len;
    __disable_irq();
    SysTick->CTRL = 0;
    for (uint32_t i = 0; i < 8u; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFu;
        NVIC->ICPR[i] = 0xFFFFFFFFu;
    }
    /* Only touch USB if we started it: at boot its clock may be gated, and a
     * read of a gated peripheral stalls the bus forever (the first hardware
     * run of this image hung here, before the watchdog was armed). */
    if (usb_up) {
        USB1->USBCMD &= ~USBHS_USBCMD_RS_MASK;
        for (volatile uint32_t i = 0; i < 4000000u; i++) {
        }
        USB1->USBCMD |= USBHS_USBCMD_RST_MASK;
        while (USB1->USBCMD & USBHS_USBCMD_RST_MASK) {
        }
    }
    SCB_DisableDCache();
    /* Stage the copier into ITCM (the vendor loader only copies up to ITCM
     * 0x1E39C), then start the watchdog: the app must feed it from here on. */
    volatile uint32_t *cd = (volatile uint32_t *)(uintptr_t)__copier_start__;
    const volatile uint32_t *cs = (const volatile uint32_t *)COPIER_FLASH;
    uint32_t n = ((uint32_t)(__copier_end__ - __copier_start__) + 3u) / 4u;
    for (uint32_t i = 0; i < n; i++) cd[i] = cs[i];
    __asm volatile ("dsb 0xF\n isb 0xF" ::: "memory");
    wdog_start();
    copier(blob_len);
}

const char *recovery_boot(void)
{
    const char *why = 0;
    snapshot_crumbs();
    uint32_t srsr = SRC_SRSR;
    SRC_SRSR = srsr;   /* W1C: make the next reset's cause readable */
    if ((snap[0] & 0xFF000000u) == CRUMB_FAULT) return "the app faulted (see crumbs)";
    if (snap[0] == CRUMB_REQUEST) return "requested by the app";
    if ((srsr & SRSR_WDOG) || (WDOG1_WRSR & 0x2u)) return "watchdog reset (app hung)";
    if (!slot_valid(&why)) return why;
    recovery_launch_app(0);
}
