/* Two-stage boot, crumbs and the watchdog. See recovery.h. */
#include <stdint.h>
#include <stddef.h>
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
/* Written by the app once it runs; every clean exit (reset, recovery
 * request, fault) overwrites it. Still present at boot = the app died without
 * a word: watchdog after a hang. (WDOG1 WRSR was tried: its timeout flag
 * survives later software resets and gave false positives.) */
#define CRUMB_ALIVE       0xA11FE000u

#define SRC_SRSR          (*(volatile uint32_t *)0x400F8008u)
#define SRSR_WDOG         (1u << 4)
#define WDOG1_WCR         (*(volatile uint16_t *)0x400B8000u)
#define WDOG1_WSR         (*(volatile uint16_t *)0x400B8002u)
#define WDOG1_WMCR        (*(volatile uint16_t *)0x400B8008u)
#define WDOG_TIMEOUT_HALF_S 15u       /* (15 + 1) * 0.5 s = 8 s */

#define COPIER_FLASH      0x6001F000u /* block 0 offset 0xF000 */
#define BSS_START         0x20018B44u /* the vendor loader's memset range */
#define BSS_LEN           0x358A4u

extern uint8_t __copier_start__[], __copier_end__[];

static uint32_t snap[4], boot_srsr;

void crumb_alive(void) { CRUMB[0] = CRUMB_ALIVE; }
void crumb_clear(void) { CRUMB[0] = 0; }

void wdog_feed(void)
{
    WDOG1_WSR = 0x5555u;
    WDOG1_WSR = 0xAAAAu;
}

#ifdef FB200_RECOVERY
static void wdog_start(void)
{
    WDOG1_WMCR = 0;   /* power-down counter off */
    WDOG1_WCR = (uint16_t)((WDOG_TIMEOUT_HALF_S << 8) | (1u << 5) | (1u << 4) | (1u << 2));
    wdog_feed();
}
#endif

void crumbs_print(void)
{
    log_printf("crumbs %08x %08x %08x %08x srsr %08x\r\n", (unsigned)snap[0],
               (unsigned)snap[1], (unsigned)snap[2], (unsigned)snap[3], (unsigned)boot_srsr);
    if ((snap[0] & 0xFF000000u) == CRUMB_FAULT)
        log_printf("  fault: IPSR=%u CFSR=%08x HFSR=%08x PC=%08x\r\n",
                   (unsigned)(snap[0] & 0x1FFu), (unsigned)snap[1], (unsigned)snap[2],
                   (unsigned)snap[3]);
}

static void snapshot_crumbs(void)
{
    for (int i = 0; i < 4; i++) { snap[i] = CRUMB[i]; CRUMB[i] = 0; }
}

/* Full crash dump in DTCM below our .bss (0x20018B44). Measured on the
 * pedal: 0x20000100..0x2000C000 and 0x20017000.. survive a warm reset;
 * OCRAM and 0x20012000 (next to the vendor bootloader's 0x2001xxxx
 * handshake slots) do not, and neither does anything from 0x20050000. */
#define DUMP_ADDR    0x20008000u
#define DUMP_MAGIC   0xC0A5D00Du
#define DUMP_STACK   32u
#define STACK_TOP    0x20058000u

typedef struct {
    uint32_t magic, ipsr, exc_return, sp;
    uint32_t r0, r1, r2, r3, r12, lr, pc, xpsr;
    uint32_t cfsr, hfsr, mmfar, bfar, afsr, count;
    uint32_t stack[DUMP_STACK];
    uint32_t crc;
} crash_dump_t;

#define DUMP ((volatile crash_dump_t *)DUMP_ADDR)

static uint32_t dump_crc(void)
{
    return fw_crc32((const uint8_t *)DUMP_ADDR, offsetof(crash_dump_t, crc));
}

