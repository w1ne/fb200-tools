#ifndef FB200_CRC32_H
#define FB200_CRC32_H
#include <stdint.h>
/* CRC-32 (IEEE 802.3, zlib.crc32): update images, crash dumps, stock data. */
uint32_t crc32_ieee(const uint8_t *p, uint32_t len);
#endif
