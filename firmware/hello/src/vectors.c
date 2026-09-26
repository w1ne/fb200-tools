/* 256-entry vector table; copied to ITCM 0x0 and selected via VTOR = 0.
 * Entry [1] keeps the flash reset-stub address (as stock does).
 * SysTick and the USB OTG IRQs resolve to the BSP handlers
 * (hw/bsp/imxrt/family.c) when those are linked; weak aliases keep the table
 * valid if they are ever dropped. Every other entry spins in a known handler
 * instead of faulting on an unexpected IRQ. */
#include <stdint.h>
#include "fsl_device_registers.h"

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

/* Cortex-M7 exceptions occupy vector entries 0-15; device IRQ n lands at
 * 16 + n. The enums come from MIMXRT1062_COMMON.h (IRQn_Type). */
#define SYSTICK_VECTOR   15
#define USB_OTG2_VECTOR  (16 + USB_OTG2_IRQn)
#define USB_OTG1_VECTOR  (16 + USB_OTG1_IRQn)

_Static_assert(16 + USB_OTG2_IRQn == 128, "USB OTG2 vector index mismatch");
_Static_assert(16 + USB_OTG1_IRQn == 129, "USB OTG1 vector index mismatch");
_Static_assert(SYSTICK_VECTOR == 15, "SysTick vector index mismatch");

__attribute__((section(".vectors"), used))
const void *const g_vectors[256] = {
    &_estack,
    reset_stub,
    [2 ... 14] = Default_Handler,
    [SYSTICK_VECTOR] = SysTick_Handler,
    [16 ... 127] = Default_Handler,
    [USB_OTG2_VECTOR] = USB_OTG2_IRQHandler,
    [USB_OTG1_VECTOR] = USB_OTG1_IRQHandler,
    [130 ... 255] = Default_Handler,
};
