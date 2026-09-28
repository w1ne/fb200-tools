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
#define SCB_DCCMVAC  (*(volatile uint32_t *)0xE000EF68u)
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
    /* The slot data blob (F:0x41000, after the app slot): cold code to OCRAM
     * (linker.ld .ocramtext), large const tables to OCRAM and DTCM
     * (.ocramdata, .dtcmdata). After the writability probe above, which
     * touches the first word of the DTCM region. */
    extern uint32_t __ocramtext_start__[], __ocramtext_end__[], __ocramtext_load__[];
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
    const volatile uint32_t *from = (const volatile uint32_t *)__ocramtext_load__;
    for (volatile uint32_t *to = __ocramtext_start__; to < __ocramtext_end__; ) *to++ = *from++;
    from = (const volatile uint32_t *)__ocramdata_load__;
    for (volatile uint32_t *to = __ocramdata_start__; to < __ocramdata_end__; ) *to++ = *from++;
    from = (const volatile uint32_t *)__dtcmdata_load__;
    for (volatile uint32_t *to = __dtcmdata_start__; to < __dtcmdata_end__; ) *to++ = *from++;

    /* OCRAM must be executable. With the MPU off (BOARD_ConfigMPU is empty)
     * the default memory map applies: SRAM region, executable, write-back
     * cached. Should anything have left the MPU on, give OCRAM2 the highest
     * region (highest priority): normal memory, WBWA, full access, XN clear. */
    if (MPU_CTRL & 1u) {
        MPU_RNR = ((MPU_TYPE >> 8) & 0xFFu) - 1u;
        MPU_RBAR = 0x20200000u;
        MPU_RASR = (3u << 24) | (1u << 19) | (1u << 17) | (1u << 16) | (18u << 1) | 1u;   /* 512 kB */
        __asm volatile ("dsb 0xF\n isb 0xF" ::: "memory");
    }
    /* The code was written through the data side. Recovery hands over with
     * the D-cache off (recovery_launch_app), so it is already in memory; if
     * the D-cache is on anyway, clean the range. Then drop any stale
     * instruction lines and turn the I-cache on (OCRAM code runs from it). */
    if (SCB_CCR & (1u << 16)) {
        for (uint32_t a = (uint32_t)__ocramtext_start__; a < (uint32_t)__ocramtext_end__; a += 32u)
            SCB_DCCMVAC = a;
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
