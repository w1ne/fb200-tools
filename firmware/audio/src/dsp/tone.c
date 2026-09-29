#include <stdint.h>
#include <string.h>
#include "tone.h"
#include "stock_data.h"

#define PRESENCE_STEP 15   /* stock: presence is fixed at knob 50 */

void tone_init(tone_t *t)
{
    memset(t, 0, sizeof *t);
    for (unsigned s = 0; s < TONE_STAGES; s++) t->coeffs[5 * s] = 1.0f;
    arm_biquad_cascade_df1_init_f32(&t->inst, TONE_STAGES, t->coeffs, t->state);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* stock: (int)(knob fraction * 31), in float */
static int step(int knob) { return (int)(knob_fraction(knob) * 31.0f); }

void tone_set(tone_t *t, int bass, int mid, int midfreq, int treble)
{
    const stock_data_t *d = g_stock;
    if (!d) return;                          /* no stock data: stays flat */
    const float *rows[TONE_STAGES] = {
        d->tone_bass[step(bass)],
        d->tone_mid[clampi(midfreq, 0, STOCK_TONE_MID_BANKS - 1)][step(mid)],
        d->tone_presence[PRESENCE_STEP],
        d->tone_treble[step(treble)],
    };
    for (unsigned s = 0; s < TONE_STAGES; s++) memcpy(&t->coeffs[5 * s], rows[s], 5 * sizeof(float));
    t->active = 1;
}

#define DF1 TONE_DF1

/* one stage over the block, two samples per iteration */
static void df1_one(const float *restrict c, float *restrict state, float *restrict x, unsigned n)
{
    float x1 = state[0], x2 = state[1], y1 = state[2], y2 = state[3];
    unsigned i = 0;
    for (; i + 1 < n; i += 2) {
        float xa = x[i], xb = x[i + 1];
        float a = DF1(c, xa, x1, x2, y1, y2);
        float b = DF1(c, xb, xa, x1, a, y1);
        x[i] = a;
        x[i + 1] = b;
        x2 = xa; x1 = xb; y2 = a; y1 = b;
    }
    if (i < n) {
        float xa = x[i], a = DF1(c, xa, x1, x2, y1, y2);
        x[i] = a;
        x2 = x1; x1 = xa; y2 = y1; y1 = a;
    }
    state[0] = x1; state[1] = x2; state[2] = y1; state[3] = y2;
}

static uint32_t bits(float f)
{
    union { float f; uint32_t u; } v = {f};
    return v.u;
}

void tone_df1(const float *restrict coeffs, float *restrict state, unsigned stages,
              float *restrict x, unsigned n)
{
    if (!n) return;
    for (; stages >= 2; stages -= 2, coeffs += 10, state += 8) {
        const float *c = coeffs, *d = coeffs + 5;
        /* Stage B's input history is stage A's output history: {x1, x2} of
         * B = {y1, y2} of A (CMSIS writes the state that way too). Only a
         * state set some other way takes the plain path. */
        if (bits(state[4]) != bits(state[2]) || bits(state[5]) != bits(state[3])) {
            df1_one(c, state, x, n);
            df1_one(d, state + 4, x, n);
            continue;
        }
        /* Skewed: A runs sample i while B runs sample i - 1, so the two
         * recurrences are independent in the loop body and the in-order M7
         * overlaps them. h1, h2, h3 = A(i-1), A(i-2), A(i-3). */
        float ax1 = state[0], ax2 = state[1], h2 = state[2], h3 = state[3];
        float by1 = state[6], by2 = state[7];
        float x0 = x[0], h1 = DF1(c, x0, ax1, ax2, h2, h3);    /* A(0) */
        ax2 = ax1; ax1 = x0;
        unsigned i = 1;
        /* four samples per iteration: every state variable gets a new value
         * (no register moves for the shifts) */
        for (; i + 3 < n; i += 4) {
            float xa = x[i], xb = x[i + 1], xc = x[i + 2], xd = x[i + 3];
            float a0 = DF1(c, xa, ax1, ax2, h1, h2);           /* A(i) */
            float b0 = DF1(d, h1, h2, h3, by1, by2);           /* B(i - 1) */
            float a1 = DF1(c, xb, xa, ax1, a0, h1);            /* A(i + 1) */
            float b1 = DF1(d, a0, h1, h2, b0, by1);            /* B(i) */
            float a2 = DF1(c, xc, xb, xa, a1, a0);
            float b2 = DF1(d, a1, a0, h1, b1, b0);
            float a3 = DF1(c, xd, xc, xb, a2, a1);
            float b3 = DF1(d, a2, a1, a0, b2, b1);
            x[i - 1] = b0;
            x[i] = b1;
            x[i + 1] = b2;
            x[i + 2] = b3;
            ax2 = xc; ax1 = xd; h3 = a1; h2 = a2; h1 = a3; by2 = b2; by1 = b3;
        }
        for (; i < n; i++) {
            float xa = x[i];
            float a0 = DF1(c, xa, ax1, ax2, h1, h2);
            float b0 = DF1(d, h1, h2, h3, by1, by2);
            x[i - 1] = b0;
            ax2 = ax1; ax1 = xa; h3 = h2; h2 = h1; h1 = a0; by2 = by1; by1 = b0;
        }
        float b0 = DF1(d, h1, h2, h3, by1, by2);               /* B(n - 1) */
        x[n - 1] = b0;
        state[0] = ax1; state[1] = ax2; state[2] = h1; state[3] = h2;
        state[4] = h1; state[5] = h2; state[6] = b0; state[7] = by1;
    }
    if (stages) df1_one(coeffs, state, x, n);
}

void tone_process(tone_t *t, float *x, unsigned n)
{
    if (t->active) tone_df1(t->coeffs, t->state, TONE_STAGES, x, n);
}
