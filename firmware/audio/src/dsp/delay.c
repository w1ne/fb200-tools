/* Bass delay: see delay.h. */
#include <string.h>
#include "delay.h"
#include "math.h"

#define PI_F 3.14159265358979f
#define TINY 1e-20f                     /* filter states below this are flushed */

static unsigned knob(unsigned k) { return k > 100u ? 100u : k; }

/* 16-bit float, see delay.h: code = sign << 15 | e << 12 | m. e = 0: the
 * value is m * 2^-17 (truncated); e = 1..7: (4096 + m) * 2^(e - 18), the
 * float's mantissa rounded to 12 bits (to nearest even; a carry moves into
 * e, and past e = 7 saturates). The codes are monotonic in |v|. */
uint16_t delay_enc(float v)
{
    union { float f; uint32_t u; } b = {v};
    uint32_t sign = (b.u >> 16) & 0x8000u;
    uint32_t mag = b.u & 0x7FFFFFFFu;
    if (mag >= 0x40800000u) return (uint16_t)(sign | 0x7FFFu);    /* >= 4.0, inf, NaN */
    int fe = (int)(mag >> 23) - 127;                               /* |v| = 1.m 2^fe */
    uint32_t code;
    if (fe < -5) {                                                 /* linear: step 2^-17 */
        b.u = mag;
        code = (uint32_t)(b.f * 131072.0f);                        /* truncated */
    } else {
        uint32_t mant = mag & 0x7FFFFFu;
        code = ((uint32_t)(fe + 6) << 12) | (mant >> 11);
        uint32_t rem = mant & 0x7FFu;
        if (rem > 0x400u || (rem == 0x400u && (code & 1u))) code++;
        if (code > 0x7FFFu) code = 0x7FFFu;
    }
    return (uint16_t)(sign | code);
}

float delay_dec(uint16_t c)
{
    uint32_t e = (c >> 12) & 7u, m = c & 0xFFFu;
    union { float f; uint32_t u; } b;
    if (e == 0) b.f = (float)m * (1.0f / 131072.0f);
    else b.u = ((e - 6u + 127u) << 23) | (m << 11);
    b.u |= (uint32_t)(c & 0x8000u) << 16;
    return b.f;
}

float delay_lowcut_hz(unsigned lowcut)
{
    lowcut = knob(lowcut);
    return lowcut ? 20.0f * dsp_exp2f((float)lowcut * 0.01f * 4.6438562f) : 0.0f;   /* 25^(k/100) */
}

static float tone_hz(unsigned tone)
{
    return 1000.0f * dsp_exp2f((float)knob(tone) * 0.01f * 3.3219281f);             /* 10^(k/100) */
}

/* RBJ high-pass, Q = 1/sqrt(2) (Butterworth, no peak: the feedback loop gain
 * stays <= fb at every frequency). cos w = 1 - 2 sin^2(w/2) keeps the
 * low cut-offs accurate. b1 = -2 b0 exactly: a true zero at DC. */
static void design_hp(delay_t *dl, float fc)
{
    float w = 2.0f * PI_F * fc / dl->fs;
    float s = dsp_sinf(0.5f * w);
    float c = 1.0f - 2.0f * s * s;
    float alpha = dsp_sinf(w) * 0.70710678f;
    float a0 = 1.0f + alpha;
    dl->hb0 = 0.5f * (1.0f + c) / a0;
    dl->hb1 = -2.0f * dl->hb0;
    dl->hb2 = dl->hb0;
    dl->ha1 = -2.0f * c / a0;
    dl->ha2 = (1.0f - alpha) / a0;
}

void delay_clear(delay_t *dl)
{
    memset(dl->line, 0, DELAY_LEN * sizeof dl->line[0]);
    dl->hx1 = dl->hx2 = dl->hy1 = dl->hy2 = dl->lz = 0.0f;
    dl->fresh = 1;
}

void delay_init(delay_t *dl, float fs, int16_t *line)
{
    memset(dl, 0, sizeof *dl);
    dl->line = line;
    if (fs > (float)DELAY_FS_MAX) fs = (float)DELAY_FS_MAX;
    dl->fs = fs;
    dl->d_c = 1.0f / (0.03f * fs);      /* time glide: ~30 ms time constant */
    delay_clear(dl);
    delay_set_params(dl, DELAY_DEF_MS, DELAY_DEF_FB, DELAY_DEF_MIX, DELAY_DEF_LOWCUT, DELAY_DEF_TONE);
    dl->fresh = 1;                      /* the first set after init snaps too */
}

