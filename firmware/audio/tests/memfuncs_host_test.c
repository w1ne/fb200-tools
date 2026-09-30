/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

/* memfuncs.c (the firmware's memcpy/memset/memmove) against byte-wise
 * references: every alignment of dst and src, lengths 0..80, overlaps both
 * ways. Renamed so the host libc is not replaced. */
#define memcpy fw_memcpy
#define memset fw_memset
#define memmove fw_memmove
#define strlen fw_strlen
#define memcmp fw_memcmp
#include "memfuncs.c"
#undef memcpy
#undef memset
#undef memmove
#undef strlen
#undef memcmp
#include <stdio.h>

enum { LEN = 80, PAD = 16, BUF = LEN + 2 * PAD + 8 };
static unsigned char a[BUF], b[BUF], ref[BUF];
static int fails;

static void fill(unsigned char *p, unsigned seed)
{
    for (unsigned i = 0; i < BUF; i++) p[i] = (unsigned char)(seed * 131u + i * 7u + 1u);
}

static void check(int ok, const char *what, unsigned x, unsigned y, unsigned n)
{
    if (!ok && fails++ < 10) printf("FAIL %s dst %u src %u n %u\n", what, x, y, n);
}

static int same(const unsigned char *p, const unsigned char *q)
{
    for (unsigned i = 0; i < BUF; i++)
        if (p[i] != q[i]) return 0;
    return 1;
}

int main(void)
{
    for (unsigned n = 0; n <= LEN; n++) {
        for (unsigned x = 0; x < 8; x++) {
            /* memset */
            fill(a, n + x);
            fill(ref, n + x);
            for (unsigned i = 0; i < n; i++) ref[PAD + x + i] = 0xA5;
            check(fw_memset(a + PAD + x, 0x1A5, n) == a + PAD + x && same(a, ref), "memset", x, 0, n);
            for (unsigned y = 0; y < 8; y++) {
                /* memcpy, separate buffers */
                fill(a, n);
                fill(b, n + 99);
                fill(ref, n);
                for (unsigned i = 0; i < n; i++) ref[PAD + x + i] = b[PAD + y + i];
                check(fw_memcpy(a + PAD + x, b + PAD + y, n) == a + PAD + x && same(a, ref),
                      "memcpy", x, y, n);
                /* memmove in one buffer: dst - src from -PAD..PAD */
                for (int d = -PAD; d <= PAD; d++) {
                    unsigned s0 = PAD + y, d0 = (unsigned)((int)s0 + d) + x;
                    if (d0 + n > BUF) continue;
                    fill(a, n + (unsigned)d);
                    fill(ref, n + (unsigned)d);
                    unsigned char tmp[BUF];
                    for (unsigned i = 0; i < n; i++) tmp[i] = ref[s0 + i];
                    for (unsigned i = 0; i < n; i++) ref[d0 + i] = tmp[i];
                    check(fw_memmove(a + d0, a + s0, n) == a + d0 && same(a, ref), "memmove", d0, s0, n);
                    /* memcpy with dst below src is used by overlap-save style code: must work too */
                    if (d0 <= s0) {
                        fill(a, n + (unsigned)d);
                        fw_memcpy(a + d0, a + s0, n);
                        check(same(a, ref), "memcpy down", d0, s0, n);
                    }
                }
            }
        }
    }
    check(fw_strlen("abc") == 3 && fw_memcmp("ab", "ac", 2) < 0 && fw_memcmp("ab", "ab", 2) == 0,
          "strlen/memcmp", 0, 0, 0);
    if (fails) {
        printf("%d failures\n", fails);
        return 1;
    }
    printf("memfuncs host tests OK\n");
    return 0;
}
