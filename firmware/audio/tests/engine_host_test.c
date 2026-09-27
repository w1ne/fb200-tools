/* Host test: the elastic playback ring must duplicate the last frame when it
 * starves (bounded insert count) and discard frames when it stays overfull,
 * keeping the long-term fill within bounds. */
#include <assert.h>
#include <stdio.h>
#include "audio/drift.h"

int main(void)
{
    int16_t ring[8 * 2];
    int16_t out[4 * 2];
    int16_t last[2] = {111, 222};
    uint32_t inserts = 0, drops = 0;
    uint32_t tail = 0, head = 0;

    /* Empty ring: every frame repeats `last`, counters advance. */
    size_t n = drift_fill(ring, 8, &tail, head, out, 4, last, &inserts);
    assert(n == 4 && inserts == 4);
    assert(out[0] == 111 && out[1] == 222);
    assert(out[6] == 111 && out[7] == 222);

    /* Two frames available: two real frames then two inserts. */
    ring[0] = 1; ring[1] = 2; ring[2] = 3; ring[3] = 4;
    head = 2; tail = 0; inserts = 0;
    n = drift_fill(ring, 8, &tail, head, out, 4, last, &inserts);
    assert(n == 4 && inserts == 2 && tail == 2);
    assert(out[0] == 1 && out[2] == 3 && out[4] == 3 && out[6] == 3);

    /* Overfull ring: trim restores the fill to max_fill and counts drops. */
    head = 7; tail = 0; drops = 0;
    uint32_t d = drift_trim(8, head, &tail, 4, &drops);
    assert(d == 3 && drops == 3);
    assert(((head + 8 - tail) % 8) == 4);

    /* Ring within bounds: trim is a no-op. */
    d = drift_trim(8, head, &tail, 4, &drops);
    assert(d == 0 && drops == 3);

    printf("drift host tests OK\n");
    return 0;
}
