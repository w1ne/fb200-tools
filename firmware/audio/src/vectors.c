/* 256-entry vector table, stored at block 0 offset 0 and copied to ITCM 0x0
 * by stage2, which then selects it via VTOR = 0.
 * Entry [1] keeps the vendor flash stub address (as stock does); the vendor
 * loader reaches stage2 at ITCM 0x4d6, not through this vector.
 * SysTick and the USB OTG IRQs resolve to the BSP handlers
 * (hw/bsp/imxrt/family.c) when those are linked; weak aliases keep the table
 * valid if they are ever dropped. Every other entry records a crumb and
 * resets into recovery (src/debug/recovery.c). */
#include <stdint.h>
#include "fsl_device_registers.h"

extern uint32_t _estack;
/* Faults and unexpected IRQs record what happened in SRC_GPR (console
 * `crumbs`) and reset; recovery then stays on the USB console instead of
 * relaunching the app (src/debug/recovery.c). */
__attribute__((naked)) void Default_Handler(void)
{
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "mov r1, lr\n"
        "b fault_record\n");
}

void SysTick_Handler(void) __attribute__((weak, alias("Default_Handler")));
void USB_OTG1_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void USB_OTG2_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));

/* Vendor reset stub at block 0 offset 0x4d8 (flash 0x600104d8). */
#define VENDOR_STUB 0x600104d9u

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
    (const void *)VENDOR_STUB,
    [2 ... 14] = Default_Handler,
    [SYSTICK_VECTOR] = SysTick_Handler,
    [16 ... 127] = Default_Handler,
    [USB_OTG2_VECTOR] = USB_OTG2_IRQHandler,
    [USB_OTG1_VECTOR] = USB_OTG1_IRQHandler,
    [130 ... 255] = Default_Handler,
};
