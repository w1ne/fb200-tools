/* Host harness for the app protocol (src/proto/proto.c) on top of the real
 * UI edit buffer (src/ui/ui.c) and an in-memory flash. Driven line by line
 * from stdin by tests/test_proto_host.py:
 *
 *   feed u|b <hex>        feed bytes from USB (u) or BLE (b); prints TX lines
 *   edit | settings       print the edit buffer / settings block
 *   flash <off> <len>     print flash bytes (hex offset)
 *   index                 print the current preset index
 *   notify <mask>         set the notification mask
 *   select <n>            front-panel preset change + proto_notify_preset()
 *
 * Every command's output ends with "." on its own line. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "proto/proto.h"
#include "preset/preset.h"
#include "ui/ui.h"
#include "audio/engine.h"
#include "ui/controls.h"
#include "ui/display.h"

#define FLASH_SIZE 0x100000u
static uint8_t flash[FLASH_SIZE];

/* ---- stubs for the UI's hardware dependencies ---- */
void log_printf(const char *fmt, ...) { (void)fmt; }
void display_text(const char *s) { (void)s; }
void knob_led(int led, bool on) { (void)led; (void)on; }
uint16_t knob_value(int k) { (void)k; return 0; }
bool knob_changed(int k) { (void)k; return false; }
fsw_event_t fsw_event(int *sw) { (void)sw; return FSW_NONE; }
/* engine: drum machine and tuner (ui.c's rhythm/tuner modes) */
static drums_t drums;
drums_t *engine_drums(void) { return &drums; }
void engine_set_tuner(bool on) { (void)on; }
bool engine_tuner_poll(tuner_result_t *out) { (void)out; return false; }
void drums_start(drums_t *d) { d->on = 1; }
void drums_stop(drums_t *d) { d->on = 0; }
void drums_set_rhythm(drums_t *d, unsigned r) { d->rhythm = (uint8_t)r; }
void drums_set_level(drums_t *d, unsigned l) { d->level = (uint8_t)l; }
void drums_set_tempo(drums_t *d, unsigned bpm) { d->bpm = (uint16_t)bpm; }
void drums_tap(drums_t *d, uint32_t now_ms) { (void)d; (void)now_ms; }

/* ---- preset layer over the fake flash ---- */
void preset_read(unsigned index, preset_t *out)
{
    memcpy(out, flash + PRESET_FLASH + (index % PRESET_COUNT) * PRESET_STRIDE, sizeof *out);
}
int preset_write(unsigned index, const preset_t *p)
{
    if (index >= PRESET_COUNT) return -1;
    memcpy(flash + PRESET_FLASH + index * PRESET_STRIDE, p, sizeof *p);
    return 0;
}
void settings_read(settings_t *out) { memcpy(out, flash + SETTINGS_FLASH, sizeof *out); }
int settings_write(const settings_t *s) { memcpy(flash + SETTINGS_FLASH, s, sizeof *s); return 0; }
void rhythm_settings_read(uint8_t out[RHYTHM_SIZE]) { memcpy(out, flash + RHYTHM_FLASH, RHYTHM_SIZE); }

/* ---- proto platform hooks ---- */
void proto_flash_read(uint32_t off, void *dst, uint32_t n) { memcpy(dst, flash + off, n); }
int proto_flash_write(uint32_t off, const void *src, uint32_t n)
{
    if (off < 0x71000u || off + n > 0xA1800u) { printf("BADWRITE %x %u\n", off, n); return -1; }
    memcpy(flash + off, src, n);
    return 0;
}
void proto_battery(uint8_t *percent, uint8_t *charging) { *percent = 75; *charging = 1; }
void proto_hook_bt_name(const uint8_t name[20])
{
    printf("HOOK bt_name ");
    for (int i = 0; i < 20; i++) printf("%02x", name[i]);
    printf("\n");
}
void proto_hook_bt_enable(bool on) { printf("HOOK bt_enable %d\n", on); }
void proto_hook_bootloader(void) { printf("HOOK bootloader\n"); }
int proto_hook_factory_reset(void) { printf("HOOK factory_reset\n"); return 0; }
void proto_hook_ir_changed(unsigned slot) { printf("HOOK ir %u\n", slot); }

