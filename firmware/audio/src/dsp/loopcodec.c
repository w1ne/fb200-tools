/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

/* The looper's frame codec: see loopcodec.h. On the audio path (ITCM). */
#include <string.h>
#include "loopcodec.h"

/* 2^(q/4) for q = 0..3, and the thresholds between the quarter octaves:
 * a mantissa m in [1, 2) needs step code c = the smallest with 2^(c/4) >= m */
static const float kQuarter[4] = {1.0f, 1.18920712f, 1.41421356f, 1.68179283f};

static inline float pow2i(int e)   /* 2^e, -126 <= e <= 127 */
{
    union { uint32_t u; float f; } v = {.u = (uint32_t)(e + 127) << 23};
    return v.f;
}

/* step for code k: 2^((k - LC_K0) / 4) */
static inline float step(int k)
{
    int q = k - LC_K0 + 4 * 64;         /* >= 0: floor division by 4 */
    return pow2i((q >> 2) - 64) * kQuarter[q & 3];
}

void lc_encode(const float *x, uint8_t *out)
{
    float pk = 0.0f;
    for (int i = 0; i < LC_N; i++) {
        float a = x[i] < 0.0f ? -x[i] : x[i];
        if (a > pk) pk = a;
    }
    if (!(pk > 0.0f) || pk > 1e30f) {   /* silence (and NaN or inf: silence too) */
        memset(out, 0, LC_BYTES);
        return;
    }
    /* want s >= pk / LC_MAX: pk / LC_MAX = mant * 2^e, mant in [1, 2) */
    union { float f; uint32_t u; } v = {.f = pk * (1.0f / LC_MAX)};
    int e = (int)((v.u >> 23) & 0xFFu) - 127;
    v.u = (v.u & 0x007FFFFFu) | 0x3F800000u;   /* mant */
    int c = 0;
    while (c < 4 && (c == 0 ? v.f > 1.0f : v.f > kQuarter[c])) c++;
    int k = 4 * e + c + LC_K0;
    if (k < 1) k = 1;                   /* below 2^-40: rounds to 0 anyway */
    if (k > LC_KMAX) k = LC_KMAX;
    float s = step(k), is = 1.0f / s;
    out[0] = (uint8_t)k;
    uint8_t *p = out + 1;
    for (int i = 0; i < LC_N; i += 4) {
        uint32_t w[4];
        for (int j = 0; j < 4; j++) {
            float t = x[i + j] * is;
            int m = (int)(t < 0.0f ? t - 0.5f : t + 0.5f);
            m = m > LC_MAX ? LC_MAX : m < -LC_MAX ? -LC_MAX : m;
            w[j] = (uint32_t)m & 0x3FFu;
        }
        uint32_t lo = w[0] | w[1] << 10 | w[2] << 20 | w[3] << 30;
        p[0] = (uint8_t)lo;
        p[1] = (uint8_t)(lo >> 8);
        p[2] = (uint8_t)(lo >> 16);
        p[3] = (uint8_t)(lo >> 24);
        p[4] = (uint8_t)(w[3] >> 2);
        p += 5;
    }
}

void lc_decode(const uint8_t *in, float *y)
{
    float s = in[0] > LC_KMAX ? 0.0f : step(in[0]);   /* erased flash: silence */
    const uint8_t *p = in + 1;
    for (int i = 0; i < LC_N; i += 4) {
        uint32_t lo = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
                      (uint32_t)p[3] << 24;
        uint32_t w3 = lo >> 30 | (uint32_t)p[4] << 2;
        int32_t m0 = (int32_t)(lo << 22) >> 22, m1 = (int32_t)(lo << 12) >> 22;
        int32_t m2 = (int32_t)(lo << 2) >> 22, m3 = (int32_t)(w3 << 22) >> 22;
        y[i] = (float)m0 * s;
        y[i + 1] = (float)m1 * s;
        y[i + 2] = (float)m2 * s;
        y[i + 3] = (float)m3 * s;
        p += 5;
    }
}
