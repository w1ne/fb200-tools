#include <stddef.h>
#include <string.h>
#include "stock_data.h"
#include "crc32.h"

const stock_data_t *g_stock;
const stock_factory_t *g_stock_factory;

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
