#ifndef FB200_DSP_H
#define FB200_DSP_H
#include <stddef.h>

#define DSP_BLOCK 32
#define DSP_CHANNELS 2
#define DSP_MAX_NODES 8

typedef struct { float data[DSP_CHANNELS][DSP_BLOCK]; } dsp_block_t;

typedef void (*dsp_process_fn)(void *ctx, dsp_block_t *b, size_t n);

typedef struct { dsp_process_fn process; void *ctx; } dsp_node_t;

typedef struct { dsp_node_t nodes[DSP_MAX_NODES]; size_t count; } dsp_chain_t;

static inline void dsp_chain_add(dsp_chain_t *c, dsp_process_fn fn, void *ctx)
{
    if (c->count < DSP_MAX_NODES) c->nodes[c->count++] = (dsp_node_t){fn, ctx};
}

static inline void dsp_chain_run(const dsp_chain_t *c, dsp_block_t *b, size_t n)
{
    for (size_t i = 0; i < c->count; i++) c->nodes[i].process(c->nodes[i].ctx, b, n);
}

/* one-pole smoothing for click-free parameter changes */
typedef struct { float target, current, coeff; } dsp_smooth_t;

static inline void dsp_smooth_init(dsp_smooth_t *s, float value, float coeff)
{
    s->target = s->current = value;
    s->coeff = coeff;
}

static inline void dsp_smooth_set(dsp_smooth_t *s, float value) { s->target = value; }

static inline float dsp_smooth_next(dsp_smooth_t *s)
{
    s->current += s->coeff * (s->target - s->current);
    return s->current;
}

/* The stock knob smoother (FB200 callback 0x17d8c): every 3rd sample
 * y = t * a + y * b in float, (a, b) = (0.01, 0.99) or (0.001, 0.999).
 * Same arithmetic as the stock, so it settles on the same float (which is not
 * exactly t). The first set after init snaps. */
typedef struct { float y, t, a, b; unsigned n; int fresh; } dsp_knob_t;

static inline void dsp_knob_init(dsp_knob_t *k, float a, float b)
{
    *k = (dsp_knob_t){ .a = a, .b = b, .fresh = 1 };
}

static inline void dsp_knob_set(dsp_knob_t *k, float t)
{
    k->t = t;
    if (k->fresh) k->y = t;
    k->fresh = 0;
}

static inline float dsp_knob_next(dsp_knob_t *k)
{
    if (++k->n >= 3) {
        k->n = 0;
        k->y = k->t * k->a + k->y * k->b;
    }
    return k->y;
}
#endif
