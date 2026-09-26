/* Minimal freestanding string primitives. The image is linked -nostdlib
 * (-lgcc only), but the MCUXpresso SDK and TinyUSB expect memcpy/memset/
 * memmove to exist. Byte loops; GCC is invoked with -fno-builtin so none of
 * these recursion-optimize into calls to themselves. */
#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;

    while (n--) {
        *d++ = (unsigned char)c;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    if ((uintptr_t)d < (uintptr_t)s) {
        while (n--) {
            *d++ = *s++;
        }
    } else {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dst;
}
