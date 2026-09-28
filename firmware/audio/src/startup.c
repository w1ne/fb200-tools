/* C body of the vendor-loader entry (see src/stage2.S for the 0x4d6 stub).
 * The loader has already copied the blob to ITCM 0x400 and (in the stock
 * flow) configured FlexRAM; we mirror the stock stub defensively, install
 * the vector table, zero .bss and run app_main.
 * See docs/FIRMWARE_BRINGUP.md. */
#include <stdint.h>

void app_main(void);

int g_bss_writable = 1;

#define GPR(n) (*(volatile uint32_t *)(0x400AC000u + (n)))

#ifndef FB200_RECOVERY
#include "debug/recovery.h"
#define CRUMB        ((volatile uint32_t *)0x400F8028u)   /* SRC_GPR3.., recovery.c */
#define SCB_CCR      (*(volatile uint32_t *)0xE000ED14u)
#define SCB_ICIALLU  (*(volatile uint32_t *)0xE000EF50u)
#define MPU_TYPE     (*(volatile uint32_t *)0xE000ED90u)
#define MPU_CTRL     (*(volatile uint32_t *)0xE000ED94u)
#define MPU_RNR      (*(volatile uint32_t *)0xE000ED98u)
#define MPU_RBAR     (*(volatile uint32_t *)0xE000ED9Cu)
#define MPU_RASR     (*(volatile uint32_t *)0xE000EDA0u)
#endif

/* Where this image's vector table sits in flash: block 0 for recovery (the
 * vendor loader boots it), the app slot for the app (recovery's copier has
 * already put it in ITCM; copying again is harmless). */
#ifdef FB200_RECOVERY
#define VEC_SRC 0x60010000u
#else
#define VEC_SRC 0x60020100u   /* SLOT_VECTORS, src/debug/recovery.h */
#endif

