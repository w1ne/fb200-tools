#include <stdarg.h>
#include <stdint.h>
#include "tusb.h"
#include "cdc_log.h"
#include "log_fmt.h"

static char ring[4096];
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

void cdc_log_task(void)
{
    if (!tud_cdc_connected()) return;
    size_t written = 0;
    while (tail != head && written < 64) {
        uint8_t ch = (uint8_t)ring[tail];
        tail = (tail + 1) % sizeof ring;
        tud_cdc_write(&ch, 1);
        written++;
    }
    if (written) tud_cdc_write_flush();
}
