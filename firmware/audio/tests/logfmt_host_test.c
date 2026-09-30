/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "debug/log_fmt.h"

static void check(const char *expect, const char *fmt, ...)
{
    char b[64];
    memset(b, 0, sizeof b);      /* the formatter does not NUL-terminate */
    va_list ap;
    va_start(ap, fmt);
    size_t n = log_vformat(b, sizeof b - 1, fmt, ap);
    va_end(ap);
    b[n < sizeof b - 1 ? n : sizeof b - 1] = 0;
    if (strcmp(b, expect) != 0) {
        printf("FAIL: fmt %s -> '%s' (expected '%s')\n", fmt, b, expect);
        assert(0);
    }
}

int main(void)
{
    check("fb200-audio up, bss_writable=1", "fb200-audio up, bss_writable=%d", 1);
    check("codec: 12/12 writes ok", "codec: %u/%u writes ok", 12u, 12u);
    check("1a: 2f", "%02x: %02x", 0x1a, 0x2f);
    check("addr=1a, val=ff", "addr=%x, val=%x", 0x1a, 0xff);
    check("str", "%s", "str");
    check("50%", "50%%");
    check("neg=-42", "neg=%d", -42);
    check("pad=   7", "pad=%4u", 7u);
    check("pad=0007", "pad=%04u", 7u);
    check("fill=512 ovf=0", "fill=%lu ovf=%lu", 512ul, 0ul);
    check("neg=-3 hex=ff", "neg=%ld hex=%lx", -3L, 0xfful);
    check("pad=00ab", "pad=%04lx", 0xabul);
    printf("log_fmt host tests OK\n");
    return 0;
}
