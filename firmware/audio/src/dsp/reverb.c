/* Stock FB200 reverb port. Stock addresses in comments refer to the vendor
 * image's ITCM code and its state block at RAM 0x20010cb4 ("S+off").
 * The operation order (float vs double, multiply-add grouping) follows the
 * stock code so the host build (-ffp-contract=off) is bit-exact at 44.1 kHz. */
#include <string.h>
#include "reverb.h"

/* LFO wavetables read by the stock helpers 0xb4e4 (table 1, RAM 0x20017048)
 * and 0xc044 (table 3, RAM 0x20017848): 256 points, linear interpolation.
 * Rebuilt from fitted formulas (lfo_tables_init), so no stock data is needed:
 * table 1 within 5e-6, table 3 within 9e-7 of the stock values. */
static float LFO1[256], LFO3[256];

/* double sin/cos for coefficient design (no libm in the firmware);
 * |x| <= pi / 2 is all we need, Taylor to x^19 gives ~1e-16 */
static double dsin(double x)
{
    double x2 = x * x, t = x, s = x;
    for (int k = 1; k < 10; k++) { t *= -x2 / ((2 * k) * (2 * k + 1)); s += t; }
    return s;
}

static double dcos(double x) { return dsin(1.5707963267948966 - x); }

static void lfo_tables_init(void)
{
    /* table 1: triangle, slope 1/58.75, parabolic caps where |n - 64| < 10.5 */
    const double a = 1.0 / 58.75;
    /* table 3: a hand-shaped "sine": lines of slope 4/255 joined by cosine
     * arcs R cos(w (n - c)) - 2 (R = 2.681175, one w per arc), with three
     * odd single points (+-0.396234) at the joins, as in the stock table */
    const double R = 2.681175;
    for (int n = 0; n < 256; n++) {
        int k = n & 127;
        if (k > 64) k = 128 - k;
        double v = k <= 53 ? a * k : 1.0 - (a / 21.0) * (64 - k) * (64 - k);
        LFO1[n] = (float)(n < 128 ? v : -v);
        double u;
        if (n <= 36) u = 2.0 - R * dcos(0.012573363 * n);
        else if (n == 37 || n == 219) u = -0.396234;
        else if (n <= 88) u = (4.0 * (n - 64) + 1.0) / 255.0;
        else if (n == 89) u = 0.396234;
        else if (n <= 165) u = R * dcos(0.012083488 * (n - 127.5)) - 2.0;
        else if (n <= 218) u = (99.0 - 4.0 * (n - 166)) / 255.0;
        else u = 2.0 - R * dcos(0.012922629 * (n - 255));
        LFO3[n] = (float)u;
    }
}

static double round6(double v)   /* nearest 1e-6, as the stock table was printed */
{
    int q = v >= 0 ? (int)(v * 1e6 + 0.5) : -(int)(-v * 1e6 + 0.5);   /* |v| < 2 */
    return (double)q / 1e6;
}

typedef struct {
    unsigned short comb[6], echo;
    unsigned char hp1;            /* S+0x10: coefficient set of the float HP biquad */
    float damp_a, damp_b;         /* comb low-pass: lp = out * a + lp * b */
    double fb0, fbk;              /* feedback = f32(fb0 + decay * fbk) */
    double tone_k;                /* tone = f32((double)0.15f + tone * tone_k) */
    float center, depth, dscale;  /* tap = (lfo * dscale) * depth + center */
    float rate;                   /* LFO phase step */
    double wet, dry;              /* tap mix: f32(tap * wet + x * dry) */
    float gain;                   /* out = x + (level * lp) * gain */
    int out_double;               /* Mod: out = f32((double)x + (double)(level * lp) * 1.4) */
} type_cfg_t;

