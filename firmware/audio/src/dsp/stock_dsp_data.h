#ifndef FB200_DSP_STOCK_DSP_DATA_H
#define FB200_DSP_STOCK_DSP_DATA_H
/* Stock FB200 amp/cab/tone tables (44.1 kHz designs). The data is vendor
 * material: firmware/tools/extract_stock_dsp.py generates it from the stock
 * .mr into build/stock_dsp_data.c, and the build defines FB200_STOCK_DSP=1
 * only when that file is linked. Without it the modules pass audio through. */

#define STOCK_AMP_MODELS     10
#define STOCK_AMP_WS         256
#define STOCK_AMP_SOS        10
#define STOCK_CABS           10
#define STOCK_CAB_TAPS       512
#define STOCK_TONE_STEPS     32
#define STOCK_TONE_MID_BANKS 5     /* 200, 400, 800, 1600, 3000 Hz */

/* SOS rows are CMSIS DF1 {b0, b1, b2, -a1, -a2}: y = b0x+b1x1+b2x2-a1y1-a2y2. */
typedef struct {
    float ws[STOCK_AMP_WS];            /* waveshaper, x = i/254 */
    float pre[STOCK_AMP_SOS][5];
    float post[STOCK_AMP_SOS][5];
    float pre_gain, out_gain, drive_scale, drive_scale2, level;
} stock_amp_model_t;

#ifdef FB200_STOCK_DSP
extern const stock_amp_model_t stock_amp_models[STOCK_AMP_MODELS];
/* 3x-rate anti-alias low-pass {b0, b1, b2, a1, a2} (ITCM literals 0x318c..):
 * exact bits matter, the amp's filter chains are ill-conditioned */
extern const float stock_amp_aa[5];
extern const float stock_cab_taps[STOCK_CABS][STOCK_CAB_TAPS];   /* natural order */
extern const float stock_cab_gain[STOCK_CABS];
/* CMSIS {b0, b1, b2, a1, a2} per knob step (idx = int(knob/100 * 31)) */
extern const float stock_tone_bass[STOCK_TONE_STEPS][5];
extern const float stock_tone_presence[STOCK_TONE_STEPS][5];
extern const float stock_tone_treble[STOCK_TONE_STEPS][5];
extern const float stock_tone_mid[STOCK_TONE_MID_BANKS][STOCK_TONE_STEPS][5];
#endif
#endif
