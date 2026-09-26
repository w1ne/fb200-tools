/* Minimal freestanding <stdlib.h>: declarations only. The allocator never
 * succeeds in this image; see src/compat/stubs.c. */
#pragma once

#include <stddef.h>

void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
void abort(void);
