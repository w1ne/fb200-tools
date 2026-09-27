#ifndef FB200_ENGINE_H
#define FB200_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t fifo_drops;   /* playback ring overfull: stale frames discarded */
    uint32_t fifo_inserts; /* playback ring starved: last frame repeated */
    uint32_t dma_errors;   /* SAI FIFO error flags seen */
} engine_stats_t;

void engine_init(void);
void engine_task(void);
void engine_get_stats(engine_stats_t *out);

void engine_set_gain_db(float db);
float engine_get_gain_db(void);
void engine_set_testgen(int mode, float amp, float freq); /* 0 off 1 sine 2 white 3 impulse */
void engine_set_mute(bool mute);
bool engine_get_mute(void);
void engine_set_meters(bool on);
void engine_drop_tx(uint32_t blocks); /* `x`: stop refilling TX (underrun check) */

/* Meter peaks (updated at ~1 Hz when meters are enabled). */
extern volatile float g_meter_peak[2];

#endif
