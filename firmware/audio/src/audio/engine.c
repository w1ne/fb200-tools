/* Audio engine. Milestone-1: the codec's ADC -> (DSP chain TBD) -> codec DAC,
 * with the USB path as monitor/capture. Until the DSP chain lands the guitar
 * passes through unchanged. */
#include <string.h>
#include "audio/engine.h"
#include "audio/sai.h"
#include "audio/usb_audio.h"

#define ENGINE_FRAMES 64

void engine_init(void)
{
    usb_audio_init();
    sai_audio_init();
}

void engine_task(void)
{
    int16_t in[ENGINE_FRAMES * 2];
    float fb[ENGINE_FRAMES * 2];
    int16_t out[ENGINE_FRAMES * 2];

    /* Codec ADC -> engine. */
    size_t n = sai_pull(in, ENGINE_FRAMES);
    if (n != 0) {
        for (size_t i = 0; i < n * 2; i++) {
            fb[i] = (float)in[i] * (1.0f / 32768.0f);
        }
        usb_audio_push(fb, n);
        (void)sai_push(in, n); /* pass-through until the DSP chain lands */
    }

    /* Host playback monitors into the DAC path. */
    size_t m = usb_audio_pull(fb, ENGINE_FRAMES);
    if (m != 0) {
        for (size_t i = 0; i < m; i++) {
            float l = fb[i * 2 + 0], r = fb[i * 2 + 1];
            if (l > 1.0f) l = 1.0f; else if (l < -1.0f) l = -1.0f;
            if (r > 1.0f) r = 1.0f; else if (r < -1.0f) r = -1.0f;
            out[i * 2 + 0] = (int16_t)(l * 32767.0f);
            out[i * 2 + 1] = (int16_t)(r * 32767.0f);
        }
        (void)sai_push(out, m);
    }
}
