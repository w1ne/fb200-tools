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
/* One pass of the main loop's audio work (usb_audio_task + engine_task) for
 * a flash write that blocks the main loop (debug/flash_rmw.h flash_pump):
 * the flash is busy, so the drums (samples in flash) advance silently.
 * Nothing before engine_init. */
void engine_pump(void);
void engine_get_stats(engine_stats_t *out);

void engine_set_gain_db(float db);
float engine_get_gain_db(void);
void engine_set_testgen(int mode, float amp, float freq); /* 0 off 1 sine 2 white 3 impulse */
void engine_set_dither(bool on);      /* TPDF dither on the DAC and USB capture (dsp/outq.h), off by default */
bool engine_get_dither(void);
void engine_testgen_input(bool on);   /* testgen into the chain input instead of the output */
/* Host USB playback routing (console `usb out|in|mix`). OUT (default, the
 * stock): mixed into the DAC after the capture tap. IN (reamping): replaces
 * the instrument at the chain input, so it reaches the effects, the DAC and
 * the capture. MIX: summed with the instrument at the chain input. */
enum { ENGINE_USB_OUT = 0, ENGINE_USB_IN = 1, ENGINE_USB_MIX = 2 };
void engine_set_usb_route(int route);
int engine_get_usb_route(void);
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
float engine_input_peak(void);   /* chain input peak since the last call (main loop) */
drums_t *engine_drums(void);
/* Our EQ (dsp/eq.h), for the console `eq` (main loop, as engine_task). */
struct eq_s;
struct eq_s *engine_eq(void);
/* Apply a preset (stock layout) and the master volume 0..100 to the chain. */
#include "preset/preset.h"
void engine_apply_preset(const preset_t *p, unsigned master);
/* Global settings: input gain (S+0x1a), tuner A4 (S+0x2c), tuner mute (S+0x2e). */
void engine_apply_settings(const settings_t *s);
void engine_dsp_reset(void);
bool engine_needs_reapply(void);   /* after a DSP reset: apply the preset again */
/* User IR staging in taps, a RAM budget (engine.c; memory map:
 * docs/FIRMWARE_BRINGUP.md). 512 (CAB_TAPS, the stock and user slot size) =
 * long IRs off: no conv2 tail (~96 kB), no 512-point FFT tables (~4.9 kB);
 * `cab long` over 512 is "not available". Up to 4096 (CAB_MAX_TAPS) = long
 * IRs on (docs/PARITY.md M5). */
#define ENGINE_IR_TAPS 4096
/* `cab long <taps>`: synthetic long IR in the cab (0: the preset's cab). 0 on
 * success, -2 when taps is over what this build has RAM for (long IRs off:
 * 512, engine.c ENGINE_IR_TAPS), -1 on other bad taps. */
int engine_cab_long(unsigned taps);
/* Looper (dsp/looper.h), after the reverb; the loop is in the flash
 * (loopstore/loopstore.h). engine_loop: a LOOPER_* action, looper_cmd's
 * result. engine_loop_poll: main loop, every pass (finishes fades).
 * engine_loop_task: main loop, every pass: the flash side (streams, erase
 * ahead). engine_loop_busy: it has work now (the main loop must not sleep).
 * engine_loop_writing: a record or a dub is writing (flash_store waits; RAM
 * code). engine_loop_arm: start erasing ahead (looper mode on). */
#include "dsp/looper.h"
#include "loopstore/loopstore.h"
int engine_loop(int action);
void engine_loop_poll(void);
void engine_loop_info(looper_info_t *out);
void engine_loop_level(unsigned pct);  /* 0..100 */
void engine_loop_arm(void);
void engine_loop_task(void);
int engine_loop_busy(void);
int engine_loop_writing(void);
typedef struct {
    ls_info_t store;
    const loopio_t *io;
    uint32_t cut;
} engine_loop_stats_t;
void engine_loop_stats(engine_loop_stats_t *out);   /* console `loop stats` */

#endif
