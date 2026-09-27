/* Audio engine. Milestone-1 placeholder: USB playback loops back to USB
 * capture so the UAC2 path is testable before the codec/SAI exist. Tasks 6-8
 * replace this with codec in -> DSP chain -> codec out, with the USB capture
 * fed from the processed signal. */
#include "audio/engine.h"
#include "audio/usb_audio.h"

#define ENGINE_FRAMES 64

void engine_init(void)
{
    usb_audio_init();
}

void engine_task(void)
{
    float buf[ENGINE_FRAMES * 2];
    size_t n = usb_audio_pull(buf, ENGINE_FRAMES);
    if (n == 0) {
        return;
    }
    usb_audio_push(buf, n);
}
