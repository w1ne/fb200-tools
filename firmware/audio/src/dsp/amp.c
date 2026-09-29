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

/* odd waveshaper, 254 steps over |v| <= 1, linear interpolation. Selects,
 * no branches: the M7 then overlaps several of these. */
static inline float shape(const float *ws, float v)
{
    int pos = v >= 0.0f;
    float s = pos ? 1.0f : -1.0f;
    float m = pos ? v : -v;
    m = m > 1.0f ? 1.0f : m;
    m *= 254.0f;
    int i = (int)m, j = (int)(m + 1.0f);
    float f = m - (float)i;
    return (ws[i] * (1.0f - f) + ws[j] * f) * s;
}

/* 3x oversampling (prev, prev + d, prev + 2d, d = (x - prev) / 3), the
 * waveshaper, the anti-alias biquad at 3 fs and decimation (every third),
 * fused and skewed: iteration i filters sample i's three values while it
 * shapes sample i + 1's and divides for sample i + 2, so the filter's
 * recurrence and the divide overlap independent work. Same operations in
 * the same order as shaping, filtering (CMSIS df1) and decimating in turn.
 * Indices past the block are clamped: those results are never used. */
static void oversample(amp_t *a, const float *ws, float *x, unsigned n)
{
    const float *c = a->aa.pCoeffs;
    float *st = a->aa_state;
    float x1 = st[0], x2 = st[1], y1 = st[2], y2 = st[3];
    unsigned last = n - 1;
    float prev = a->prev, d = (x[0] - prev) / 3.0f, v = prev + d;
    float s0 = shape(ws, prev), s1 = shape(ws, v), s2 = shape(ws, v + d);
    d = (x[last ? 1 : 0] - x[0]) / 3.0f;                 /* sample 1 */
    for (unsigned i = 0; i < n; i++) {
        float xi = x[i], w = xi + d;                      /* sample i + 1 */
        float t0 = shape(ws, xi), t1 = shape(ws, w), t2 = shape(ws, w + d);
        unsigned k1 = i + 1 < last ? i + 1 : last, k2 = i + 2 < last ? i + 2 : last;
        float dn = (x[k2] - x[k1]) / 3.0f;                /* sample i + 2 */
        float o0 = TONE_DF1(c, s0, x1, x2, y1, y2);
        float o1 = TONE_DF1(c, s1, s0, x1, o0, y1);
        float o2 = TONE_DF1(c, s2, s1, s0, o1, o0);
        x2 = s1; x1 = s2; y2 = o1; y1 = o2;
        x[i] = o2;
        prev = xi;
        s0 = t0; s1 = t1; s2 = t2; d = dn;
    }
    a->prev = prev;
    st[0] = x1; st[1] = x2; st[2] = y1; st[3] = y2;
}

void amp_process(amp_t *a, float *x, unsigned n)
{
    const stock_amp_model_t *m = a->m;
    if (!m || n == 0) return;
    if (n > DSP_BLOCK) n = DSP_BLOCK;
    float vol[DSP_BLOCK];

    /* the smoothers' target terms are the same product every sample */
    const float td = a->drv_target * 0.001f, tv = a->vol_target * 0.0005f;
    const float ds2 = m->drive_scale2, ds = m->drive_scale, pg = m->pre_gain;
    float drv = a->drv, vl = a->vol;
    for (unsigned i = 0; i < n; i++) {
        drv = td + drv * 0.999f;
        vl = tv + vl * 0.9995f;
        vol[i] = vl;
        x[i] = pg * (ds2 * drv * ds * x[i]);
    }
    a->drv = drv;
    a->vol = vl;
    tone_df1(a->pre.pCoeffs, a->pre_state, STOCK_AMP_SOS, x, n);
    oversample(a, m->ws, x, n);
    tone_df1(a->post.pCoeffs, a->post_state, STOCK_AMP_SOS, x, n);
    const float og = m->out_gain, level2 = m->level * 2.0f;
    for (unsigned i = 0; i < n; i++) x[i] = x[i] * og * level2 * vol[i];
    tone_process(&a->tone, x, n);
}