static void tx(const char *t, const uint8_t *f, uint32_t n)
{
    printf("TX %s ", t);
    for (uint32_t i = 0; i < n; i++) printf("%02x", f[i]);
    printf("\n");
}
static void tx_usb(const uint8_t *f, uint32_t n) { tx("u", f, n); }
static void tx_ble(const uint8_t *f, uint32_t n) { tx("b", f, n); }

static void hexdump(const char *tag, const uint8_t *p, uint32_t n)
{
    printf("%s ", tag);
    for (uint32_t i = 0; i < n; i++) printf("%02x", p[i]);
    printf("\n");
}

static void seed(void)
{
    memset(flash, 0xFF, sizeof flash);
    for (unsigned i = 0; i < PRESET_COUNT; i++) {
        preset_t p;
        memset(&p, 0, sizeof p);
        snprintf((char *)p.b, 20, "Preset %02u", i);
        pset(&p, P_AMP_EN, 1);
        pset(&p, P_AMP_MODEL, (uint16_t)(1 + i % 10));
        pset(&p, P_AMP_GAIN, (uint16_t)(10 + i));
        pset(&p, P_CAB_EN, 1);
        pset(&p, P_CAB_TYPE, 2);
        pset(&p, P_MOD_TYPE, 3);
        pset(&p, P_REV_TYPE, 1);
        preset_write(i, &p);
    }
    static const uint8_t def[SETTINGS_SIZE] = {
        'B', '1', [0x17] = 1, [0x19] = 1, [0x1a] = 13, 13, 13, 13, 13,
        [0x28] = 100, 100, 100, 100, [0x2c] = 5, [0x2e] = 1,
    };
    memcpy(flash + SETTINGS_FLASH, def, sizeof def);
}

static uint32_t unhex(const char *s, uint8_t *out, uint32_t cap)
{
    uint32_t n = 0;
    while (s[0] && s[1] && n < cap) {
        unsigned v;
        if (sscanf(s, "%2x", &v) != 1) break;
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

int main(void)
{
    seed();
    ui_init();
    proto_init();
    proto_set_sender(PROTO_USB, tx_usb);
    proto_set_sender(PROTO_BLE, tx_ble);
    static char line[70000];
    static uint8_t buf[35000];
    while (fgets(line, sizeof line, stdin)) {
        char cmd[16] = {0}, a[16] = {0};
        line[strcspn(line, "\r\n")] = 0;
        if (sscanf(line, "%15s %15s", cmd, a) < 1) continue;
        if (!strcmp(cmd, "feed")) {
            const char *hex = strchr(line + 5, ' ');
            uint32_t n = hex ? unhex(hex + 1, buf, sizeof buf) : 0;
            proto_feed(a[0] == 'b' ? PROTO_BLE : PROTO_USB, buf, n);
        } else if (!strcmp(cmd, "edit")) {
            hexdump("EDIT", ui_edit_preset()->b, PRESET_SIZE);
        } else if (!strcmp(cmd, "settings")) {
            hexdump("SET", ui_settings()->b, SETTINGS_SIZE);
        } else if (!strcmp(cmd, "flash")) {
            unsigned off = 0, len = 0;
            sscanf(line, "%*s %x %u", &off, &len);
            hexdump("FLASH", flash + off, len);
        } else if (!strcmp(cmd, "index")) {
            printf("IDX %u\n", ui_preset_index());
        } else if (!strcmp(cmd, "notify")) {
            proto_set_notify_mask((uint8_t)atoi(a));
        } else if (!strcmp(cmd, "select")) {
            ui_select((unsigned)atoi(a));
            proto_notify_preset();
        } else if (!strcmp(cmd, "rev")) {
            printf("REV %u\n", ui_revision());
        }
        printf(".\n");
        fflush(stdout);
    }
    return 0;
}
