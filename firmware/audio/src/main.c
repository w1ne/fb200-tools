/* fb200-audio: milestone 1. The USB console (see `help`) is the debug
 * backbone; audio bring-up lands in later tasks (codec, SAI/eDMA, USB audio). */
#include <stdint.h>
#include "fsl_device_registers.h"
#include "tusb.h"
#include "bsp/board_api.h"
#include "debug/cdc_log.h"
#include "debug/console.h"
#include "audio/i2c_probe.h"
#include "debug/recovery.h"

extern int g_bss_writable;

/* Handover to the vendor bootloader: the stock 0xC1 handler mutes the codec
 * and issues a software reset (SCB->AIRCR SYSRESETREQ, stock ITCM 0x18c68);
 * the bootloader decides what to do from the reset source. */
__attribute__((noreturn)) void console_reboot(void)
{
    crumb_clear();   /* a deliberate reset is not a hang */
    tud_disconnect();
    for (volatile uint32_t i = 0; i < 4000000u; i++) {
    }
    NVIC_SystemReset();
    __builtin_unreachable();
}

#ifdef FB200_RECOVERY
#define VARIANT "recovery"
#else
#define VARIANT "app"
#endif

void app_main(void)
{
    cdc_log_init();
    console_init();
#ifdef FB200_RECOVERY
    /* Launches the app (never returns) unless something says stay. */
    const char *stay = recovery_boot();
#endif
    log_printf("fb200-audio 0.6.2-dev " VARIANT "\r\n");
#ifdef FB200_RECOVERY
    log_printf("recovery: staying because %s\r\n", stay);
#endif
    log_printf("bss_writable=%d - type 'help'\r\n", g_bss_writable);
    board_init();
    tusb_init();
    i2c_probe_init();
#ifndef FB200_RECOVERY
    crumb_alive();
#endif
    log_printf("ready\r\n");

    uint32_t loops = 0;
    while (1) {
        wdog_feed();
        tud_task();
        cdc_log_task();
        console_task();
        if (console_heartbeat_on() && ++loops >= 2000000u) {
            loops = 0;
            log_printf("hb\r\n");
        }
    }
}
