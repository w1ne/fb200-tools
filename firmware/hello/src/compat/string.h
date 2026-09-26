/* Minimal freestanding <string.h> for the MCUXpresso SDK/TinyUSB subset.
 * The functions the linked object set references are implemented in
 * src/memfuncs.c; nothing else is declared so unimplemented calls fail
 * loudly at compile/link time. */
#pragma once

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
size_t strlen(const char *s);
