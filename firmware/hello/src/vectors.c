/* 256-entry vector table; copied to ITCM 0x0 and selected via VTOR = 0.
 * Entry [1] keeps the flash reset-stub address (as stock does).
 * SysTick and the USB OTG IRQs resolve to the BSP handlers
 * (hw/bsp/imxrt/family.c) when those are linked; weak aliases keep the table
 * valid if they are ever dropped. */
#include <stdint.h>

extern uint32_t _estack;
extern void reset_stub(void);
void Default_Handler(void)
{
    for (;;) {
    }
}

void SysTick_Handler(void) __attribute__((weak, alias("Default_Handler")));
void USB_OTG1_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void USB_OTG2_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));

/* IRQ numbers from MIMXRT1062_COMMON.h; entries 0-15 are the Cortex-M7
 * exceptions, so device IRQ n lands at vector 16 + n. */
#define SYSTICK_VECTOR   15
#define USB_OTG1_VECTOR  (16 + 113)
#define USB_OTG2_VECTOR  (16 + 114)

__attribute__((section(".vectors"), used))
const void *const g_vectors[256] = {
    &_estack,
    reset_stub,
    [2 ... 14] = Default_Handler,
    [SYSTICK_VECTOR] = SysTick_Handler,
    [USB_OTG1_VECTOR] = USB_OTG1_IRQHandler,
    [USB_OTG2_VECTOR] = USB_OTG2_IRQHandler,
    [131 ... 255] = Default_Handler,
};
