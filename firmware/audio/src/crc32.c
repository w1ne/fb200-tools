#include "crc32.h"

uint32_t crc32_ieee_update(uint32_t c, const uint8_t *p, uint32_t len)
{
    while (len--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c;
}

uint32_t crc32_ieee(const uint8_t *p, uint32_t len)
{
    return ~crc32_ieee_update(0xFFFFFFFFu, p, len);
}