static const type_cfg_t cfg[REVERB_TYPES] = {
    /* Room 0xdbd8 */
    { {1107, 1133, 1165, 1218, 1237, 1272}, 355, 1, 0.78f, 0x1.c28f6p-3f,
      0.83, 0.068, 0.8, 400.0f, 10.0f, 1.0f, 5e-05f, 0.4, 0.6, 1.5f, 0 },
    /* Hall 0xc0d8 */
    { {1205, 1229, 1271, 1318, 1339, 1367}, 355, 0, 0.82f, 0.18f,
      0.9, 0.075, 0.8, 500.0f, 60.0f, 1.0f, 3.5e-05f, 0.35, 0.65, 1.0f, 0 },
    /* Plate 0xd2f8 */
    { {887, 910, 945, 967, 995, 1025}, 243, 2, 0.821f, 0x1.6e979p-3f,
      0.89, 0.074, 0.8, 450.0f, 20.0f, 1.0f, 5e-05f, 0.35, 0.65, 1.4f, 0 },
    /* Spring 0xe350: no modulated delay; drip instead */
    { {887, 930, 975, 1027, 1053, 1085}, 313, 0, 0.81f, 0.19f,
      0.9, 0.065, 0.78, 0, 0, 0, 0, 0, 0, 1.0f, 0 },
    /* Mod 0xc9c0: rate 0.323834 * 0.0002, depth 0.322833 * 90 */
    { {1205, 1229, 1271, 1318, 1339, 1367}, 355, 1, 0.8f, 0x1.999998p-3f,
      0.9, 0.08, 0.8, 500.0f, 90.0f, 0.322833f, 0.323834f * 0.0002f, 0.5, 0.5, 0, 1 },
};

/* float high-pass biquads at RAM 0x20010df0 (b0 b1 b2 a1 a2, y = b.x - a.y) */
static const float hp1_coef[3][5] = {
    {0.983610988f, -1.94084406f, 0.965586007f, -1.94084406f, 0.949196994f},
    {0.953480005f, -1.87607396f, 0.933355987f, -1.87607396f, 0.886835992f},
    {0.960125029f, -1.92025006f, 0.960125029f, -1.91868997f, 0.921809971f},
};

/* double high-pass (~20 Hz): y = b0 x - b1 x1 + b2 x2 + a1 y1 - a2 y2 */
static const double hp2_room[5] = {0.995865236078914, 1.97171418181162, 0.975990631693455,
                                   1.971643674462973, 0.971926375121015};
static const double hp2_spring[5] = {0.982489825913, 1.964979651826, 0.982489825913,
                                     1.96495472171, 0.965004581943};

static unsigned scaled(float scale, unsigned n, unsigned max)
{
    unsigned v = scale == 1.0f ? n : (unsigned)((float)n * scale + 0.5f);
    return v > max ? max : (v ? v : 1);
}

/* stock 0xb4e4 / 0xc044: clamp to [0, 1], 255 steps, wrap to entry 0 */
static float lfo(const float *tab, float ph)
{
    if (ph > 1.0f) ph = 1.0f;
    if (ph < 0.0f) ph = 0.0f;
    float t = ph * 255.0f;
    int i = (int)t;
    float a = tab[i], b = i < 255 ? tab[i + 1] : tab[0];
    float frac = t - (float)i;
    return a * (1.0f - frac) + b * frac;
}

/* linear-interpolated read `d` samples behind the write index (already advanced) */
static float mdl_read(const float *buf, unsigned w, unsigned len, float d)
{
    float pos = (float)w - d;
    if (pos < 0.0f) pos += (float)len;
    int i = (int)pos;
    float frac = pos - (float)i, next = pos + 1.0f;
    int j = next >= (float)len ? 0 : (int)next;
    return buf[i] * (1.0f - frac) + buf[j] * frac;
}

static void clear(reverb_t *r)   /* what the stock re-init 0x6630 zeroes */
{
    memset(r->comb, 0, sizeof r->comb);
    memset(r->echo, 0, sizeof r->echo);
    memset(r->mdl, 0, sizeof r->mdl);
    memset(r->comb_idx, 0, sizeof r->comb_idx);
    memset(r->comb_out, 0, sizeof r->comb_out);
    memset(r->comb_lp, 0, sizeof r->comb_lp);
    memset(r->echo_idx, 0, sizeof r->echo_idx);
    memset(r->lp, 0, sizeof r->lp);
    memset(r->drip_y, 0, sizeof r->drip_y);
    r->drip_fb = r->drip_x1 = r->drip_x2 = 0.0f;
}

static void configure(reverb_t *r)
{
    const type_cfg_t *c = &cfg[r->type];
    for (int k = 0; k < 6; k++) r->comb_len[k] = scaled(r->scale, c->comb[k], REVERB_COMB_MAX);
    r->echo_len = scaled(r->scale, c->echo, REVERB_ECHO_MAX);
    r->mdl_len = scaled(r->scale, 600, REVERB_MDL_MAX);
    r->center = c->center * r->scale;
    r->depth = c->depth * r->scale;
    r->rate = c->rate / r->scale;
}

