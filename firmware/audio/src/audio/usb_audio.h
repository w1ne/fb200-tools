#ifndef FB200_USB_AUDIO_H
#define FB200_USB_AUDIO_H

#include <stddef.h>
#include <stdint.h>

void usb_audio_init(void);
void usb_audio_task(void); /* pump TinyUSB audio FIFOs; call from the main loop */

/* Engine side: host playback -> engine, engine -> host capture. */
size_t usb_audio_pull(float *dst, size_t frames);     /* interleaved stereo */
void usb_audio_push(const float *src, size_t frames); /* interleaved stereo */

void usb_audio_stats(uint32_t *play_fill, uint32_t *cap_fill, uint32_t *overflow,
                     uint32_t *underflow, uint8_t *spk_alt, uint8_t *mic_alt);

#endif
