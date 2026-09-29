#include "drums.h"
#include <string.h>


#define TICKS_PER_BEAT 120u
#define SAMPLES_PER_MIN_X_BEAT 2646000u   /* 60 * 44100 */

static int bank_parse(drums_t *d, const void *bank)
{
    const uint32_t *h = (const uint32_t *)bank;
    d->n_samples = 0;
    if (!h) return -1;
    uint32_t n = h[0];
    if (n == 0 || n > DRUMS_MAX_SAMPLES || h[1] != 0) return -1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t page = h[2 + 2 * i], bytes = h[3 + 2 * i];
        if (page == 0 || bytes == 0 || (bytes & 3)) return -1;
        d->smp[i] = (const float *)((const uint8_t *)bank + page * 512u);
        d->smp_len[i] = bytes / 4u;
    }
    d->n_samples = n;
    return 0;
}

static unsigned pattern_of(const drums_t *d)
{
    if (d->count_in > 0) return 79u + (unsigned)d->count_in;
    return d->data->rhythm[d->rhythm];
}

static const uint32_t *pattern_start(const drums_t *d, unsigned p)
{
    uint32_t off = 0;
    for (unsigned i = 0; i < p; i++) off += d->data->lens[i];
    return d->data->events + off;
}

static void voices_reset(drums_t *d)
{
    memset(d->v, 0, sizeof d->v);
    d->rr = 0;
}

/* stock 0xbfd0 */
static void tempo_apply(drums_t *d)
{
    unsigned bpb = d->data->beats[d->pattern];
    d->frac = 0.0f;
    d->spt = (float)(22050.0 / (double)d->bpm);          /* 120 ticks per beat */
    d->bar_samples = bpb * SAMPLES_PER_MIN_X_BEAT / d->bpm;
    d->beat_half = (d->bar_samples / bpb) >> 1;
    if (bpb == 6) d->beat_half >>= 1;
    d->beat_ctr = 0;
    d->cur_bpm = d->bpm;
}

/* stock 0x17394, once per sample */
static void tick(drums_t *d)
{
    d->pattern = (int)pattern_of(d);
    if (!d->started) {
        d->ev = d->start = pattern_start(d, (unsigned)d->pattern);
        d->until = 0;
        d->elapsed = 0;
        d->frac = 0.0f;
        d->beat_ctr = 0;
        d->started = 1;
    }
    if (d->spt > 0.0f) {
        d->frac += 1.0f;
        if (d->frac >= d->spt) {
            d->frac -= d->spt;
            d->elapsed++;
        }
    }
    if (++d->beat_ctr > d->beat_half) {
        d->beat_ctr = 0;
        d->beat_flag = 1;
    }
}

/* stock 0x17810 */
static void trigger(drums_t *d, unsigned sample, unsigned vel)
{
    if (sample >= d->n_samples) return;
    drums_voice_t *v = &d->v[d->rr];
    v->trig = 1;
    v->sample = (uint8_t)sample;
    v->gain = (float)vel / 127.0f;
    v->len = d->smp_len[sample];
    if (++d->rr >= DRUMS_VOICES) d->rr = 0;
}

/* stock 0xb3ac, once per sub-block */
static void sequencer(drums_t *d)
{
    if (d->pattern != d->cur_pattern) {
        voices_reset(d);
        d->cur_pattern = d->pattern;
        d->ev = d->start = pattern_start(d, (unsigned)d->pattern);
        d->until = 0;
        d->elapsed = 0;
        tempo_apply(d);
    }
    if (d->bpm != d->cur_bpm) tempo_apply(d);
    while (d->until <= d->elapsed) {
        uint32_t w = *d->ev;
        if ((w & 0xff00u) == 0xff00u) {           /* end of list: loop */
            d->ev = d->start;
            d->until = 0;
            d->elapsed = 0;
            d->frac = 0.0f;
            d->beat_ctr = 0;
            if (d->count_in > 0) d->count_in = 0; /* count-in plays once */
            return;
        }
        unsigned note = (w >> 8) & 0xffu;
        if (note) trigger(d, note - 1u, w & 0xffu);
        d->ev++;
        d->until += *d->ev >> 16;
    }
    d->until -= d->elapsed;
    d->elapsed = 0;
}

