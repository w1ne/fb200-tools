#include "gate.h"

void gate_init(gate_t *g, float fs)
{
    *g = (gate_t){0};
    detector_init(&g->det, fs);
    double k = 44100.0 / fs;
    g->up = 0.00015 * k;
    g->down = 5e-05 * k;
    g->a = 0.1 * k;                     /* follower: new = a ctl + b old */
    g->b = 1.0 - g->a;
    if (fs == 44100.0f) g->b = 0.9;     /* the stock literal, bit for bit */
    dsp_knob_init(&g->thr, 0.01f, 0.99f);
}

void gate_set_params(gate_t *g, unsigned threshold)
{
    dsp_knob_set(&g->thr, (float)threshold * 0.01f);
}

static float gate_step(gate_t *g, float x)
{
    float t = dsp_knob_next(&g->thr);

    detector_step(&g->det, x);                       /* the chain's step */
    float env = detector_step(&g->det, x);           /* the gate's own step */

    float T = (float)((double)(t * t) * 0.01 + 1e-05);
    float w = t >= 0.1f ? 1.0f : t * 10.0f;
    float d = (float)((double)env - (double)T * 0.4);
    float T3 = T * 3.0f;
    if (d < 0.0f) d = 0.0f;
    if (d > T3) d = T3;

    if (g->ctl < d) {
        g->ctl = (float)((double)g->ctl + g->up);
        if (g->ctl > d) g->ctl = d;
    }
    if (g->ctl > d) {
        g->ctl = (float)((double)g->ctl - g->down);
        if (g->ctl < d) g->ctl = d;
    }
    if (g->ctl > T3) g->ctl = T3;

    g->smooth = (float)((double)g->ctl * g->a + (double)g->smooth * g->b);
    float gain = g->smooth >= g->ctl ? g->smooth : g->ctl;
    return (gain * w / T3 + (1.0f - w)) * x;
}

void gate_process(gate_t *g, float *x, unsigned n)
{
    for (unsigned i = 0; i < n; i++) x[i] = gate_step(g, x[i]);
}
