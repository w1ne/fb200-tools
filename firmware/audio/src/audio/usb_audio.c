/* UAC2 audio plumbing: SPSC rings between the TinyUSB audio FIFOs and the
 * engine. Single producer/consumer per ring, main-loop context only. */
#include <string.h>
#include "tusb.h"
#include "usb_descriptors.h"
#include "audio/usb_audio.h"
#include "audio/audio_config.h"
#include "audio/drift.h"
#include "dsp/outq.h"

#define RING_FRAMES 1024
#define TMP_FRAMES 64

static int16_t play_ring[RING_FRAMES * 2];
static uint32_t play_head, play_tail;
static int16_t cap_ring[RING_FRAMES * 2];
static uint32_t cap_head, cap_tail;

static uint8_t alt_spk, alt_mic;
static uint32_t cnt_overflow, cnt_underflow;

/* UAC2 control state. The host (CoreAudio) reads the clock's rate and range
 * and the feature unit's mute/volume at attach; if any request stalls it
 * drops the whole device (seen on macOS 2026-09-27: interfaces present, no
 * audio device). Channel 0 = master, 1..2 = L/R. Volume in 1/256 dB. */
#define SAMPLE_RATE     AUDIO_FS
#define VOL_MIN_DB256   (-60 * 256)
#define VOL_MAX_DB256   0
#define VOL_RES_DB256   256
static uint8_t fu_mute[3];
static int16_t fu_volume[3];
static uint32_t cnt_ctrl_stall;

void usb_audio_init(void)
{
    for (int i = 0; i < 3; i++) { fu_mute[i] = 0; fu_volume[i] = 0; }
    cnt_ctrl_stall = 0;
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

static bool clock_get(uint8_t rhport, tusb_control_request_t const *req)
{
    uint8_t const sel = TU_U16_HIGH(req->wValue);
    if (sel == AUDIO20_CS_CTRL_SAM_FREQ && req->bRequest == AUDIO20_CS_REQ_CUR) {
        audio20_control_cur_4_t cur = {.bCur = (int32_t)tu_htole32(SAMPLE_RATE)};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &cur, sizeof cur);
    }
    if (sel == AUDIO20_CS_CTRL_SAM_FREQ && req->bRequest == AUDIO20_CS_REQ_RANGE) {
        audio20_control_range_4_n_t(1) range = {
            .wNumSubRanges = tu_htole16(1),
            .subrange[0] = {.bMin = SAMPLE_RATE, .bMax = SAMPLE_RATE, .bRes = 0},
        };
        return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &range, sizeof range);
    }
    if (sel == AUDIO20_CS_CTRL_CLK_VALID && req->bRequest == AUDIO20_CS_REQ_CUR) {
        audio20_control_cur_1_t valid = {.bCur = 1};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &valid, sizeof valid);
    }
    return false;
}

static bool feature_get(uint8_t rhport, tusb_control_request_t const *req)
{
    uint8_t const sel = TU_U16_HIGH(req->wValue);
    uint8_t const ch = TU_U16_LOW(req->wValue);
    if (ch > 2u) return false;
    if (sel == AUDIO20_FU_CTRL_MUTE && req->bRequest == AUDIO20_CS_REQ_CUR) {
        audio20_control_cur_1_t mute = {.bCur = fu_mute[ch]};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &mute, sizeof mute);
    }
    if (sel == AUDIO20_FU_CTRL_VOLUME && req->bRequest == AUDIO20_CS_REQ_RANGE) {
        audio20_control_range_2_n_t(1) range = {
            .wNumSubRanges = tu_htole16(1),
            .subrange[0] = {.bMin = tu_htole16(VOL_MIN_DB256), .bMax = tu_htole16(VOL_MAX_DB256),
                            .bRes = tu_htole16(VOL_RES_DB256)},
        };
        return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &range, sizeof range);
    }
    if (sel == AUDIO20_FU_CTRL_VOLUME && req->bRequest == AUDIO20_CS_REQ_CUR) {
        audio20_control_cur_2_t vol = {.bCur = tu_htole16(fu_volume[ch])};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &vol, sizeof vol);
    }
    return false;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *req)
{
    uint8_t const entity = TU_U16_HIGH(req->wIndex);
    bool ok = entity == UAC2_ENTITY_CLOCK ? clock_get(rhport, req)
            : entity == UAC2_ENTITY_SPK_FEATURE_UNIT ? feature_get(rhport, req) : false;
    if (!ok) cnt_ctrl_stall++;
    return ok;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *req, uint8_t *buf)
{
    (void)rhport;
    uint8_t const entity = TU_U16_HIGH(req->wIndex);
    uint8_t const sel = TU_U16_HIGH(req->wValue);
    uint8_t const ch = TU_U16_LOW(req->wValue);
    bool ok = false;
    if (req->bRequest == AUDIO20_CS_REQ_CUR) {
        if (entity == UAC2_ENTITY_CLOCK && sel == AUDIO20_CS_CTRL_SAM_FREQ &&
            req->wLength == sizeof(audio20_control_cur_4_t)) {
            /* Fixed-rate clock: accept only what we run at. */
            ok = ((audio20_control_cur_4_t const *)buf)->bCur == SAMPLE_RATE;
        } else if (entity == UAC2_ENTITY_SPK_FEATURE_UNIT && ch <= 2u) {
            if (sel == AUDIO20_FU_CTRL_MUTE && req->wLength == sizeof(audio20_control_cur_1_t)) {
                fu_mute[ch] = ((audio20_control_cur_1_t const *)buf)->bCur;
                ok = true;
            } else if (sel == AUDIO20_FU_CTRL_VOLUME &&
                       req->wLength == sizeof(audio20_control_cur_2_t)) {
                int16_t v = (int16_t)((audio20_control_cur_2_t const *)buf)->bCur;
                fu_volume[ch] = v < VOL_MIN_DB256 ? VOL_MIN_DB256 : v > VOL_MAX_DB256 ? VOL_MAX_DB256 : v;
                ok = true;
            }
        }
    }
    if (!ok) cnt_ctrl_stall++;
    return ok;
}

bool usb_audio_playing(void)
{
    return alt_spk != 0u;
}

void usb_audio_host_controls(uint8_t *mute, int16_t *volume_db256, uint32_t *stalls)
{
    *mute = (uint8_t)(fu_mute[0] | fu_mute[1] | fu_mute[2]);
    *volume_db256 = fu_volume[0];
    *stalls = cnt_ctrl_stall;
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

static outq_t s_cap_q = {0x6C8E9CF5u, 0};

void usb_audio_set_dither(bool on) { s_cap_q.dither = on; }

void usb_audio_push(const float *src, size_t frames)
{
    if (alt_mic == 0u) {
        return;   /* host not recording: nothing to queue */
    }
    for (size_t i = 0; i < frames; i++) {
        /* rounded, clamped to +-1.0 (v0.9.1 truncated toward zero: dsp/outq.h) */
        int16_t frame[2] = {outq_sample(&s_cap_q, src[i * 2 + 0]),
                            outq_sample(&s_cap_q, src[i * 2 + 1])};
        cap_push(frame);
    }
}

size_t usb_audio_pull_rs(drift_rs_t *rs, int16_t *dst, size_t frames, uint32_t max_fill,
                         uint32_t *inserts, uint32_t *drops)
{
    return drift_rs_pull(rs, play_ring, RING_FRAMES, &play_tail, play_head, dst, frames,
                         max_fill, inserts, drops);
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
