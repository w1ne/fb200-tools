/* C body of the vendor-loader entry (see src/stage2.S for the 0x4d6 stub).
 * The loader has already copied the blob to ITCM 0x400 and (in the stock
 * flow) configured FlexRAM; we mirror the stock stub defensively, install
 * the vector table, zero .bss and run app_main.
 * See docs/FIRMWARE_BRINGUP.md. */
#include <stdint.h>

void app_main(void);

int g_bss_writable = 1;

#define GPR(n) (*(volatile uint32_t *)(0x400AC000u + (n)))

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
    /* Large const tables: flash (F:0x41000, after the app slot) -> DTCM
     * (linker.ld .dtcmdata). After the writability probe above, which touches
     * the first word of this region. */
    extern uint32_t __dtcmdata_start__[], __dtcmdata_end__[];
    const volatile uint32_t *from = (const volatile uint32_t *)0x60041000u;
    for (volatile uint32_t *to = __dtcmdata_start__; to < __dtcmdata_end__; ) *to++ = *from++;
#endif

    *(volatile uint32_t *)0xE000ED08u = 0;   /* SCB->VTOR = ITCM base */
    __asm volatile ("dsb 0xF" ::: "memory");
    __asm volatile ("isb 0xF" ::: "memory");
    __asm volatile ("cpsie i" ::: "memory");
    app_main();
    __builtin_unreachable();
}
