/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

/* C body of the vendor-loader entry (see src/stage2.S for the 0x4d6 stub).
 * The loader has already copied the blob to ITCM 0x400 and (in the stock
 * flow) configured FlexRAM; we mirror the stock stub defensively, install
 * the vector table, zero .bss and run app_main.
 * See docs/FIRMWARE_BRINGUP.md. */
#include <stdint.h>

extern uint32_t __bss_start__[], __bss_end__[];
void app_main(void);

#define GPR(n) (*(volatile uint32_t *)(0x400AC000u + (n)))

__attribute__((used, noreturn))
void stage2_main(void)
{
    __asm volatile ("cpsid i" ::: "memory");

    GPR(0x38) = 0x00AA0000u;   /* IOMUXC_GPR14 */
    GPR(0x40) = 0x00200007u;   /* IOMUXC_GPR16: TCM control */
    GPR(0x44) = 0xFFAAAAA9u;   /* IOMUXC_GPR17: FlexRAM banks */

    *(volatile uint32_t *)0xE000ED88u |= (3u << 20) | (3u << 22);   /* CPACR: FPU on */
    __asm volatile ("dsb 0xF" ::: "memory");

    uint32_t sp = *(volatile uint32_t *)0x60010000u;   /* image[0] */
    __asm volatile ("msr msp, %0" :: "r" (sp) : "memory");

    /* Copy the vector table (block 0 offsets 0..0x400) to ITCM 0x0. */
    volatile uint32_t *dst = (volatile uint32_t *)0x0u;
    const volatile uint32_t *src = (const volatile uint32_t *)0x60010000u;
    for (unsigned i = 0; i < 256u; i++) {
        dst[i] = src[i];
    }

    for (volatile uint32_t *b = __bss_start__; b < __bss_end__; ) {
        *b++ = 0;
    }

    *(volatile uint32_t *)0xE000ED08u = 0;   /* SCB->VTOR = ITCM base */
    __asm volatile ("dsb 0xF" ::: "memory");
    __asm volatile ("isb 0xF" ::: "memory");
    __asm volatile ("cpsie i" ::: "memory");
    app_main();
    __builtin_unreachable();
}
