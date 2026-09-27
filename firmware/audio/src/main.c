/* fb200-audio: milestone 1. Phase 0 adds the I2C probe commands
 * ('s' scan, 'd' dump bus1/0x1A, 'D' dump bus2/0x1A); audio bring-up lands in
 * later tasks (codec, SAI/eDMA, USB audio). */
#include "tusb.h"
#include "bsp/board_api.h"
#include "debug/cdc_log.h"
#include "audio/i2c_probe.h"

extern int g_bss_writable;

void app_main(void)
{
    cdc_log_init();
    log_printf("app_main entered, bss_writable=%d\r\n", g_bss_writable);
    board_init();
    log_printf("board_init done\r\n");
    tusb_init();
    log_printf("tusb_init done\r\n");
    i2c_probe_init();
    log_printf("probe init done\r\n");

    uint32_t loops = 0;
    while (1) {
        tud_task();
        cdc_log_task();
        if (++loops >= 2000000u) {          /* heartbeat, no timer needed */
            loops = 0;
            log_printf("hb, cdc=%d\r\n", (int)tud_cdc_connected());
        }
        if (tud_cdc_available()) {
            char cmd = (char)tud_cdc_read_char();
            if (cmd == 's') i2c_scan_all();
            else if (cmd == 'd') i2c_dump(1, 0x1A);
            else if (cmd == 'D') i2c_dump(2, 0x1A);
        }
    }
}
