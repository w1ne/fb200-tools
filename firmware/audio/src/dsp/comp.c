#include "arm_math.h"
#include "comp.h"
#include "math.h"

void comp_init(comp_t *c, float fs)
{
    *c = (comp_t){0};
    float k = 44100.0f / fs;
    c->hold = 500.0f / k;
    c->release = 0.0025f * k;
    c->ring_len = (unsigned)(129.0f / k + 0.5f);
    if (c->ring_len > COMP_RING_MAX) c->ring_len = COMP_RING_MAX;
    c->thr_index = -1;
    dsp_knob_init(&c->attack, 0.01f, 0.99f);
    dsp_knob_init(&c->ratio, 0.01f, 0.99f);
    dsp_knob_init(&c->thr, 0.001f, 0.999f);
    dsp_knob_init(&c->level, 0.001f, 0.999f);
}

void comp_set_params(comp_t *c, unsigned type, unsigned attack, unsigned threshold,
                     unsigned ratio, unsigned level)
{
    (void)type;
    float sq;
    arm_sqrt_f32((float)ratio * 0.01f, &sq);
    dsp_knob_set(&c->attack, (float)attack * 0.01f);
    dsp_knob_set(&c->thr, (float)threshold * 0.01f);
    dsp_knob_set(&c->ratio, sq);
    dsp_knob_set(&c->level, (float)level * 0.01f);
}

/* Stock table 0x20008978: exact 3 dB points, the four steps between them at
 * fixed (uneven) ratios. */
static float threshold_of(int i)
{
    static const float step[5] = { 1.0f, 1.0732111f, 1.1493996f, 1.2315438f, 1.3203702f };
    if (i < 0) i = 0;
    if (i > 100) i = 100;
    return dsp_db_to_gain(-60.0f + 3.0f * (float)(i / 5)) * step[i % 5];
}

static float comp_step(comp_t *c, float x)
{
    /* knobs, as the callback hands them over (<= 1e-4 reads as 0) */
    float at = dsp_knob_next(&c->attack);
    float sq = dsp_knob_next(&c->ratio);
    float thr = dsp_knob_next(&c->thr);
    float lvl = dsp_knob_next(&c->level);
    if (!(at > 1e-4f)) at = 0.0f;
    if (!(sq > 1e-4f)) sq = 0.0f;

    int ti = (int)(thr * 100.0f);        /* truncates: 0.29f * 100 -> 28 */
    if (ti != c->thr_index) {
        c->thr_index = ti;
        c->T = threshold_of(ti);
    }
    float slope = 1.0f / (1.0f + sq * 9.0f);
    float delay = (1.0f - at) * (float)(c->ring_len - 1);

    /* detector: mono = (L + R) / 2 of the stock's identical channels */
    float a = x < 0.0f ? -x : x;
    if (c->win < a) c->win = a;
    c->count += 1.0f;
    if (c->count > c->hold) {
        c->held = c->win;
        c->count = 0.0f;
        c->win = 0.0f;
    }
    if (c->held < a) c->held = a;
    if (c->env < c->held) c->env = c->held;
    if (c->env > c->held) {
        c->env -= c->held * c->release;
        if (c->env < c->held) c->env = c->held;
    }

    float e = c->env;
    float out_level = e < c->T ? e : c->T + (e - c->T) * slope;
    if (e < 1e-6f) e = 1e-6f;
    c->ring[c->w] = out_level / e;
    int r = (int)((float)c->w - delay);
    if (r < 0) r += (int)c->ring_len;
    if (++c->w >= c->ring_len) c->w -= c->ring_len;
    c->g = c->ring[r] * 0.2f + c->g * 0.8f;

    /* audio: HP then the high shelf, the second fed by the HP's history */
    float y = x * 0.98915714f;
    y = y - c->x1 * 1.9783143f;
    y = y + c->x2 * 0.98915714f;
    y = y + c->y1 * 1.9783102f;
    y = y - c->y2 * 0.9783183f;
    float v = y * 0.93143725f;
    v = v + c->y1 * 1.5594625f;
    v = v + c->y2 * 0.6969919f;
    v = v - c->v1 * 1.5594625f;
    v = v - c->v2 * 0.62842923f;
    c->x2 = c->x1;
    c->x1 = x;
    c->y2 = c->y1;
    c->y1 = y;
    c->v2 = c->v1;
    c->v1 = v;
    return v * c->g * lvl * 6.0f;
}

void comp_process(comp_t *c, float *x, unsigned n)
{
    for (unsigned i = 0; i < n; i++) x[i] = comp_step(c, x[i]);
}
