#ifndef FB200_LIGHTBAR_H
#define FB200_LIGHTBAR_H
#include <stdbool.h>
#include <stdint.h>
/* The footswitch light rings (40 RGB LEDs, 10 per footswitch dome) as the
 * stock drives them (LED task 0x67e0, docs/UI_AND_STORAGE.md §3):
 *   preset mode  only the loaded slot's ring, in its app colour and level
 *   live mode    a ring per switch, lit while its module is on
 *   tuner mode   all off
 *   rhythm mode  A, B lit while held; C flashes the tempo; D lit while playing
 *   save         1 s blink of the saved slot's ring */
typedef enum { LB_PRESET, LB_LIVE, LB_TUNER, LB_RHYTHM } lb_mode_t;

typedef struct {
    lb_mode_t mode;
    uint8_t slot;          /* loaded slot 0..3 = A..D (S+0x21) */
    uint8_t colour, level; /* the slot's colour index S+0x24+slot, level S+0x28+slot */
    uint8_t on;            /* bit per switch: live = module on, rhythm = switch held */
    bool playing;          /* rhythm: drums running */
    uint16_t bpm;          /* rhythm: tempo */
} lightbar_in_t;

/* Stock colour (0x17908): palette index 0..9 (> 9 -> 8), level 0..100
 * scales it to 30..100 %. Returns 0xRRGGBB. */
uint32_t lightbar_colour(unsigned index, unsigned level);
/* The four ring colours (A..D, 0xRRGGBB) for this state at now_ms. */
void lightbar_rings(uint32_t now_ms, const lightbar_in_t *in, uint32_t ring[4]);
void lightbar_save(uint32_t now_ms);                       /* start the save blink */
/* Build the frame and send it (rgb_show) only when it changed, at most every
 * 20 ms: the DMA frame must not disturb the audio. */
void lightbar_task(uint32_t now_ms, const lightbar_in_t *in);
#endif