/* stock 0x2708: a triggered voice starts on the next sub-block */
static void mixer(drums_t *d, float *out, unsigned n)
{
    for (unsigned k = 0; k < n; k++) out[k] = 0.0f;
    for (unsigned i = 0; i < DRUMS_VOICES; i++) {
        drums_voice_t *v = &d->v[i];
        if (v->trig) {
            if (v->active) v->pos = 0;
            v->active = 1;
            v->trig = 0;
            continue;
        }
        if (!v->active) continue;
        if (v->pos >= v->len) {
            v->pos = 0;
            v->active = 0;
            continue;
        }
        uint32_t cnt = v->len - v->pos;
        if (cnt > n) cnt = n;
        if (!d->no_flash) {   /* no flash read while the flash is busy */
            const float *s = d->smp[v->sample] + v->pos;
            for (uint32_t k = 0; k < cnt; k++) out[k] += s[k] * v->gain;
        }
        v->pos += n;
    }
}

static void render_sub(drums_t *d, float *out)
{
    if (!d->on || !d->data || !d->n_samples) {
        for (unsigned k = 0; k < DRUMS_SUBBLOCK; k++) out[k] = 0.0f;
        return;
    }
    for (unsigned k = 0; k < DRUMS_SUBBLOCK; k++) tick(d);
    sequencer(d);
    mixer(d, out, DRUMS_SUBBLOCK);
}

void drums_data_from_stock(drums_data_t *out, const stock_data_t *s)
{
    *out = (drums_data_t){
        s->drum_events, STOCK_DRUM_EVENTS, s->drum_lens, STOCK_DRUM_PATTERNS,
        s->drum_rhythm, STOCK_DRUM_RHYTHMS, s->drum_beats,
    };
}

int drums_init(drums_t *d, const void *bank, const drums_data_t *data)
{
    memset(d, 0, sizeof *d);
    d->data = data;
    d->bpm = DRUMS_BPM_DEFAULT;
    d->level = DRUMS_LEVEL_DEFAULT;
    d->cur_pattern = -1;
    d->sub_pos = DRUMS_SUBBLOCK;
    int ok = bank_parse(d, bank);
    if (!d->data || d->data->n_rhythms < DRUMS_RHYTHMS || d->data->n_patterns < 90) {
        d->data = 0;
        ok = -1;
    }
    return ok;
}

void drums_start(drums_t *d)
{
    d->on = 1;
    d->started = 0;          /* stock re-init on the first tick */
    d->cur_pattern = -1;     /* always start the list from the top */
}

void drums_stop(drums_t *d)
{
    d->on = 0;
    d->count_in = 0;
}

void drums_set_tempo(drums_t *d, unsigned bpm)
{
    if (bpm < DRUMS_BPM_MIN) bpm = DRUMS_BPM_MIN;
    if (bpm > DRUMS_BPM_MAX) bpm = DRUMS_BPM_MAX;
    d->bpm = (uint16_t)bpm;
}

void drums_set_rhythm(drums_t *d, unsigned rhythm)
{
    d->rhythm = (uint8_t)(rhythm < DRUMS_RHYTHMS ? rhythm : DRUMS_RHYTHMS - 1);
}

void drums_set_level(drums_t *d, unsigned level)
{
    d->level = (uint8_t)(level > 100 ? 100 : level);
}

void drums_count_in(drums_t *d, unsigned beats)
{
    if (beats < 1) beats = 1;
    if (beats > 9) beats = 9;
    d->count_in = (int)beats;
}

void drums_tap(drums_t *d, uint32_t now_ms)
{
    uint32_t dt = now_ms - d->last_tap_ms;
    d->last_tap_ms = now_ms;
    if (dt == 0 || dt > 3000u) return;             /* first tap / too slow */
    drums_set_tempo(d, 60000u / dt);
}

void drums_render(drums_t *d, float *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (d->sub_pos >= DRUMS_SUBBLOCK) {
            render_sub(d, d->sub);
            d->sub_pos = 0;
        }
        out[i] = d->sub[d->sub_pos++];
    }
}

void drums_process(drums_t *d, float *mix, size_t n)
{
    float lvl = (float)d->level;
    for (size_t i = 0; i < n; i++) {
        if (d->sub_pos >= DRUMS_SUBBLOCK) {
            render_sub(d, d->sub);
            d->sub_pos = 0;
        }
        mix[i] += d->sub[d->sub_pos++] * lvl * 0.02f * 0.8f;
    }
}

void drums_process_stereo(drums_t *d, float *l, float *r, size_t n)
{
    float lvl = (float)d->level;
    for (size_t i = 0; i < n; i++) {
        if (d->sub_pos >= DRUMS_SUBBLOCK) {
            render_sub(d, d->sub);
            d->sub_pos = 0;
        }
        float v = d->sub[d->sub_pos++] * lvl * 0.02f * 0.8f;
        l[i] += v;
        r[i] += v;
    }
}
