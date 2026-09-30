/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#include <stdint.h>
#include "log_fmt.h"

static size_t put(char *dst, size_t cap, size_t at, char c)
{
    if (at < cap) dst[at] = c;
    return at + 1;
}

static size_t fmt_uint(char *dst, size_t cap, size_t at, uint32_t v, unsigned base,
                       unsigned width, int zero)
{
    char tmp[11];
    unsigned n = 0;
    do { tmp[n++] = "0123456789abcdef"[v % base]; v /= base; } while (v);
    while (n < width) tmp[n++] = zero ? '0' : ' ';
    while (n) at = put(dst, cap, at, tmp[--n]);
    return at;
}

static size_t fmt_int(char *dst, size_t cap, size_t at, int32_t v, unsigned width, int zero)
{
    if (v < 0) {
        at = put(dst, cap, at, '-');
        return fmt_uint(dst, cap, at, (uint32_t)(-(int64_t)v), 10, width, zero);
    }
    return fmt_uint(dst, cap, at, (uint32_t)v, 10, width, zero);
}

size_t log_vformat(char *dst, size_t cap, const char *fmt, va_list ap)
{
    size_t at = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { at = put(dst, cap, at, *p); continue; }
        p++;
        int zero = 0;
        unsigned width = 0;
        if (*p == '0') { zero = 1; p++; }
        while (*p >= '0' && *p <= '9') { width = width * 10u + (unsigned)(*p - '0'); p++; }
        int is_long = 0;   /* %lu/%ld/%lx: consume a long (32-bit on the MCU, 64 on hosts) */
        if (*p == 'l') { is_long = 1; p++; }
        switch (*p) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s) at = put(dst, cap, at, *s++);
            break;
        }
        case 'c': at = put(dst, cap, at, (char)va_arg(ap, int)); break;
        case 'd':
            at = fmt_int(dst, cap, at, is_long ? (int32_t)va_arg(ap, long) : va_arg(ap, int),
                         width, zero);
            break;
        case 'u':
        case 'x': {
            uint32_t v = is_long ? (uint32_t)va_arg(ap, unsigned long) : va_arg(ap, unsigned);
            at = fmt_uint(dst, cap, at, v, *p == 'x' ? 16u : 10u, width, zero);
            break;
        }
        case '%': at = put(dst, cap, at, '%'); break;
        case '\0': p--; break;
        default:
            at = put(dst, cap, at, '%');
            at = put(dst, cap, at, *p);
            break;
        }
    }
    return at;
}
