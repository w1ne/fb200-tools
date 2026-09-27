#ifndef FB200_PRESET_H
#define FB200_PRESET_H
/* Stock preset and settings formats, kept byte-compatible so existing presets
 * and the phone app keep working (docs/UI_AND_STORAGE.md §5). All knob-type
 * fields are u16 little endian in 0..100 knob units. */
#include <stdbool.h>
#include <stdint.h>

#define PRESET_COUNT 40            /* 10 banks x 4 slots */
#define PRESET_FLASH 0x00071000u   /* + index * 0x200 */
#define PRESET_STRIDE 0x200u
#define PRESET_SIZE 0x100u
#define SETTINGS_FLASH 0x00080000u
#define SETTINGS_SIZE 0x31u

/* Field offsets inside a preset record. */
enum {
    P_NAME = 0x00,                                   /* char[20] */
    P_COMP_EN = 0x14, P_COMP_TYPE = 0x16, P_COMP_ATTACK = 0x18, P_COMP_THRESH = 0x1a,
    P_COMP_RATIO = 0x1c, P_COMP_LEVEL = 0x1e,
    P_AMP_EN = 0x2c, P_AMP_MODEL = 0x2e, P_AMP_GAIN = 0x30, P_AMP_BASS = 0x32,
    P_AMP_MID = 0x34, P_AMP_MIDFREQ = 0x36, P_AMP_TREBLE = 0x38, P_AMP_VOLUME = 0x3a,
    P_CAB_EN = 0x44, P_CAB_TYPE = 0x46,
    P_GATE_EN = 0x5c, P_GATE_THRESH = 0x60,
    P_MOD_EN = 0x74, P_MOD_TYPE = 0x76, P_MOD_P1 = 0x78, P_MOD_P2 = 0x7a, P_MOD_P3 = 0x7c,
    P_MOD_P4 = 0x7e,
    P_REV_EN = 0xa4, P_REV_TYPE = 0xa6, P_REV_A8 = 0xa8, P_REV_LEVEL = 0xaa, P_REV_DECAY = 0xac,
    P_REV_AE = 0xae,
};

/* Global settings offsets. */
enum { S_PRESET = 0x16, S_BT = 0x17, S_MASTER = 0x18, S_STOMP = 0x1f, S_RHYTHM = 0x20,
       S_SLOT = 0x21, S_BANK = 0x22, S_TUNER = 0x2d };

typedef struct { uint8_t b[PRESET_SIZE]; } preset_t;
typedef struct { uint8_t b[SETTINGS_SIZE]; } settings_t;

static inline uint16_t pget(const preset_t *p, unsigned off)
{
    return (uint16_t)(p->b[off] | (p->b[off + 1] << 8));
}
static inline void pset(preset_t *p, unsigned off, uint16_t v)
{
    p->b[off] = (uint8_t)v;
    p->b[off + 1] = (uint8_t)(v >> 8);
}

void preset_read(unsigned index, preset_t *out);         /* from flash */
int preset_write(unsigned index, const preset_t *p);      /* 0 on success */
void settings_read(settings_t *out);
int settings_write(const settings_t *s);
#endif
