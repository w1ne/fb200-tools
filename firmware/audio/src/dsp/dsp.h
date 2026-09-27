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
#endif
