/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

/* Target glue for the app protocol: flash access through the XIP window and
 * flash_store() (sector read-modify-write), battery from the power monitor. */
#include <string.h>
#include "proto/proto.h"
#include "debug/selfupdate.h"
#include "ui/power.h"

#define FLASH_AHB 0x60000000u
#define SECTOR 0x1000u

void proto_flash_read(uint32_t off, void *dst, uint32_t n)
{
    memcpy(dst, (const void *)(FLASH_AHB + off), n);
}

int proto_flash_write(uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *p = src;
    while (n) {   /* flash_store works inside one 4 KB sector */
        uint32_t k = SECTOR - (off & (SECTOR - 1u));
        if (k > n) k = n;
        int r = flash_store(off, p, k);
        if (r) return r;
        off += k;
        p += k;
        n -= k;
    }
    return 0;
}

void proto_battery(uint8_t *percent, uint8_t *charging)
{
    const power_state_t *s = power_state();
    *percent = (uint8_t)(s->level * 25u);   /* stock: level 0..4 x 25 */
    *charging = s->charging;
}

/* ---- transports and actions on the target ---- */
#include "bt/bt.h"
#include "debug/recovery.h"

void bt_rx_frame_bytes(const uint8_t *data, size_t n) { proto_feed(PROTO_BLE, data, (uint32_t)n); }

static void ble_send(const uint8_t *frame, uint32_t len) { (void)bt_queue(frame, len); }

void proto_port_init(void)
{
    proto_set_sender(PROTO_BLE, ble_send);
    proto_init();
}

#include "ui/ui.h"
int proto_hook_factory_reset(void) { return ui_factory_reset(); }

/* 0xC1/0xC4: our updates go through the USB recovery, not the vendor DFU. */
void proto_hook_bootloader(void) { recovery_request(); }

void proto_hook_bt_enable(bool on)
{
    (void)bt_at(on ? "AT+B501" : "AT+B500");
    (void)bt_at("AT+CZ");
}

void proto_hook_bt_name(const uint8_t name[20])
{
    /* the name goes into AT command lines: stop at a control byte (a CR/LF
     * in the name would start a second AT command) */
    size_t len = 0;
    while (len < 20 && name[len] >= 0x20u && name[len] != 0x7Fu) len++;
    char cmd[40];
    memcpy(cmd, "AT+BD", 5);
    memcpy(cmd + 5, name, len);
    memcpy(cmd + 5 + len, " Audio", 7);
    (void)bt_at(cmd);
    memcpy(cmd, "AT+BM", 5);
    memcpy(cmd + 5, name, len);
    cmd[5 + len] = 0;
    (void)bt_at(cmd);
    (void)bt_at("AT+CZ");
}
