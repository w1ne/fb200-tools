#ifndef FB200_UI_H
#define FB200_UI_H
#include <stdbool.h>
#include <stdint.h>
#include "preset/preset.h"
/* The stock front-panel behaviour (docs/UI_AND_STORAGE.md §2): preset/stomp
 * modes, bank chords, knob pickup with LED feedback, long-A save. */
void ui_init(void);
void ui_task(uint32_t now_ms);
const preset_t *ui_edit_preset(void);     /* the live edit buffer */
unsigned ui_preset_index(void);
uint8_t ui_master(void);                  /* 0..100 */
uint32_t ui_revision(void);               /* bumps on every edit/preset change */
void ui_select(unsigned index);           /* console / remote */
int ui_save(void);                        /* write the edit buffer to its slot */
void ui_set_log(bool on);                 /* log footswitch and knob events */
void ui_flush_settings(void);             /* write unsaved settings now (power fail) */
#endif
