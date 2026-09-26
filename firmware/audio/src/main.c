/* fb200-audio: milestone 1 scaffolding. Audio bring-up lands in later tasks
 * (DSP framework, CDC log, I2C probe, codec, SAI/eDMA, USB audio). */
#include "tusb.h"
#include "bsp/board_api.h"

extern int g_bss_writable;

void app_main(void)
{
    board_init();
    tusb_init();
    (void)g_bss_writable;
    while (1) {
        tud_task();
    }
}
