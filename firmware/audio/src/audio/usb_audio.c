/* UAC2 audio plumbing: SPSC rings between the TinyUSB audio FIFOs and the
 * engine. Single producer/consumer per ring, main-loop context only. */
#include <string.h>
#include "tusb.h"
#include "usb_descriptors.h"
#include "audio/usb_audio.h"

#define RING_FRAMES 1024
#define TMP_FRAMES 64

static int16_t play_ring[RING_FRAMES * 2];
static uint32_t play_head, play_tail;
static int16_t cap_ring[RING_FRAMES * 2];
static uint32_t cap_head, cap_tail;

static uint8_t alt_spk, alt_mic;
static uint32_t cnt_overflow, cnt_underflow;

void usb_audio_init(void)
{
    play_head = play_tail = 0;
    cap_head = cap_tail = 0;
    alt_spk = alt_mic = 0;
    cnt_overflow = cnt_underflow = 0;
}

/* TinyUSB: streaming interface opened/closed. Flush the matching ring. */
bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    (void)rhport;
    uint8_t const itf = (uint8_t)tu_u16_low(p_request->wIndex);
    uint8_t const alt = (uint8_t)tu_u16_low(p_request->wValue);
    if (itf == ITF_NUM_AUDIO_STREAMING_SPK) {
        alt_spk = alt;
        if (alt == 0) {
            play_tail = play_head;
        }
    } else if (itf == ITF_NUM_AUDIO_STREAMING_MIC) {
        alt_mic = alt;
        if (alt == 0) {
            cap_tail = cap_head;
        }
    }
    return true;
}

static void play_push(const int16_t *frame)
{
    uint32_t next = (play_head + 1u) % RING_FRAMES;
    if (next == play_tail) {
        cnt_overflow++;
        return;
    }
    play_ring[play_head * 2 + 0] = frame[0];
    play_ring[play_head * 2 + 1] = frame[1];
    play_head = next;
}

static int play_pop(int16_t *frame)
{
    if (play_tail == play_head) {
        return 0;
    }
    frame[0] = play_ring[play_tail * 2 + 0];
    frame[1] = play_ring[play_tail * 2 + 1];
    play_tail = (play_tail + 1u) % RING_FRAMES;
    return 1;
}

static void cap_push(const int16_t *frame)
{
    uint32_t next = (cap_head + 1u) % RING_FRAMES;
    if (next == cap_tail) {
        cnt_overflow++;
        return;
    }
    cap_ring[cap_head * 2 + 0] = frame[0];
    cap_ring[cap_head * 2 + 1] = frame[1];
    cap_head = next;
}

static int cap_pop(int16_t *frame)
{
    if (cap_tail == cap_head) {
        return 0;
    }
    frame[0] = cap_ring[cap_tail * 2 + 0];
    frame[1] = cap_ring[cap_tail * 2 + 1];
    cap_tail = (cap_tail + 1u) % RING_FRAMES;
    return 1;
}

void usb_audio_task(void)
{
    static int16_t tmp[TMP_FRAMES * 2];

    /* Host playback -> play ring. */
    uint16_t n = tud_audio_read(tmp, sizeof tmp);
    for (uint16_t i = 0; i + 3 < n; i += 4) {
        int16_t frame[2] = {tmp[i / 2], tmp[i / 2 + 1]};
        play_push(frame);
    }

    /* Capture ring -> host. tud_audio_write may accept less than offered;
     * rewind the tail by the unaccepted frames. */
    if (alt_mic == 0) {
        return;
    }
    uint16_t frames = 0;
    int16_t frame[2];
    while (frames < TMP_FRAMES && cap_pop(frame)) {
        tmp[frames * 2 + 0] = frame[0];
        tmp[frames * 2 + 1] = frame[1];
        frames++;
    }
    if (frames == 0) {
        return;
    }
    uint16_t wrote = tud_audio_write(tmp, (uint16_t)(frames * 4));
    uint32_t accepted = wrote / 4u;
    if (accepted < frames) {
        cap_tail = (cap_tail + RING_FRAMES - (frames - accepted)) % RING_FRAMES;
    }
}

size_t usb_audio_pull(float *dst, size_t frames)
{
    size_t n = 0;
    int16_t frame[2];
    while (n < frames && play_pop(frame)) {
        dst[n * 2 + 0] = (float)frame[0] * (1.0f / 32768.0f);
        dst[n * 2 + 1] = (float)frame[1] * (1.0f / 32768.0f);
        n++;
    }
    if (n == 0) {
        cnt_underflow++;
    }
    return n;
}

void usb_audio_push(const float *src, size_t frames)
{
    for (size_t i = 0; i < frames; i++) {
        float l = src[i * 2 + 0];
        float r = src[i * 2 + 1];
        if (l > 1.0f) {
            l = 1.0f;
        } else if (l < -1.0f) {
            l = -1.0f;
        }
        if (r > 1.0f) {
            r = 1.0f;
        } else if (r < -1.0f) {
            r = -1.0f;
        }
        int16_t frame[2] = {(int16_t)(l * 32767.0f), (int16_t)(r * 32767.0f)};
        cap_push(frame);
    }
}

void usb_audio_stats(uint32_t *play_fill, uint32_t *cap_fill, uint32_t *overflow,
                     uint32_t *underflow, uint8_t *spk_alt, uint8_t *mic_alt)
{
    *play_fill = (play_head + RING_FRAMES - play_tail) % RING_FRAMES;
    *cap_fill = (cap_head + RING_FRAMES - cap_tail) % RING_FRAMES;
    *overflow = cnt_overflow;
    *underflow = cnt_underflow;
    *spk_alt = alt_spk;
    *mic_alt = alt_mic;
}
