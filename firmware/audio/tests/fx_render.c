/* Host render tool for the stock-effect ports: tests/test_fx_parity.py builds
 * it once per module (-DFX_GATE / -DFX_COMP / -DFX_MOD / -DFX_REVERB) and
 * compares its output with the stock DSP run in emulation.
 *
 *   fx_render <warmup> <p0> [p1 ...] < in.f32 > out.f32
 *
 * Runs `warmup` zero samples first (the stock chain ran that many since its
 * last re-init), then the float32 mono input, in DSP_BLOCK blocks. Output is
 * float32 mono, or interleaved L/R for the reverb. Parameters are stock knob
 * units, in the order of the module's *_set_params(). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsp/dsp.h"

#if defined(FX_GATE)
#include "dsp/gate.h"
static gate_t fx;
#define NPAR 1
static void setup(const unsigned *p) { gate_init(&fx, 44100.0f); gate_set_params(&fx, p[0]); }
#elif defined(FX_COMP)
#include "dsp/comp.h"
static comp_t fx;
#define NPAR 5
static void setup(const unsigned *p)
{
    comp_init(&fx, 44100.0f);
    comp_set_params(&fx, p[0], p[1], p[2], p[3], p[4]);
}
#elif defined(FX_MOD)
#include "dsp/mod.h"
static mod_t fx;
#define NPAR 5
static void setup(const unsigned *p)
{
    mod_init(&fx, 44100.0f);
    mod_set_params(&fx, p[0], p[1], p[2], p[3], p[4]);
}
#elif defined(FX_REVERB)
#include "dsp/reverb.h"
static reverb_t fx;
#define NPAR 5
#define STEREO 1
static void setup(const unsigned *p)
{
    reverb_init(&fx, 44100.0f);
    reverb_set_params(&fx, p[0], p[1], p[2], p[3], p[4]);
}
#else
#error "define one of FX_GATE FX_COMP FX_MOD FX_REVERB"
#endif

static void run(float *x, unsigned n, FILE *out)
{
#ifdef STEREO
    float l[DSP_BLOCK], r[DSP_BLOCK], lr[2 * DSP_BLOCK];
    reverb_process(&fx, x, l, r, n);
    for (unsigned i = 0; i < n; i++) { lr[2 * i] = l[i]; lr[2 * i + 1] = r[i]; }
    if (out) fwrite(lr, sizeof(float), 2 * n, out);
#else
#if defined(FX_GATE)
    gate_process(&fx, x, n);
#elif defined(FX_COMP)
    comp_process(&fx, x, n);
#else
    mod_process(&fx, x, n);
#endif
    if (out) fwrite(x, sizeof(float), n, out);
#endif
}

int main(int argc, char **argv)
{
    if (argc != 2 + NPAR) {
        fprintf(stderr, "usage: %s <warmup> <%d params> < in.f32 > out.f32\n", argv[0], NPAR);
        return 2;
    }
    unsigned long warm = strtoul(argv[1], NULL, 0);
    unsigned p[NPAR];
    for (int i = 0; i < NPAR; i++) p[i] = (unsigned)strtoul(argv[2 + i], NULL, 0);
    setup(p);
    float buf[DSP_BLOCK];
    while (warm) {
        unsigned n = warm < DSP_BLOCK ? (unsigned)warm : DSP_BLOCK;
        memset(buf, 0, sizeof buf);
        run(buf, n, NULL);
        warm -= n;
    }
    size_t n;
    while ((n = fread(buf, sizeof(float), DSP_BLOCK, stdin)) > 0) run(buf, (unsigned)n, stdout);
    return 0;
}