/* Called from Default_Handler with the stacked frame and EXC_RETURN. */
__attribute__((noreturn, used)) void fault_record(const uint32_t *frame, uint32_t exc_return)
{
    uint32_t count = (DUMP->magic == DUMP_MAGIC && DUMP->crc == dump_crc()) ? DUMP->count + 1u : 1u;
    DUMP->magic = DUMP_MAGIC;
    DUMP->ipsr = __get_IPSR();
    DUMP->exc_return = exc_return;
    DUMP->sp = (uint32_t)frame;
    DUMP->r0 = frame[0]; DUMP->r1 = frame[1]; DUMP->r2 = frame[2]; DUMP->r3 = frame[3];
    DUMP->r12 = frame[4]; DUMP->lr = frame[5]; DUMP->pc = frame[6]; DUMP->xpsr = frame[7];
    DUMP->cfsr = SCB->CFSR; DUMP->hfsr = SCB->HFSR;
    DUMP->mmfar = SCB->MMFAR; DUMP->bfar = SCB->BFAR; DUMP->afsr = SCB->AFSR;
    DUMP->count = count;
    const uint32_t *st = frame + 8;   /* the caller's stack above the frame */
    for (uint32_t i = 0; i < DUMP_STACK; i++)
        DUMP->stack[i] = ((uint32_t)(st + i) < STACK_TOP) ? st[i] : 0xDEADBEEFu;
    DUMP->crc = dump_crc();
    SCB_CleanDCache();
    CRUMB[0] = CRUMB_FAULT | __get_IPSR();
    CRUMB[1] = SCB->CFSR;
    CRUMB[2] = SCB->HFSR;
    CRUMB[3] = frame[6];   /* stacked PC */
    NVIC_SystemReset();
}

void crashdump_print(void)
{
    if (DUMP->magic != DUMP_MAGIC || DUMP->crc != dump_crc()) {
        log_printf("no crash dump\r\n");
        return;
    }
    log_printf("crash #%u: IPSR=%u EXC_RETURN=%08x SP=%08x\r\n", (unsigned)DUMP->count,
               (unsigned)DUMP->ipsr, (unsigned)DUMP->exc_return, (unsigned)DUMP->sp);
    log_printf("  PC=%08x LR=%08x xPSR=%08x\r\n", (unsigned)DUMP->pc, (unsigned)DUMP->lr,
               (unsigned)DUMP->xpsr);
    log_printf("  R0=%08x R1=%08x R2=%08x R3=%08x R12=%08x\r\n", (unsigned)DUMP->r0,
               (unsigned)DUMP->r1, (unsigned)DUMP->r2, (unsigned)DUMP->r3, (unsigned)DUMP->r12);
    log_printf("  CFSR=%08x HFSR=%08x MMFAR=%08x BFAR=%08x AFSR=%08x\r\n",
               (unsigned)DUMP->cfsr, (unsigned)DUMP->hfsr, (unsigned)DUMP->mmfar,
               (unsigned)DUMP->bfar, (unsigned)DUMP->afsr);
    for (uint32_t i = 0; i < DUMP_STACK; i += 8) {
        log_printf("  [sp+%02x]", (unsigned)(0x20u + i * 4u));
        for (uint32_t j = 0; j < 8; j++) log_printf(" %08x", (unsigned)DUMP->stack[i + j]);
        log_printf("\r\n");
    }
}

void crashdump_clear(void) { DUMP->magic = 0; }

void recovery_request(void)
{
    CRUMB[0] = CRUMB_REQUEST;
    log_flush_ms(300);
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
    if (h->version >= 2u && (h->data_len > SLOT_DATA_MAX ||
                             fw_crc32((const uint8_t *)SLOT_DATA, h->data_len) != h->data_crc)) {
        *why = "app data CRC mismatch (update the app over USB)";
        return 0;
    }
    if (fw_crc32((const uint8_t *)SLOT_VECTORS, 0x400u + h->blob_len) != h->crc) {
        *why = "app slot CRC mismatch";
        return 0;
    }
    return 1;
}

#ifdef FB200_RECOVERY

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

#else /* !FB200_RECOVERY */

/* The app does not stage or launch slots; `boot` just restarts. */
void recovery_launch_app(int usb_up)
{
    (void)usb_up;
    NVIC_SystemReset();
    for (;;) {
    }
}

#endif /* FB200_RECOVERY */

const char *recovery_boot(void)
{
    const char *why = 0;
    snapshot_crumbs();
    uint32_t srsr = SRC_SRSR;
    boot_srsr = srsr;
    SRC_SRSR = srsr;   /* W1C: make the next reset's cause readable */
    if ((snap[0] & 0xFF000000u) == CRUMB_FAULT) return "the app faulted (see crumbs)";
    if (snap[0] == CRUMB_REQUEST) return "requested by the app";
    if (snap[0] == CRUMB_ALIVE || (srsr & SRSR_WDOG)) return "the app stopped without a clean reset (hang/watchdog)";
    if (!slot_valid(&why)) return why;
    recovery_launch_app(0);
}
