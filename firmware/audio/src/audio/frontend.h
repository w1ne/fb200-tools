#ifndef FB200_FRONTEND_H
#define FB200_FRONTEND_H
/* Board audio GPIOs, replayed from the stock firmware (main at ITCM 0x1774c):
 * at boot GPIO_B1_15 high, B1_10 low, B1_09 and B1_11 high; after the codec
 * and SAI are running B1_15 -> 0 and B1_10 -> 1 (power latch / mute pair,
 * docs/UI_AND_STORAGE.md). Also AD_B0_02 (GPIO1_IO2) high = knob-LED supply. */
void frontend_init(void);
void frontend_enable(void);
#endif
