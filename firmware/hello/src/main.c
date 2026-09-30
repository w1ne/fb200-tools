/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#include "tusb.h"
#include "bsp/board_api.h"

static const char banner[] = "FB200 hello - fb200-tools custom firmware\r\n";

void app_main(void)
{
    board_init();
    tusb_init();

    bool announced = false;
    while (1) {
        tud_task();

        if (tud_cdc_connected() && !announced) {
            tud_cdc_write(banner, sizeof(banner) - 1);
            tud_cdc_write_flush();
            announced = true;
        } else if (!tud_cdc_connected()) {
            announced = false;
        }

        if (tud_cdc_available()) {
            char buf[64];
            uint32_t n = tud_cdc_read(buf, sizeof(buf));
            if (n) {
                tud_cdc_write(buf, n);
                tud_cdc_write_flush();
            }
        }
    }
}