void reverb_init(reverb_t *r, float fs)
{
    memset(r, 0, sizeof *r);
    r->fs = fs;
    r->scale = fs / 44100.0f;
    dsp_knob_init(&r->level, 0.01f, 0.99f);
    r->fresh = 1;
    lfo_tables_init();
    /* Spring drip: 128 RBJ allpass biquads, f0 = 2000..6000 Hz, Q = 0.8,
     * coefficients rounded to 6 decimals (stock table RAM 0x20017c78) */
    for (int k = 0; k < 128; k++) {
        double w = 6.283185307179586 * (2000.0 + k * 4000.0 / 127.0) / fs;
        double al = dsin(w) / 1.6;
        double a1 = round6(-2.0 * dcos(w) / (1.0 + al)), a2 = round6((1.0 - al) / (1.0 + al));
        float *cf = r->drip_coef[k];
        cf[0] = (float)a2; cf[1] = (float)a1; cf[2] = 1.0f; cf[3] = (float)a1; cf[4] = (float)a2;
    }
    configure(r);
}

void reverb_set_params(reverb_t *r, unsigned type, unsigned level, unsigned decay,
                       unsigned tone, unsigned p_a8)
{
    (void)p_a8;   /* not read by the stock audio path (see reverb.h) */
    if (type >= REVERB_TYPES) type = REVERB_ROOM;
    if (type != r->type && !r->fresh) {
        r->type = type;
        clear(r);
        configure(r);
    } else if (r->fresh) {
        r->type = type;
        configure(r);
    }
    r->fresh = 0;
    const type_cfg_t *c = &cfg[type];
    float t = (float)tone * 0.01f, d = (float)decay * 0.01f;
    r->tone = (float)((double)0.15f + (double)t * c->tone_k);
    r->fb = (float)(c->fb0 + (double)d * c->fbk);
    dsp_knob_set(&r->level, (float)level * 0.01f);
}

/* Spring's input-level follower and drip filter (0xe350: 0xe424..0xe4a4 and
 * 0xe854..0xea08). Returns the third allpass output. */
static float drip(reverb_t *r, float x, int stage)
{
    if (stage == 0) {                        /* before the combs */
        float a = x * 32768.0f;
        a = a < 0.0f ? -a : a;
        if (a > r->drip_peak) r->drip_peak = a;
        if (r->drip_count == 2500) {         /* hold the 2500-sample peak */
            r->drip_hold = r->drip_peak;
            r->drip_peak = 0.0f;
            r->drip_count = 0;
        }
        r->drip_count++;
        if (a > r->drip_hold) r->drip_hold = a;
        float e = r->drip_env1 < r->drip_hold ? r->drip_hold : r->drip_env1;
        e -= 1.0f;                           /* 1 LSB (of 32768) per sample */
        r->drip_env1 = e < 0.0f ? 0.0f : e;
        return 0.0f;
    }
    float over = r->drip_env1 - 9000.0f;
    if (over < 0.0f) over = 0.0f;
    if (over > 20000.0f) over = 20000.0f;
    float s = over / 25000.0f;
    if (s > r->drip_env2) r->drip_env2 = s;
    if (!(s >= r->drip_env2)) {
        r->drip_env2 = (float)((double)r->drip_env2 - 3e-06);
    }
    if (r->drip_env2 < 0.0f) r->drip_env2 = 0.0f;
    r->drip_phase = (float)((double)r->drip_phase + 0.00016 / (double)r->scale);
    if (r->drip_phase >= 1.0f) r->drip_phase -= 1.0f;
    float e = r->drip_env2;
    float sw = (lfo(LFO3, r->drip_phase) + 1.0f) * (e * 2.0f - e * e);
    int k = (int)(sw * 63.0f);
    k = k < 0 ? 0 : (k > 127 ? 127 : k);
    const float *c = r->drip_coef[k];
    float in = (float)((double)x + (double)r->drip_fb * 0.65);
    float (*y)[2] = r->drip_y;
    float y1 = c[0] * in + r->drip_x1 * c[1] + r->drip_x2 * c[2] - y[0][0] * c[3] - y[0][1] * c[4];
    float y2 = c[0] * y1 + y[0][0] * c[1] + y[0][1] * c[2] - y[1][0] * c[3] - y[1][1] * c[4];
    float y3 = c[0] * y2 + y[1][0] * c[1] + y[1][1] * c[2] - y[2][0] * c[3] - y[2][1] * c[4];
    r->drip_x2 = r->drip_x1; r->drip_x1 = in;
    y[0][1] = y[0][0]; y[0][0] = y1;
    y[1][1] = y[1][0]; y[1][0] = y2;
    y[2][1] = y[2][0]; y[2][0] = y3;
    r->drip_fb = y1;                         /* feedback taps the first stage */
    return y3;
}

