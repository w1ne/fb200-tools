#ifndef FB200_DSP_LOOPCODEC_H
#define FB200_DSP_LOOPCODEC_H
/* The flash looper's sample format (dsp/looper.h): NICAM-like block
 * floating point. One frame = LC_N samples at the loop's rate (22.05 kHz)
 * in LC_BYTES bytes:
 *
 *   byte 0      scale code k: step s = 2^((k - LC_K0) / 4) (quarter-octave
 *               steps); k = 0 is silence (s = 2^-40)
 *   bytes 1..40 LC_N mantissas m, 10-bit two's complement, -511..511, packed
 *               little-endian, 4 samples in 5 bytes
 *
 * x = m * s. The encoder picks the smallest s with max|x| <= 511 s: the
 * quantization error is at most s/2 and follows the block's level, so the
 * SNR of a sine is 60..63 dB whatever its level (tests/looper_host_test.c).
 * Every frame decodes on its own (no state): the loop can start, wrap and
 * be rewritten at any frame. All-zero bytes decode to silence, and so does
 * erased flash (k = 0xFF is never written: LC_KMAX). */
#include <stdint.h>

#define LC_N     32
#define LC_BYTES 41
#define LC_K0    160
#define LC_MAX   511
#define LC_KMAX  254

void lc_encode(const float *x, uint8_t *out);   /* LC_N samples -> LC_BYTES */
void lc_decode(const uint8_t *in, float *y);    /* LC_BYTES -> LC_N samples */
#endif
