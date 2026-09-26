/* Freestanding libc stubs for symbols the MCUXpresso SDK object files
 * reference. There is no heap: allocation always fails, and abort() traps. */
#include <stddef.h>

void *malloc(size_t size)
{
    (void)size;
    return NULL;
}

void *calloc(size_t nmemb, size_t size)
{
    (void)nmemb;
    (void)size;
    return NULL;
}

void *realloc(void *ptr, size_t size)
{
    (void)ptr;
    (void)size;
    return NULL;
}

void free(void *ptr)
{
    (void)ptr;
}

void abort(void)
{
    for (;;) {
    }
}
