#include <string.h>
#include "cold.h"
#include "cab.h"
#include "arm_const_structs.h"
#include "stock_data.h"

void cab_init_long(cab_t *c, conv2_tail_t *tail)
{
    memset(c, 0, sizeof *c);
    (void)conv2_init(&c->conv, tail);        /* unit impulse */
    c->scale = 1.0f;
}

void cab_init(cab_t *c)
{
    memset(c, 0, sizeof *c);
    (void)conv2_init_head(&c->conv);         /* unit impulse; no 512-point FFT linked */
    c->scale = 1.0f;
}

int cab_set_ir_len(cab_t *c, const float *ir, unsigned taps, float gain)
{
    if (conv2_set_ir(&c->conv, ir, taps) != 0) return -1;   /* keeps the input history */
    float scale = (float)((double)gain * 1.15);   /* stock: 1.15 applied in double */
    c->scale_due = conv2_pending(&c->conv);
    if (c->scale_due) c->next_scale = scale;
    else c->scale = scale;
    c->active = 1;
    return 0;
}

void cab_set_ir(cab_t *c, const float *ir, float gain)
{
    (void)cab_set_ir_len(c, ir, CAB_TAPS, gain);
}

COLD int cab_set_model(cab_t *c, int cab)
{
    if (!g_stock || cab < 1 || cab > STOCK_CABS) return -1;
    cab_set_ir(c, g_stock->cab_taps[cab - 1], g_stock->cab_gain[cab - 1]);
    return 0;
}

/* cos/sin(2 pi / 1025) and of twice that angle */
#define HANN_C1 0.9999812119957244
#define HANN_S1 0.006129898495245035
#define HANN_C2 0.999924848688876
#define HANN_S2 0.012259566653371795

COLD float cab_user_ir_gain(const float *ir)
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

/* A long IR's gain takes over with the IR: it swaps at the end of a block
 * (the engine's 32-sample blocks), or in conv2_finish */
static void scale_swap(cab_t *c)
{
    if (c->scale_due && !conv2_pending(&c->conv)) {
        c->scale = c->next_scale;
        c->scale_due = 0;
    }
}

void cab_process(cab_t *c, float *x, unsigned n)
{
    if (!c->active || n == 0) return;
    if (n > DSP_BLOCK) n = DSP_BLOCK;
    scale_swap(c);
    conv2_process(&c->conv, x, x, n);
    arm_scale_f32(x, c->scale, x, n);
    scale_swap(c);
}
