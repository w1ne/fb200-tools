#include "audio/drift.h"

size_t drift_fill(const int16_t *ring, uint32_t cap, uint32_t *tail,
                  uint32_t head, int16_t *out, size_t frames, int16_t last[2],
                  uint32_t *inserts)
{
    size_t n = 0;
    while (n < frames) {
        if (*tail == head) {
            out[n * 2 + 0] = last[0];
            out[n * 2 + 1] = last[1];
            (*inserts)++;
        } else {
            last[0] = ring[*tail * 2 + 0];
            last[1] = ring[*tail * 2 + 1];
            out[n * 2 + 0] = last[0];
            out[n * 2 + 1] = last[1];
            *tail = (*tail + 1u) % cap;
        }
        n++;
    }
    return n;
}

uint32_t drift_trim(uint32_t cap, uint32_t head, uint32_t *tail,
                    uint32_t max_fill, uint32_t *drops)
{
    uint32_t fill = (head + cap - *tail) % cap;
    uint32_t dropped = 0;
    while (fill > max_fill) {
        *tail = (*tail + 1u) % cap;
        dropped++;
        fill--;
    }
    *drops += dropped;
    return dropped;
}
