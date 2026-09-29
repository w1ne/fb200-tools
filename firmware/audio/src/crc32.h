#ifndef FB200_CRC32_H
#define FB200_CRC32_H
#include <stdint.h>
/* CRC-32 (IEEE 802.3, zlib.crc32): update images, crash dumps, stock data.
 * crc32_ieee_update continues a sum (start from 0xFFFFFFFF, complement at
 * the end). crc32_ieee is that over one buffer. */
uint32_t crc32_ieee_update(uint32_t c, const uint8_t *p, uint32_t len);
uint32_t crc32_ieee(const uint8_t *p, uint32_t len);
#endif
