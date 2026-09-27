#include "detector.h"

void detector_init(detector_t *d, float fs)
{
    *d = (detector_t){0};
    float k = 44100.0f / fs;
    d->release = 0.002f * k;
    d->hold = (unsigned)(800.0f / k + 0.5f);
}

float detector_step(detector_t *d, float x)
{
    /* ITCM 0x7a08: the products and sums are double, the state float */
    double acc = (double)x * 6.158394962781;
    acc -= (double)d->x1 * 12.119162896656;
    acc += (double)d->x2 * 5.963889239004;
    acc += (double)d->y1 * 1.919444571522;
    acc -= (double)d->y2 * 0.922565876651;
    float y = (float)acc;
    d->x2 = d->x1;
    d->x1 = x;
    d->y2 = d->y1;
    d->y1 = y;

    float a = y > 0.0f ? y : -y;
    if (d->win < a) d->win = a;
    if (++d->count > d->hold) {           /* window done: its peak becomes the held one */
        d->held = d->win;
        d->count = 0;
        d->win = 0.0f;
    }
    if (d->held < a) d->held = a;
    if (d->env < d->held) d->env = d->held;
    if (d->env > d->held) {
        d->env -= (d->env - d->held) * d->release;
        if (d->env < d->held) d->env = d->held;
    }
    return d->env;
}

void detector_process(detector_t *d, float *x, unsigned n)
{
    for (unsigned i = 0; i < n; i++) x[i] = detector_step(d, x[i]);
}
