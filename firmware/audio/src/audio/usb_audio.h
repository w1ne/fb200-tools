#ifndef FB200_USB_AUDIO_H
#define FB200_USB_AUDIO_H

#include <stddef.h>
#include <stdbool.h>
#include "audio/drift.h"
#include <stdint.h>

void usb_audio_init(void);
void usb_audio_task(void); /* pump TinyUSB audio FIFOs; call from the main loop */

/* Engine side: host playback -> engine, engine -> host capture. */
size_t usb_audio_pull(float *dst, size_t frames);     /* interleaved stereo */
void usb_audio_push(const float *src, size_t frames); /* interleaved stereo */
void usb_audio_set_dither(bool on);   /* TPDF dither on the 16-bit capture (dsp/outq.h) */

/* The host playback, resampled to the codec clock (audio/drift.h drift_rs). */
size_t usb_audio_pull_rs(drift_rs_t *rs, int16_t *dst, size_t frames, uint32_t max_fill,
                         uint32_t *inserts, uint32_t *drops);

void usb_audio_stats(uint32_t *play_fill, uint32_t *cap_fill, uint32_t *overflow,
                     uint32_t *underflow, uint8_t *spk_alt, uint8_t *mic_alt);

/* UAC2 feature-unit state set by the host (master channel volume in 1/256 dB)
 * and the count of class requests we had to stall. */
bool usb_audio_playing(void);   /* host has the playback interface open */
void usb_audio_host_controls(uint8_t *mute, int16_t *volume_db256, uint32_t *stalls);

#endif
