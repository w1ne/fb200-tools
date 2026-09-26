/* Flash-resident reset stub: mirrors the stock startup (docs/HARDWARE.md 3.1).
 * Configures FlexRAM/TCM, copies the image to ITCM, sets VTOR = 0 and jumps.
 */
#include <stdint.h>

extern uint32_t __itcm_start__[], __itcm_end__[], __itcm_lma__[];
extern uint32_t __data_start__[], __data_end__[], __data_lma__[];
extern uint32_t __bss_start__[], __bss_end__[];
void app_main(void);

#define GPR(n) (*(volatile uint32_t *)(0x400AC000u + (n)))

__attribute__((section(".boot_stub"), used, noreturn))
void reset_stub(void)
{
    __asm volatile ("cpsid i" ::: "memory");

    GPR(0x38) = 0x00AA0000u;   /* IOMUXC_GPR14 */
    GPR(0x40) = 0x00200007u;   /* IOMUXC_GPR16: TCM control */
    GPR(0x44) = 0xFFAAAAA9u;   /* IOMUXC_GPR17: FlexRAM banks */

    uint32_t sp = *(volatile uint32_t *)0x60010000u;
    __asm volatile ("msr msp, %0" :: "r" (sp) : "memory");

    uint32_t *dst = __itcm_start__;
    uint32_t *src = __itcm_lma__;
    while (dst < __itcm_end__) {
        *dst++ = *src++;
    }

    dst = __data_start__;
    src = __data_lma__;
    while (dst < __data_end__) {
        *dst++ = *src++;
    }

    for (uint32_t *b = __bss_start__; b < __bss_end__; ) {
        *b++ = 0;
    }

    *(volatile uint32_t *)0xE000ED08u = 0;   /* SCB->VTOR = ITCM base */
    __asm volatile ("cpsie i" ::: "memory");
    app_main();
    __builtin_unreachable();
}
