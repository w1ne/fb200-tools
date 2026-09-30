/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

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
int memcmp(const void *a, const void *b, size_t n);
