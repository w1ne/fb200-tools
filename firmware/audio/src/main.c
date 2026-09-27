/* fb200-audio: milestone 1. The USB console (see `help`) is the debug
 * backbone; audio bring-up lands in later tasks (codec, SAI/eDMA, USB audio). */
#include <stdint.h>
#include "fsl_device_registers.h"
#include "tusb.h"
#include "bsp/board_api.h"
#include "debug/cdc_log.h"
#include "debug/console.h"
#include "audio/i2c_probe.h"
#include "audio/engine.h"
#include "audio/usb_audio.h"
#include "audio/codec.h"
#include "led.h"
#include "debug/recovery.h"
#include "audio/frontend.h"
#include "ui/display.h"
#include "ui/controls.h"
#include "ui/power.h"
#include "ui/ui.h"
#include "ui/rgb.h"

extern int g_bss_writable;
extern uint32_t tusb_time_millis_api(void);

/* Handover to the vendor bootloader: the stock 0xC1 handler mutes the codec
 * and issues a software reset (SCB->AIRCR SYSRESETREQ, stock ITCM 0x18c68);
 * the bootloader decides what to do from the reset source. */
__attribute__((noreturn)) void console_reboot(void)
{
    crumb_clear();   /* a deliberate reset is not a hang */
    log_flush_ms(300);
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
#ifndef FB200_RECOVERY
    i2c_probe_init();
    frontend_init();
    engine_init();
    led_init();
    bool codec_ok = codec_init();
    uint32_t codec_retries;
    uint16_t codec_retry_reg;
    codec_init_stats(&codec_retries, &codec_retry_reg);
    log_printf("codec init: %s (retries=%lu first=%x)\r\n", codec_ok ? "ok" : "FAILED",
               (unsigned long)codec_retries, (unsigned)codec_retry_reg);
    frontend_enable();
    display_init();
    controls_init();
    power_init();
    rgb_init();
    ui_init();
    if (!codec_ok) {
        engine_set_mute(true); /* start muted when the codec did not answer */
    }
    crumb_alive();   /* after init: recovery treats a leftover marker as a hang */
#endif
    log_printf("ready\r\n");

    uint32_t loops = 0;
    while (1) {
        wdog_feed();
        tud_task();
#ifndef FB200_RECOVERY
        {
            uint32_t now = tusb_time_millis_api();
            display_task(now);
            controls_task(now);
            power_task(now);
            ui_task(now);
        }
#endif
        cdc_log_task();
        console_task();
#ifndef FB200_RECOVERY
        usb_audio_task();
        engine_task();
        {
            uint32_t pf, cf, ovf, unf;
            uint8_t spk_alt, mic_alt;
            usb_audio_stats(&pf, &cf, &ovf, &unf, &spk_alt, &mic_alt);
            led_set_streaming(spk_alt != 0 || mic_alt != 0);
        }
        led_task();
#endif
        if (console_heartbeat_on() && ++loops >= 2000000u) {
            loops = 0;
            log_printf("hb\r\n");
        }
    }
}