static float hp1(reverb_bq_state_t *s, const float *c, float x)
{
    float y = s->x1 * c[1] + x * c[0] + s->x2 * c[2] - s->y1 * c[3] - s->y2 * c[4];
    s->x2 = s->x1; s->x1 = x; s->y2 = s->y1; s->y1 = y;
    return y;
}

static float hp2(reverb_bq_state_t *s, const double *c, float x)
{
    double d = (double)x * c[0];
    d = d - (double)s->x1 * c[1];
    d = d + (double)s->x2 * c[2];
    d = d + (double)s->y1 * c[3];
    d = d - (double)s->y2 * c[4];
    float y = (float)d;
    s->x2 = s->x1; s->x1 = x; s->y2 = s->y1; s->y1 = y;
    return y;
}

static float sample(reverb_t *r, float x, float *out_r)
{
    const type_cfg_t *c = &cfg[r->type];
    const int spring = r->type == REVERB_SPRING;
    float level = dsp_knob_next(&r->level);
    if (spring) drip(r, x, 0);

    /* 6 combs: 0-2 fed by L, 3-5 by R (mono here) */
    float o[6];
    for (int k = 0; k < 6; k++) {
        float lp = r->comb_out[k] * c->damp_a + r->comb_lp[k] * c->damp_b;
        r->comb_lp[k] = lp;
        unsigned i = r->comb_idx[k];
        r->comb[k][i] = x + r->fb * lp;          /* stock also scales by 1.0 (0x200088e8) */
        if (++i >= r->comb_len[k]) i = 0;
        r->comb_idx[k] = i;
        o[k] = r->comb_out[k] = r->comb[k][i];
    }
    float a = (o[0] + o[2] + o[4]) / 3.0f, b = (o[1] + o[3] + o[5]) / 3.0f;
    float ab[2] = {a, b}, e[2];
    for (int ch = 0; ch < 2; ch++) {             /* y = x + 0.2 x[n - D] */
        unsigned i = r->echo_idx[ch];
        r->echo[ch][i] = ab[ch] * 0.2f;
        if (++i >= r->echo_len) i = 0;
        r->echo_idx[ch] = i;
        e[ch] = r->echo[ch][i] + ab[ch];
    }

    float wl = e[0], wr = e[1];
    if (!spring) {                               /* modulated stereo delay */
        float ph = r->phase + r->rate;
        if (ph >= 1.0f) ph -= 1.0f;
        r->phase = ph;
        float d1 = lfo(LFO1, ph) * c->dscale * r->depth + r->center;
        unsigned w = r->mdl_idx;
        r->mdl[0][w] = e[0];
        r->mdl[1][w] = e[1];
        if (++w >= r->mdl_len) w = 0;
        r->mdl_idx = w;
        float t1 = mdl_read(r->mdl[0], w, r->mdl_len, d1);
        wl = (float)((double)t1 * c->wet + (double)e[0] * c->dry);
        float d2 = lfo(LFO3, ph) * c->dscale * -r->depth + r->center;
        float t2 = mdl_read(r->mdl[1], w, r->mdl_len, d2);
        wr = (float)((double)t2 * c->wet + (double)e[1] * c->dry);
    }

    const double *h2 = spring ? hp2_spring : hp2_room;
    float hl = hp2(&r->hp2[0], h2, hp1(&r->hp1[0], hp1_coef[c->hp1], wl));
    float hr = hp2(&r->hp2[1], h2, hp1(&r->hp1[1], hp1_coef[c->hp1], wr));

    float t = r->tone, it = 1.0f - t;
    if (spring) {
        double dr = (double)drip(r, x, 1) * 0.55;
        r->lp[1] = (float)((double)(it * r->lp[1]) + ((double)hr + dr) * (double)t);
        r->lp[0] = (float)((double)(it * r->lp[0]) + ((double)hl + dr) * (double)t);
    } else {
        r->lp[1] = hr * t + r->lp[1] * it;
        r->lp[0] = hl * t + r->lp[0] * it;
    }
    float sl = level * r->lp[0], sr = level * r->lp[1];
    if (c->out_double) {
        *out_r = (float)((double)x + (double)sr * 1.4);
        return (float)((double)x + (double)sl * 1.4);
    }
    *out_r = x + sr * c->gain;
    return x + sl * c->gain;
}

void reverb_process(reverb_t *r, const float *in, float *out_l, float *out_r, unsigned n)
{
    for (unsigned i = 0; i < n; i++) out_l[i] = sample(r, in[i], &out_r[i]);
}
