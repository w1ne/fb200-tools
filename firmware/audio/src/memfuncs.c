/* Minimal freestanding string primitives. The image is linked -nostdlib
 * (-lgcc only), but the MCUXpresso SDK and TinyUSB expect memcpy/memset/
 * memmove to exist. Word loops when the addresses allow (the DSP code copies
 * float buffers: a byte loop costs ~4x), bytes for the rest. GCC is invoked
 * with -fno-builtin, and loop distribution is off here: none of these loops
 * may turn into a call to the function itself. */
#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__) && !defined(__clang__)
#define NO_LIBCALL __attribute__((optimize("no-tree-loop-distribute-patterns")))
#else
#define NO_LIBCALL
#endif

typedef uint32_t __attribute__((may_alias)) word_t;
#define W sizeof(word_t)

/* Forward copy, also correct for an overlap with dst below src. */
NO_LIBCALL static void copy_fwd(unsigned char *d, const unsigned char *s, size_t n)
{
    if ((((uintptr_t)d ^ (uintptr_t)s) & (W - 1)) == 0) {
        while (n && ((uintptr_t)d & (W - 1))) {
            *d++ = *s++;
            n--;
        }
        word_t *dw = (word_t *)(void *)d;
        const word_t *sw = (const word_t *)(const void *)s;
        for (; n >= W; n -= W) *dw++ = *sw++;
        d = (unsigned char *)dw;
        s = (const unsigned char *)sw;
    }
    while (n--) *d++ = *s++;
}

/* Backward copy (dst above src), the mirror of copy_fwd. */
NO_LIBCALL static void copy_bwd(unsigned char *d, const unsigned char *s, size_t n)
{
    d += n;
    s += n;
    if ((((uintptr_t)d ^ (uintptr_t)s) & (W - 1)) == 0) {
        while (n && ((uintptr_t)d & (W - 1))) {
            *--d = *--s;
            n--;
        }
        word_t *dw = (word_t *)(void *)d;
        const word_t *sw = (const word_t *)(const void *)s;
        for (; n >= W; n -= W) *--dw = *--sw;
        d = (unsigned char *)dw;
        s = (const unsigned char *)sw;
    }
    while (n--) *--d = *--s;
}

NO_LIBCALL void *memcpy(void *dst, const void *src, size_t n)
{
    copy_fwd(dst, src, n);
    return dst;
}

NO_LIBCALL void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    unsigned char b = (unsigned char)c;

    while (n && ((uintptr_t)d & (W - 1))) {
        *d++ = b;
        n--;
    }
    word_t v = b * 0x01010101u;
    word_t *dw = (word_t *)(void *)d;
    for (; n >= W; n -= W) *dw++ = v;
    d = (unsigned char *)dw;
    while (n--) *d++ = b;
    return dst;
}

NO_LIBCALL void *memmove(void *dst, const void *src, size_t n)
{
    if ((uintptr_t)dst <= (uintptr_t)src || (uintptr_t)dst >= (uintptr_t)src + n)
        copy_fwd(dst, src, n);
    else
        copy_bwd(dst, src, n);
    return dst;
}

size_t strlen(const char *s)
{
    const char *p = s;

    while (*p) {
        p++;
    }
    return (size_t)(p - s);
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    }
    return 0;
}
