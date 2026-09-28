/* Host test: the elastic playback ring must duplicate the last frame when it
 * starves (bounded insert count) and discard frames when it stays overfull,
 * keeping the long-term fill within bounds. Also the USB playback -> chain
 * input mix (`usb in|mix`). */
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

    /* USB playback into the chain input: mono mean of L/R. Replace drops
     * the instrument (R cleared, so the engine's L + R is the playback);
     * mix adds it to the instrument. */
    {
        int16_t play[3 * 2] = {16384, 16384, 16384, -16384, -32768, 0};
        float l[3] = {0.25f, 0.25f, 0.25f}, r[3] = {0.1f, 0.1f, 0.1f};
        drift_play_to_input(l, r, play, 3, 1);
        assert(l[0] == 0.5f && l[1] == 0.0f && l[2] == -0.5f);
        assert(r[0] == 0.0f && r[1] == 0.0f && r[2] == 0.0f);
        float ml[3] = {0.25f, 0.25f, 0.25f}, mr[3] = {0.1f, 0.1f, 0.1f};
        drift_play_to_input(ml, mr, play, 3, 0);
        assert(ml[0] == 0.75f && ml[1] == 0.25f && ml[2] == -0.25f);
        assert(mr[0] == 0.1f && mr[2] == 0.1f);
        /* host not playing: the engine passes zeros -> silence / instrument */
        int16_t zero[3 * 2] = {0};
        drift_play_to_input(l, r, zero, 3, 1);
        assert(l[0] == 0.0f && l[2] == 0.0f);
        drift_play_to_input(ml, mr, zero, 3, 0);
        assert(ml[0] == 0.75f);
    }

    printf("drift host tests OK\n");
    return 0;
}