void delay_set_params(delay_t *dl, unsigned time_ms, unsigned fb, unsigned mix, unsigned lowcut,
                      unsigned tone)
{
    if (time_ms < DELAY_MS_MIN) time_ms = DELAY_MS_MIN;
    if (time_ms > DELAY_MS_MAX) time_ms = DELAY_MS_MAX;
    fb = knob(fb); mix = knob(mix); lowcut = knob(lowcut); tone = knob(tone);
    dl->d_t = (float)(unsigned)((float)time_ms * dl->fs * 0.001f + 0.5f);
    dl->fb_t = (float)fb * 0.0095f;
    dl->mix_t = (float)mix * 0.01f;
    if (dl->fresh || lowcut != dl->cut_k) {
        dl->hp_on = lowcut != 0;
        if (dl->hp_on) design_hp(dl, delay_lowcut_hz(lowcut));
    }
    if (dl->fresh || tone != dl->tone_k) {
        dl->lp_on = tone < 100u;
        float w = 2.0f * PI_F * tone_hz(tone) / dl->fs;
        dl->la = w / (1.0f + w);        /* one-pole, matched at low w */
    }
    if (dl->fresh) {
        dl->d = dl->d_t;
        dl->fb = dl->fb_t;
        dl->mix = dl->mix_t;
        dl->fresh = 0;
    }
    dl->time_ms = time_ms; dl->fb_k = fb; dl->mix_k = mix; dl->cut_k = lowcut; dl->tone_k = tone;
}

static float glide(float y, float t, float c, float snap)
{
    float e = t - y;
    return (e < snap && e > -snap) ? t : y + c * e;
}

void delay_process(delay_t *dl, float *x, unsigned n)
{
    const float g = 1.0f / (0.01f * dl->fs);     /* gain smoothing, ~10 ms */
    float d = dl->d, fb = dl->fb, mix = dl->mix;
    float hx1 = dl->hx1, hx2 = dl->hx2, hy1 = dl->hy1, hy2 = dl->hy2, lz = dl->lz;
    unsigned w = dl->w;
    int16_t *line = dl->line;
    const int hp = dl->hp_on, lp = dl->lp_on;
    const float b0 = dl->hb0, b1 = dl->hb1, b2 = dl->hb2, a1 = dl->ha1, a2 = dl->ha2, la = dl->la;
    const float d_t = dl->d_t, d_c = dl->d_c, fb_t = dl->fb_t, mix_t = dl->mix_t;
    for (unsigned i = 0; i < n; i++) {
        {   /* time glide; near the target at least 0.01 sample per sample: a
             * plain one-pole stalls ~1 sample short (float d ~ 44100) */
            float e = d_t - d, st = d_c * e;
            if (e <= 0.01f && e >= -0.01f) d = d_t;
            else d += st > 0.01f || st < -0.01f ? st : e > 0.0f ? 0.01f : -0.01f;
        }
        fb = glide(fb, fb_t, g, 1e-6f);
        mix = glide(mix, mix_t, g, 1e-6f);
        float rp = (float)w - d;
        if (rp < 0.0f) rp += (float)DELAY_LEN;
        unsigned i0 = (unsigned)rp;
        float fr = rp - (float)i0;
        unsigned i1 = i0 + 1u == DELAY_LEN ? 0u : i0 + 1u;
        float a = delay_dec((uint16_t)line[i0]);
        float y = a + fr * (delay_dec((uint16_t)line[i1]) - a);
        float v = x[i] + fb * y;
        if (hp) {
            float h = b0 * v + b1 * hx1 + b2 * hx2 - a1 * hy1 - a2 * hy2;
            hx2 = hx1; hx1 = v; hy2 = hy1; hy1 = h;
            v = h;
        }
        if (lp) {
            lz += la * (v - lz);
            v = lz;
        }
        line[w] = v == v ? (int16_t)delay_enc(v) : 0;   /* 16-bit float (delay.h) */
        if (++w == DELAY_LEN) w = 0;
        x[i] += mix * y;
    }
    /* decaying filter states would go subnormal on silence */
    if (hy1 < TINY && hy1 > -TINY) hy1 = 0.0f;
    if (hy2 < TINY && hy2 > -TINY) hy2 = 0.0f;
    if (hx1 < TINY && hx1 > -TINY) hx1 = 0.0f;
    if (hx2 < TINY && hx2 > -TINY) hx2 = 0.0f;
    if (lz < TINY && lz > -TINY) lz = 0.0f;
    dl->d = d; dl->fb = fb; dl->mix = mix;
    dl->hx1 = hx1; dl->hx2 = hx2; dl->hy1 = hy1; dl->hy2 = hy2; dl->lz = lz;
    dl->w = w;
}
