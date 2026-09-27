#include <string.h>
#include "cab.h"
#include "arm_const_structs.h"
#include "stock_dsp_data.h"

void cab_init(cab_t *c)
{
    memset(c, 0, sizeof *c);
    c->coeffs[CAB_TAPS - 1] = 1.0f;          /* unit impulse */
    c->scale = 1.0f;
    arm_fir_init_f32(&c->fir, CAB_TAPS, c->coeffs, c->state, DSP_BLOCK);
}

void cab_set_ir(cab_t *c, const float *ir, float gain)
{
    for (unsigned i = 0; i < CAB_TAPS; i++) c->coeffs[i] = ir[CAB_TAPS - 1 - i];
    c->scale = (float)((double)gain * 1.15);  /* stock: 1.15 applied in double */
    c->active = 1;
}

int cab_set_model(cab_t *c, int cab)
{
#ifdef FB200_STOCK_DSP
    if (cab < 1 || cab > STOCK_CABS) return -1;
    cab_set_ir(c, stock_cab_taps[cab - 1], stock_cab_gain[cab - 1]);
    return 0;
#else
    (void)c; (void)cab;
    return -1;
#endif
}

/* cos/sin(2 pi / 1025) and of twice that angle */
#define HANN_C1 0.9999812119957244
#define HANN_S1 0.006129898495245035
#define HANN_C2 0.999924848688876
#define HANN_S2 0.012259566653371795

float cab_user_ir_gain(const float *ir)
{
    float buf[2 * 512], mag[85];
    /* stock window table: 0.5 - 0.5 cos(2 pi m / 1025) rounded to 6 decimals;
     * sample j uses m = 2j + 1. The angle is advanced by rotation (no libm). */
    double re = HANN_C1, im = HANN_S1;
    for (unsigned j = 0; j < 512; j++) {
        double w = 0.5 - 0.5 * re;
        w = (double)(int)(w * 1e6 + 0.5) / 1e6;
        buf[2 * j] = ir[j] * (float)w;
        buf[2 * j + 1] = 0.0f;
        double r = re * HANN_C2 - im * HANN_S2;
        im = re * HANN_S2 + im * HANN_C2;
        re = r;
    }
    arm_cfft_f32(&arm_cfft_sR_f32_len512, buf, 0, 1);
    arm_cmplx_mag_f32(buf, mag, 85);
    float sum = 0.0f;
    for (int k = 1; k <= 85; k++) {
        float root;
        arm_sqrt_f32(mag[k - 1], &root);
        sum += root * (float)(30 / k + 1);
    }
    return sum == 0.0f ? 1.0f : 100.0f / sum;
}

void cab_process(cab_t *c, float *x, unsigned n)
{
    if (!c->active || n == 0) return;
    if (n > DSP_BLOCK) n = DSP_BLOCK;
    float y[DSP_BLOCK];
    arm_fir_f32(&c->fir, x, y, n);
    arm_scale_f32(y, c->scale, x, n);
}
