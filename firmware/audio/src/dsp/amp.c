#include <string.h>
#include "amp.h"

void amp_init(amp_t *a, float fs)
{
    memset(a, 0, sizeof *a);
    a->fs = fs;
    /* drive and volume ramp up from 0 like the stock (and stall a few ulps
     * below target like its float32 smoothers) */
    tone_init(&a->tone);
}

int amp_set_model(amp_t *a, int model)
{
    if (!g_stock || model < 1 || model > STOCK_AMP_MODELS) return -1;
    a->m = &g_stock->amp_models[model - 1];
    memset(a->pre_state, 0, sizeof a->pre_state);
    memset(a->post_state, 0, sizeof a->post_state);
    memset(a->aa_state, 0, sizeof a->aa_state);
    a->prev = 0.0f;
    arm_biquad_cascade_df1_init_f32(&a->pre, STOCK_AMP_SOS, &a->m->pre[0][0], a->pre_state);
    arm_biquad_cascade_df1_init_f32(&a->post, STOCK_AMP_SOS, &a->m->post[0][0], a->post_state);
    arm_biquad_cascade_df1_init_f32(&a->aa, 1, g_stock->amp_aa, a->aa_state);
    return 0;
}

void amp_set_params(amp_t *a, int gain, int bass, int mid, int midfreq, int treble, int volume)
{
    float g = knob_fraction(gain);
    a->drv_target = g < 0.5f ? 0.05f + g * 1.9f : 1.0f + (g - 0.5f) * 10.0f;
    a->vol_target = knob_fraction(volume);
    tone_set(&a->tone, bass, mid, midfreq, treble);
}

/* odd waveshaper, 254 steps over |v| <= 1, linear interpolation */
static inline float shape(const float *ws, float v)
{
    float s = v >= 0.0f ? 1.0f : -1.0f;
    float m = v >= 0.0f ? v : -v;
    if (m > 1.0f) m = 1.0f;
    m *= 254.0f;
    int i = (int)m, j = (int)(m + 1.0f);
    float f = m - (float)i;
    return (ws[i] * (1.0f - f) + ws[j] * f) * s;
}

void amp_process(amp_t *a, float *x, unsigned n)
{
    const stock_amp_model_t *m = a->m;
    if (!m || n == 0) return;
    if (n > DSP_BLOCK) n = DSP_BLOCK;
    float vol[DSP_BLOCK], os[3 * DSP_BLOCK];

    for (unsigned i = 0; i < n; i++) {
        a->drv = a->drv_target * 0.001f + a->drv * 0.999f;
        a->vol = a->vol_target * 0.0005f + a->vol * 0.9995f;
        vol[i] = a->vol;
        x[i] = m->pre_gain * (m->drive_scale2 * a->drv * m->drive_scale * x[i]);
    }
    arm_biquad_cascade_df1_f32(&a->pre, x, x, n);

    for (unsigned i = 0; i < n; i++) {           /* 3x: prev, prev + d, prev + 2d */
        float v = a->prev, d = (x[i] - a->prev) / 3.0f;
        for (unsigned k = 0; k < 3; k++) {
            os[3 * i + k] = shape(m->ws, v);
            v += d;
        }
        a->prev = x[i];
    }
    arm_biquad_cascade_df1_f32(&a->aa, os, os, 3 * n);
    for (unsigned i = 0; i < n; i++) x[i] = os[3 * i + 2];

    arm_biquad_cascade_df1_f32(&a->post, x, x, n);
    float level2 = m->level * 2.0f;
    for (unsigned i = 0; i < n; i++) x[i] = x[i] * m->out_gain * level2 * vol[i];
    tone_process(&a->tone, x, n);
}
