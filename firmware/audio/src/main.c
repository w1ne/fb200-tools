/* fb200-audio: milestone 1 scaffolding. Audio bring-up lands in later tasks
 * (I2C probe, codec, SAI/eDMA, USB audio). */
#include "tusb.h"
#include "bsp/board_api.h"
#include "debug/cdc_log.h"

extern int g_bss_writable;

void app_main(void)
{
    board_init();
    tusb_init();
    cdc_log_init();
    log_printf("fb200-audio up, bss_writable=%d\r\n", g_bss_writable);
    while (1) {
        tud_task();
        cdc_log_task();
    }
}
