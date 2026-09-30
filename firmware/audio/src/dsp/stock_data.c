/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#include <stddef.h>
#include <string.h>
#include "stock_data.h"
#include "crc32.h"

const stock_data_t *g_stock;
const stock_factory_t *g_stock_factory;

/* The tables the code indexes with (drums.c walks the event lists to their
 * end marker, divides by the beats): a blob that passed its CRC can still
 * carry bad tables (a host tool bug, a hand-made file). Same rules as
 * stockdata.py check(). Floats: no NaN/Inf (bit test, no libm). */
static int tables_ok(const stock_data_t *s)
{
    uint32_t off = 0;
    for (unsigned p = 0; p < STOCK_DRUM_PATTERNS; p++) {
        uint32_t n = s->drum_lens[p];
        if (n == 0u || n > STOCK_DRUM_EVENTS - off) return 0;
        if (((s->drum_events[off + n - 1u] >> 8) & 0xFFu) != 0xFFu) return 0;
        if (s->drum_beats[p] < 1u || s->drum_beats[p] > 9u) return 0;
        off += n;
    }
    for (unsigned r = 0; r < STOCK_DRUM_RHYTHMS; r++)
        if (s->drum_rhythm[r] >= STOCK_DRUM_PATTERNS) return 0;
    const uint32_t *f = (const uint32_t *)(const void *)&s->amp_models[0];
    const uint32_t *end = (const uint32_t *)(const void *)&s->drum_events[0];
    for (; f < end; f++)
        if ((*f & 0x7F800000u) == 0x7F800000u) return 0;
    return 1;
}

int stock_check(const void *p, uint32_t len)
{
    const stock_data_t *s = p;
    const uint32_t body = offsetof(stock_data_t, crc) + 4u;
    if (len < sizeof *s) return -1;
    if (s->magic != STOCK_MAGIC) return -2;
    uint32_t size = s->version == 1u ? sizeof *s
                  : s->version == 2u ? sizeof *s + sizeof(stock_factory_t) : 0u;
    if (size == 0u || s->size != size) return -3;
    if (len < size) return -1;
    if (crc32_ieee((const uint8_t *)p + body, size - body) != s->crc) return -4;
    if (!tables_ok(s)) return -5;
    return 0;
}

const char *stock_error(int code)
{
    switch (code) {
    case 0: return "ok";
    case -1: return "too short";
    case -2: return "missing";
    case -3: return "wrong version";
    case -4: return "bad CRC";
    case -5: return "bad tables";
    default: return "?";
    }
}

#if defined(__arm__)   /* target only; host tests set g_stock */
/* In DTCM (.bss): reading flash at run time would stall the audio during
 * flash writes. */
static stock_data_t s_stock;

int stock_load(void)
{
    int r = stock_check((const void *)STOCK_FLASH, STOCK_FLASH_SIZE);
    g_stock = NULL;
    g_stock_factory = NULL;
    if (r == 0) {
        memcpy(&s_stock, (const void *)STOCK_FLASH, sizeof s_stock);
        g_stock = &s_stock;
        /* only read by a factory reset: it stays in flash */
        if (s_stock.version >= 2u)
            g_stock_factory = (const stock_factory_t *)(STOCK_FLASH + sizeof s_stock);
    }
    return r;
}
#endif
