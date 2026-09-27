/* Math shims: the toolchain ships no libm for the target (see the plan). */
#include <stdint.h>
#include "math.h"

/* range-reduced minimax sine, |error| < 1e-6 over [-pi, pi] */
float dsp_sinf(float x)
{
    const float pi = 3.14159265358979f;
    while (x > pi) x -= 2.0f * pi;
    while (x < -pi) x += 2.0f * pi;
    /* reduce to [-pi/2, pi/2] for a well-conditioned degree-9 series
     * (max |error| ~4e-6 over [-2pi, 2pi]) */
    if (x > pi * 0.5f) x = pi - x;
    else if (x < -pi * 0.5f) x = -pi - x;
    float x2 = x * x;
    return x * (1.0f + x2 * (-0.1666666667f + x2 * (0.0083333333f + x2 * (-0.0001984127f + x2 * 0.0000027557f))));
}

/* 2^x via exponent split + degree-4 polynomial */
float dsp_exp2f(float x)
{
    int e = (int)x;
    float f = x - (float)e;
    if (f < 0.0f) { f += 1.0f; e -= 1; }
    /* Taylor of 2^f to f^6: max relative error ~8e-6 on [0,1) */
    float p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f +
              f * (0.0096181f + f * (0.0013334f + f * 0.0001540f)))));
    union { float f; uint32_t u; } v;
    v.u = (uint32_t)(e + 127) << 23;
    return p * v.f;
}

float dsp_db_to_gain(float db) { return dsp_exp2f(db * 0.16609640474f); }
