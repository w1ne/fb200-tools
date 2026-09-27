#include "gate.h"
#include "math.h"

static float ms_coeff(float fs, float ms)
{
    /* one-pole coefficient reaching ~63% in `ms`: 1 - e^(-1/(fs*t)) */
    float n = fs * ms * 0.001f;
    return n < 1.0f ? 1.0f : 1.0f - dsp_exp2f(-1.44269504f / n);
}

void gate_init(gate_t *g, float fs)
{
    g->fs = fs;
    g->env = 0.0f;
    g->gain = 1.0f;
    g->open = 1;
    g->hold_left = 0;
    gate_set(g, -60.0f, 6.0f, 1.0f, 50.0f, 100.0f);
}

void gate_set(gate_t *g, float threshold_db, float hysteresis_db, float attack_ms,
              float hold_ms, float release_ms)
{
    g->open_lin = dsp_db_to_gain(threshold_db);
    g->close_lin = dsp_db_to_gain(threshold_db - hysteresis_db);
    g->att_coeff = ms_coeff(g->fs, attack_ms);
    g->rel_coeff = ms_coeff(g->fs, release_ms);
    g->env_rel = ms_coeff(g->fs, 10.0f);
    g->hold = (unsigned)(g->fs * hold_ms * 0.001f);
}

void gate_process(gate_t *g, float *x, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        float a = x[i] < 0.0f ? -x[i] : x[i];
        g->env = a > g->env ? a : g->env + g->env_rel * (a - g->env);
        if (g->env >= g->open_lin) {
            g->open = 1;
            g->hold_left = g->hold;
        } else if (g->env < g->close_lin) {
            if (g->hold_left) g->hold_left--;
            else g->open = 0;
        }
        float target = g->open ? 1.0f : 0.0f;
        g->gain += (g->open ? g->att_coeff : g->rel_coeff) * (target - g->gain);
        x[i] *= g->gain;
    }
}
