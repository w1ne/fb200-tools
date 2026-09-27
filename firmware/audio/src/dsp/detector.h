#ifndef FB200_DSP_DETECTOR_H
#define FB200_DSP_DETECTOR_H
/* Stock level detector (FB200 ITCM 0x7a08), the noise gate's key signal:
 *   y   = biquad(x), double precision, b = [6.1584 -12.1192 5.9639],
 *         a = [1 -1.9194 0.9226] at 44.1 kHz: a +16 dB shelf above ~400 Hz
 *   pk  = peak of |y| over the current 800-step window, held for the next
 *   env = rises to the held peak at once, falls 0.2 %/step towards it.
 * Other sample rates keep the filter (it is part of the stock voicing) and
 * scale the hold and release to the same times. */

typedef struct {
    float x1, x2, y1, y2;       /* biquad state (the stock keeps it in float) */
    float win, held, env;       /* window peak, held peak, envelope */
    float release;              /* 0.002 per step at 44.1 kHz */
    unsigned count, hold;       /* steps in the window, window length (800) */
} detector_t;

void detector_init(detector_t *d, float fs);
float detector_step(detector_t *d, float x);                 /* one step: envelope */
void detector_process(detector_t *d, float *x, unsigned n);  /* in place: x -> envelope */
#endif
