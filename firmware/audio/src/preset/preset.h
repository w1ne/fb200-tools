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
/* Ours, in the settings sector after the stock block (ui/power.c): u16
 * marker "PW" 0x5750, idle standby minutes (0 = off), LED level %. */
#define POWER_SETTINGS_FLASH (SETTINGS_FLASH + 0x100u)
#define RHYTHM_FLASH 0x00081000u     /* on, ?, rhythm, level, bpm u16 */
#define RHYTHM_SIZE 6u

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
    /* Delay block (fn 0x85): stock fields 0x8c..0x94, our fields 0x96..0x9a
     * (see preset_delay_on) */
    P_DLY_EN = 0x8c, P_DLY_TYPE = 0x8e, P_DLY_MIX = 0x90, P_DLY_FB = 0x92, P_DLY_TIME = 0x94,
    P_DLY_MARK = 0x96, P_DLY_LOWCUT = 0x98, P_DLY_TONE = 0x9a,
    /* Our bass EQ (see preset_eq_on): marker, then EQ_REC bytes (dsp/eq.h) */
    P_EQ_MARK = 0xc4, P_EQ_DATA = 0xc6,
};

/* The stock format has a delay block (fn 0x85: en, type, mix, feedback,
 * time ms) that the stock DSP ignores, and every factory preset has it on
 * (mix 9, feedback 18, 490 ms). Playing it would change every stock preset.
 * So the delay plays only when our marker is also set: u16 0x4c44 ("DL") at
 * 0x96, the first unused word of the delay block. It is 0 in every factory
 * preset, and the stock audio path ignores it (tests/test_delay.py). Our
 * delay sets it; the app's 0x85 block (0x8c..0x95) leaves it alone, so after
 * that the app switches and edits the delay. docs/PARITY.md M4. */
#define DLY_MARK 0x4c44u

/* Our bass EQ (dsp/eq.h) lives after the module order (0xbc..0xc3), in the
 * unused tail 0xc4..0xff of the record: u16 marker 0x5145 ("EQ") at 0xc4,
 * then the settings at 0xc6..0xdd (eq_save/eq_load, EQ_REC = 24 bytes). The
 * tail is 0 in every factory preset, no app command (0x80..0x86, 0xA0) writes
 * it, and the stock DSP ignores it (tests/test_eq_preset.py). No marker (a
 * stock preset, erased flash, a whole-preset write from the app with 0 there)
 * = EQ off with the default settings. docs/PARITY.md M4. */
#define EQ_MARK 0x5145u

/* Global settings offsets. */
enum { S_PRESET = 0x16, S_BT = 0x17, S_MASTER = 0x18, S_IN_GAIN = 0x1a, S_STOMP = 0x1f,
       S_RHYTHM = 0x20, S_SLOT = 0x21, S_BANK = 0x22, S_TUNER_CAL = 0x2c, S_TUNER = 0x2d,
       S_TUNER_MUTE = 0x2e,
       S_LIGHT_COLOUR = 0x24, S_LIGHT_LEVEL = 0x28 };  /* + slot: light-ring colour 0..9, level 0..100 */

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

static inline bool preset_delay_on(const preset_t *p)
{
    return pget(p, P_DLY_MARK) == DLY_MARK && pget(p, P_DLY_EN) != 0;
}

/* the settings to load (NULL: no marker, EQ off + defaults) */
static inline const uint8_t *preset_eq(const preset_t *p)
{
    return pget(p, P_EQ_MARK) == EQ_MARK ? &p->b[P_EQ_DATA] : 0;
}
static inline bool preset_eq_on(const preset_t *p)
{
    return preset_eq(p) && p->b[P_EQ_DATA] != 0;
}

/* Checks of untrusted records (preset_check.c): an erased preset becomes
 * the stock blank "EMPTY" preset, out-of-range fields the stock's limits;
 * erased settings the stock defaults, an out-of-range field its default.
 * They return the number of fields fixed (0: the record was valid). */
extern const uint8_t settings_default[SETTINGS_SIZE];
bool preset_erased(const preset_t *p);
unsigned preset_sanitize(preset_t *p);
unsigned settings_sanitize(settings_t *s);

void preset_read(unsigned index, preset_t *out);         /* from flash */
int preset_write(unsigned index, const preset_t *p);      /* 0 on success */
void settings_read(settings_t *out);
int settings_write(const settings_t *s);
void rhythm_settings_read(uint8_t out[RHYTHM_SIZE]);
int rhythm_settings_write(const uint8_t in[RHYTHM_SIZE]);
#endif
