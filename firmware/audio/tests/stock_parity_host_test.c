/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

/* Stock parity harness for amp/tone/cab, built with the extracted stock data
 * (the stock data blob in $FB200_STOCK_BLOB, stock_host.h) by
 * tests/test_stock_dsp_parity.py, which compares the output against the
 * emulated stock DSP (firmware/tools/stock_render.py).
 *
 *   render IN.f32 OUT model gain bass mid midfreq treble volume cab
 *       amp (+tone stack) -> OUT.amp.f32, then cab (0 = none) -> OUT.f32,
 *       DSP_BLOCK samples per call, after WARMUP zero samples so the drive
 *       and volume smoothers settle (they start at 0, like the stock)
 *   irgain IRS.f32 OUT.txt
 *       cab_user_ir_gain() for each 512-sample IR in IRS.f32, one per line */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsp/amp.h"
#include "dsp/cab.h"
#include "stock_host.h"

#define WARMUP 40000

static float *read_f32(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    long bytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    float *p = malloc((size_t)bytes + DSP_BLOCK * sizeof(float));
    *n = fread(p, sizeof(float), (size_t)bytes / sizeof(float), f);
    fclose(f);
    return p;
}

static void write_f32(const char *path, const float *p, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(p, sizeof(float), n, f) != n) { perror(path); exit(2); }
    fclose(f);
}

static int render(char **av)
{
    static amp_t amp;
    static cab_t cab;
    size_t n;
    float *x = read_f32(av[0], &n);
    int model = atoi(av[2]), cabno = atoi(av[9]);
    amp_init(&amp, 44100.0f);
    if (amp_set_model(&amp, model) != 0) { fprintf(stderr, "bad model %d\n", model); return 2; }
    amp_set_params(&amp, atoi(av[3]), atoi(av[4]), atoi(av[5]), atoi(av[6]), atoi(av[7]),
                   atoi(av[8]));
    cab_init(&cab);
    if (cabno && cab_set_model(&cab, cabno) != 0) { fprintf(stderr, "bad cab %d\n", cabno); return 2; }
    float zero[DSP_BLOCK];
    for (int i = 0; i < WARMUP; i += DSP_BLOCK) {
        memset(zero, 0, sizeof zero);
        amp_process(&amp, zero, DSP_BLOCK);
        cab_process(&cab, zero, DSP_BLOCK);
    }
    float *y = malloc(n * sizeof(float) + sizeof zero);
    for (size_t i = 0; i < n; i += DSP_BLOCK) {
        unsigned len = n - i < DSP_BLOCK ? (unsigned)(n - i) : DSP_BLOCK;
        amp_process(&amp, x + i, len);
        memcpy(y + i, x + i, len * sizeof(float));
        cab_process(&cab, x + i, len);
    }
    char path[1024];
    snprintf(path, sizeof path, "%s.amp.f32", av[1]);
    write_f32(path, y, n);
    snprintf(path, sizeof path, "%s.f32", av[1]);
    write_f32(path, x, n);
    printf("rendered %zu samples\n", n);
    return 0;
}

static int irgain(char **av)
{
    size_t n;
    float *irs = read_f32(av[0], &n);
    FILE *f = fopen(av[1], "w");
    if (!f) { perror(av[1]); return 2; }
    for (size_t i = 0; i + CAB_TAPS <= n; i += CAB_TAPS)
        fprintf(f, "%.9g\n", cab_user_ir_gain(irs + i));
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    stock_from_env();
    if (!g_stock) { fprintf(stderr, "FB200_STOCK_BLOB not set\n"); return 2; }
    if (argc == 12 && strcmp(argv[1], "render") == 0) return render(argv + 2);
    if (argc == 4 && strcmp(argv[1], "irgain") == 0) return irgain(argv + 2);
    fprintf(stderr, "usage: see the header of stock_parity_host_test.c\n");
    return 2;
}
