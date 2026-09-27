#include "math.h"
#include "testgen.h"

void testgen_init(testgen_ctx_t *c, float fs)
{
    *c = (testgen_ctx_t){ .mode = TESTGEN_OFF, .amp = 0.0f, .fs = fs, .rng = 0x12345678u };
}

void testgen_set(testgen_ctx_t *c, testgen_mode_t mode, float amp, float freq)
{
    c->mode = mode;
    c->amp = amp;
    c->step = 6.28318530718f * freq / c->fs;
    c->phase = 0.0f;
    c->impulse_countdown = 0;
}

void testgen_process(void *ctx, dsp_block_t *b, size_t n)
{
    testgen_ctx_t *c = ctx;
    if (c->mode == TESTGEN_OFF) return;
    for (size_t i = 0; i < n; i++) {
        float s = 0.0f;
        switch (c->mode) {
        case TESTGEN_SINE:
            s = c->amp * dsp_sinf(c->phase);
            c->phase += c->step;
            if (c->phase > 6.28318530718f) c->phase -= 6.28318530718f;
            break;
        case TESTGEN_WHITE:
            c->rng ^= c->rng << 13; c->rng ^= c->rng >> 17; c->rng ^= c->rng << 5;
            s = c->amp * ((int32_t)c->rng / 2147483648.0f);
            break;
        case TESTGEN_IMPULSE:
            s = (c->impulse_countdown == 0) ? c->amp : 0.0f;
            c->impulse_countdown = (c->impulse_countdown == 0) ? (uint32_t)c->fs : c->impulse_countdown - 1;
            break;
        default: break;
        }
        b->data[0][i] = b->data[1][i] = s;
    }
}
