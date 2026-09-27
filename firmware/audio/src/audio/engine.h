#ifndef FB200_ENGINE_H
#define FB200_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t fifo_drops;   /* playback ring overfull: stale frames discarded */
    uint32_t fifo_inserts; /* playback ring starved: last frame repeated */
    uint32_t dma_errors;   /* SAI FIFO error flags seen */
    uint32_t latency_skips; /* output blocks skipped to bound DAC latency */
    uint32_t dsp_resets;    /* non-finite output -> DSP state reset */
} engine_stats_t;

void engine_init(void);
void engine_task(void);
void engine_get_stats(engine_stats_t *out);

void engine_set_gain_db(float db);
float engine_get_gain_db(void);
void engine_set_testgen(int mode, float amp, float freq); /* 0 off 1 sine 2 white 3 impulse */
void engine_testgen_input(bool on);   /* testgen into the chain input instead of the output */
void engine_profile(void);   /* `prof`: cycles per chain stage */
void engine_cycles(uint32_t *avg, uint32_t *max, uint32_t *budget);   /* since last call */
void engine_set_mute(bool mute);
bool engine_get_mute(void);
void engine_set_meters(bool on);
void engine_drop_tx(uint32_t blocks); /* `x`: stop refilling TX (underrun check) */

/* Meter peaks (updated at ~1 Hz when meters are enabled). */
extern volatile float g_meter_peak[2];

/* Tuner (muted output while on) and drum machine, see dsp/tuner.h, dsp/drums.h. */
#include "dsp/tuner.h"
#include "dsp/drums.h"
void engine_set_tuner(bool on);
bool engine_tuner_poll(tuner_result_t *out);   /* main loop */
drums_t *engine_drums(void);
/* Apply a preset (stock layout) and the master volume 0..100 to the chain. */
#include "preset/preset.h"
void engine_apply_preset(const preset_t *p, unsigned master);
/* Global settings: input gain (S+0x1a), tuner A4 (S+0x2c), tuner mute (S+0x2e). */
void engine_apply_settings(const settings_t *s);
void engine_dsp_reset(void);
bool engine_needs_reapply(void);   /* after a DSP reset: apply the preset again */

#endif
