#ifndef FB200_DSP_GATE_H
#define FB200_DSP_GATE_H
/* Noise gate: peak envelope with separate open/close thresholds
 * (hysteresis), hold time and smoothed gain (attack/release). */
typedef struct {
    float fs, env, gain;
    float open_lin, close_lin;
    float env_rel, att_coeff, rel_coeff;
    unsigned hold, hold_left;
    int open;
} gate_t;

void gate_init(gate_t *g, float fs);
void gate_set(gate_t *g, float threshold_db, float hysteresis_db, float attack_ms,
              float hold_ms, float release_ms);
void gate_process(gate_t *g, float *x, unsigned n);   /* in place */
#endif
