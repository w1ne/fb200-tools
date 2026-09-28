#ifndef FB200_DRIFT_H
#define FB200_DRIFT_H

#include <stddef.h>
#include <stdint.h>

/* Elastic FIFO helpers shared by the USB rings and the engine. Pure so the
 * host test (tests/test_dsp_host.py) can exercise the drift behaviour. */

/* Fill `frames` stereo frames into out[] from ring[cap*2]; when the ring
 * starves, repeat the last frame and count an insert. Updates *tail and
 * last[2]. Returns frames. */
size_t drift_fill(const int16_t *ring, uint32_t cap, uint32_t *tail,
                  uint32_t head, int16_t *out, size_t frames, int16_t last[2],
                  uint32_t *inserts);

/* Discard frames from the tail while the fill exceeds max_fill (bounds
 * long-term latency). Counts into *drops. Returns the number dropped. */
uint32_t drift_trim(uint32_t cap, uint32_t head, uint32_t *tail,
                    uint32_t max_fill, uint32_t *drops);

/* Host playback into the chain input (reamping, console `usb in|mix`). The
 * chain input is L + R of the ADC block; the stereo int16 playback goes in as
 * its mono mean (L + R) / 2, so a file played on both channels enters at its
 * own level. replace != 0: l = mean, r = 0 (the instrument is dropped);
 * replace == 0: l += mean (instrument + playback). */
void drift_play_to_input(float *l, float *r, const int16_t *play, size_t n,
                         int replace);

#endif