__attribute__((used, noreturn))
void stage2_main(void)
{
    __asm volatile ("cpsid i" ::: "memory");

    GPR(0x38) = 0x00AA0000u;   /* IOMUXC_GPR14 */
    GPR(0x40) = 0x00200007u;   /* IOMUXC_GPR16: TCM control */
    GPR(0x44) = 0xFFAAAAA9u;   /* IOMUXC_GPR17: FlexRAM banks */

    *(volatile uint32_t *)0xE000ED88u |= (3u << 20) | (3u << 22);   /* CPACR: FPU on */
    __asm volatile ("dsb 0xF" ::: "memory");

    uint32_t sp = *(volatile uint32_t *)VEC_SRC;   /* image[0] */
    __asm volatile ("msr msp, %0" :: "r" (sp) : "memory");

    /* Copy the vector table to ITCM 0x0. */
    volatile uint32_t *dst = (volatile uint32_t *)0x0u;
    const volatile uint32_t *src = (const volatile uint32_t *)VEC_SRC;
    for (unsigned i = 0; i < 256u; i++) {
        dst[i] = src[i];
    }

    /* The vendor loader already zeroed .bss (its entry 4 memsets the stock
     * region 0x20018B44..0x2004E3E8). Safety by construction: verify the
     * range is writable and report it; the FlexRAM bank split behind
     * IOMUXC_GPR17 is not verified. */
    volatile uint32_t *bss = (volatile uint32_t *)0x20018B44u;
    bss[0] = 0xA5A5A5A5u;
    bss[0x358A4 / 4 - 1] = 0x5A5A5A5Au;
    g_bss_writable = (bss[0] == 0xA5A5A5A5u && bss[0x358A4 / 4 - 1] == 0x5A5A5A5Au);
    bss[0] = 0;
    bss[0x358A4 / 4 - 1] = 0;

#ifndef FB200_RECOVERY
    /* The slot data blob (F:0x41000, after the app slot): cold code that
     * runs in place from flash (linker.ld .xiptext), then large const tables
     * copied to OCRAM and DTCM (.ocramdata, .dtcmdata). After the
     * writability probe above, which touches the first word of the DTCM
     * region. */
    extern uint32_t __dtcmdata_start__[], __dtcmdata_end__[], __dtcmdata_load__[];
    extern uint32_t __ocramdata_start__[], __ocramdata_end__[], __ocramdata_load__[];
    /* Recovery checked the CRC of data_len bytes (slot_valid). Code must
     * never run from bytes it did not check: a slot whose data is shorter
     * than this build's blob goes back to recovery (fault crumb, recovery.c
     * stays on the console). */
    const volatile slot_header_t *hdr = (const volatile slot_header_t *)SLOT_FLASH;
    uint32_t need = (uint32_t)__dtcmdata_load__ + (uint32_t)(__dtcmdata_end__ - __dtcmdata_start__) * 4u -
                    SLOT_DATA;
    if (hdr->version < 2u || hdr->data_len < need) {
        CRUMB[1] = hdr->data_len;
        CRUMB[2] = need;
        CRUMB[3] = SLOT_DATA;
        CRUMB[0] = 0xFA000000u;   /* CRUMB_FAULT, IPSR 0 */
        __asm volatile ("dsb 0xF" ::: "memory");
        *(volatile uint32_t *)0xE000ED0Cu = 0x05FA0004u;   /* AIRCR SYSRESETREQ */
        for (;;) {
        }
    }
    const volatile uint32_t *from = (const volatile uint32_t *)__ocramdata_load__;
    for (volatile uint32_t *to = __ocramdata_start__; to < __ocramdata_end__; ) *to++ = *from++;
    from = (const volatile uint32_t *)__dtcmdata_load__;
    for (volatile uint32_t *to = __dtcmdata_start__; to < __dtcmdata_end__; ) *to++ = *from++;

    /* The NOLOAD regions outside the copier's .bss memset (linker.ld): low
     * DTCM, DTCM above .bss, ITCM above the code (the copier at 0x1F000 is
     * done), OCRAM. */
    extern uint32_t __dtcm_lo_start__[], __dtcm_lo_end__[], __dtcm_hi_start__[], __dtcm_hi_end__[];
    extern uint32_t __itcm_bss_start__[], __itcm_bss_end__[], __ocram_start__[], __ocram_end__[];
    for (volatile uint32_t *p = __dtcm_lo_start__; p < __dtcm_lo_end__; ) *p++ = 0;
    for (volatile uint32_t *p = __dtcm_hi_start__; p < __dtcm_hi_end__; ) *p++ = 0;
    for (volatile uint32_t *p = __itcm_bss_start__; p < __itcm_bss_end__; ) *p++ = 0;
    for (volatile uint32_t *p = __ocram_start__; p < __ocram_end__; ) *p++ = 0;

    /* Cold code runs from flash (XIP). With the MPU off (measured:
     * MPU_CTRL = 0; BOARD_ConfigMPU is empty) the default memory map makes
     * 0x60000000.. Normal, write-back cached and executable. Should anything
     * have left the MPU on, give the flash window the highest region (highest
     * priority): normal memory, WBWA, read-only, XN clear (8 MB). Drop any
     * stale instruction lines (the slot may have just been rewritten) and
     * turn the I-cache on: XIP code runs from it. */
    if (MPU_CTRL & 1u) {
        MPU_RNR = ((MPU_TYPE >> 8) & 0xFFu) - 1u;
        MPU_RBAR = 0x60000000u;
        MPU_RASR = (6u << 24) | (1u << 19) | (1u << 17) | (1u << 16) | (22u << 1) | 1u;   /* RO, 8 MB */
        __asm volatile ("dsb 0xF\n isb 0xF" ::: "memory");
    }
    __asm volatile ("dsb 0xF" ::: "memory");
    SCB_ICIALLU = 0;
    __asm volatile ("dsb 0xF\n isb 0xF" ::: "memory");
    SCB_CCR |= 1u << 17;   /* IC */
    __asm volatile ("dsb 0xF\n isb 0xF" ::: "memory");
#endif

    *(volatile uint32_t *)0xE000ED08u = 0;   /* SCB->VTOR = ITCM base */
    __asm volatile ("dsb 0xF" ::: "memory");
    __asm volatile ("isb 0xF" ::: "memory");
    __asm volatile ("cpsie i" ::: "memory");
    app_main();
    __builtin_unreachable();
}
