/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#include <stdarg.h>
#include <stdint.h>
#include "tusb.h"
#include "cdc_log.h"
#include "log_fmt.h"

extern uint32_t tusb_time_millis_api(void);

static char ring[16384];
static volatile size_t head, tail;

void cdc_log_init(void) { head = tail = 0; }

void cdc_log_write(const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        size_t next = (head + 1) % sizeof ring;
        if (next == tail) break;            /* drop on overflow, never block */
        ring[head] = data[i];
        head = next;
    }
}

void log_printf(const char *fmt, ...)
{
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    size_t n = log_vformat(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    if (n > sizeof buf - 1) n = sizeof buf - 1;
    cdc_log_write(buf, n);
}

void log_puts(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    cdc_log_write(s, n);
}

/* Push queued output to the host before a deliberate reset or detach. */
void log_flush_ms(uint32_t ms)
{
    uint32_t t0 = tusb_time_millis_api();
    while (tusb_time_millis_api() - t0 < ms) {
        tud_task();
        cdc_log_task();
    }
}

void cdc_log_task(void)
{
    if (!tud_cdc_connected()) return;
    size_t written = 0;
    /* Only consume the ring when TinyUSB accepts the byte: dropping here
     * corrupted console output under load (the FIFO is 256 bytes). */
    while (tail != head && tud_cdc_write_available() > 0u) {
        uint8_t ch = (uint8_t)ring[tail];
        if (tud_cdc_write(&ch, 1) != 1u) break;
        tail = (tail + 1) % sizeof ring;
        written++;
    }
    if (written) tud_cdc_write_flush();
}
