/* Enter the RT1062 mask-ROM serial downloader (SDP over USB HID 1FC9:0135).
 * It lives in ROM, so it works whatever is in flash and needs neither the
 * vendor bootloader nor A+D. Used by the `rom` console command and by every
 * fault/unexpected-IRQ handler, so a crash also ends in a USB-drivable state. */
#include <stdint.h>
#include "fsl_device_registers.h"
#include "fsl_romapi.h"
#include "selfupdate.h"
#include "cdc_log.h"

#define ROM_ARG_SERIAL_DOWNLOADER 0xEB100000u   /* tag 0xEB, mode 1 = SDP */

/* SNVS LP general-purpose registers survive a system reset, so they carry
 * what happened across the reboot. [0] = what, [1..3] = details. */
#define CRUMB_ROM_ENTRY 0xB0070000u
#define CRUMB_FAULT     0xFA000000u

void crumbs_print(void)
{
    log_printf("crumbs %08x %08x %08x %08x\r\n", (unsigned)SNVS->LPGPR[0],
               (unsigned)SNVS->LPGPR[1], (unsigned)SNVS->LPGPR[2], (unsigned)SNVS->LPGPR[3]);
    for (int i = 0; i < 4; i++) SNVS->LPGPR[i] = 0;
}

/* Called from Default_Handler with the stacked exception frame. */
__attribute__((noreturn, used)) void fault_record(const uint32_t *frame)
{
    SNVS->LPGPR[0] = CRUMB_FAULT | __get_IPSR();
    SNVS->LPGPR[1] = SCB->CFSR;
    SNVS->LPGPR[2] = SCB->HFSR;
    SNVS->LPGPR[3] = frame[6];   /* stacked PC */
    rom_serial_downloader();
}

__attribute__((noreturn)) void rom_serial_downloader(void)
{
    __disable_irq();
    if ((SNVS->LPGPR[0] & 0xFF000000u) != CRUMB_FAULT) {
        SNVS->LPGPR[0] = CRUMB_ROM_ENTRY | 1u;
    }
    SysTick->CTRL = 0;
    for (uint32_t i = 0; i < 8u; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFu;
        NVIC->ICPR[i] = 0xFFFFFFFFu;
    }
    /* Detach from the host and stop our device controller so the ROM
     * enumerates from a clean state. */
    USB1->USBCMD &= ~USBHS_USBCMD_RS_MASK;
    for (volatile uint32_t i = 0; i < 4000000u; i++) {
    }
    USB1->USBCMD |= USBHS_USBCMD_RST_MASK;
    while (USB1->USBCMD & USBHS_USBCMD_RST_MASK) {
    }
    SCB_DisableDCache();
    SNVS->LPGPR[2] = CRUMB_ROM_ENTRY | 2u;   /* reached the ROM call */
    uint32_t arg = ROM_ARG_SERIAL_DOWNLOADER;
    ROM_RunBootloader(&arg);
    for (;;) {
    }
}
