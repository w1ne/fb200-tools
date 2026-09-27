/* fb200-audio console. Commands (see `help`):
 *   help | stats | hb on|off | scan | dump [bus addr] | peek <addr> [len]
 *   poke <addr> <val> | crc <addr> <len> | fwinfo | fwtest | crumbs
 *   fwbegin|fwrec <len> <crc32> (USB self-update, see selfupdate.h)
 *   recovery | boot (two-stage boot, see recovery.h) | crash | hang (app only)
 *   reset | reboot
 * Addresses accept 0x.. hex or decimal. peek/poke are limited to RAM/flash
 * (peripheral access can stall a clock-gated block). */
#include <stdint.h>
#include <string.h>
#include "fsl_device_registers.h"
#include "tusb.h"
#include "cdc_log.h"
#include "console.h"
#include "fsl_iomuxc.h"
#include "audio/i2c_probe.h"
#include "audio/usb_audio.h"
#include "audio/codec.h"
#include "audio/sai.h"
#include "audio/engine.h"
#include "led.h"
#include "selfupdate.h"
#include "recovery.h"
#include "regs.h"
#ifndef FB200_RECOVERY
#include "ui/display.h"
#include "ui/controls.h"
#include "ui/power.h"
#include "ui/ui.h"
#include "ui/rgb.h"
#include "bt/bt.h"
#endif

extern int g_bss_writable;

static int streq(const char *a, const char *b);

static char line[96];
static size_t line_len;
static int heartbeat_on;

static uint32_t parse_num(const char *s, int *ok)
{
    uint32_t v = 0;
    *ok = 1;
    if (!s || !*s) { *ok = 0; return 0; }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        if (!*s) { *ok = 0; return 0; }
        for (; *s; s++) {
            char c = *s;
            uint32_t d = (c >= '0' && c <= '9') ? (uint32_t)(c - '0')
                       : (c >= 'a' && c <= 'f') ? (uint32_t)(c - 'a' + 10)
                       : (c >= 'A' && c <= 'F') ? (uint32_t)(c - 'A' + 10) : 99u;
            if (d > 15u) { *ok = 0; return 0; }
            v = (v << 4) | d;
        }
        return v;
    }
    for (; *s; s++) {
        if (*s < '0' || *s > '9') { *ok = 0; return 0; }
        v = v * 10u + (uint32_t)(*s - '0');
    }
    return v;
}

static int mem_ok(uint32_t addr)
{
    return (addr < 0x400000u) ||                       /* ITCM/DTCM */
           (addr >= 0x20000000u && addr < 0x20300000u) ||   /* DTCM/OCRAM */
           (addr >= 0x400F8000u && addr < 0x400F9000u) ||   /* SRC (always on) */
           (addr >= 0x401F4000u && addr < 0x401F4A00u) ||   /* OCOTP fuse shadows */
           (addr >= 0x401B8000u && addr < 0x401C8000u) ||   /* GPIO1-4 */
           (addr >= 0x60000000u && addr < 0x60800000u);     /* flash */
}

static void hexdump(uint32_t addr, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        if ((i & 15u) == 0) log_printf("%08x:", (unsigned)(addr + i));
        log_printf(" %02x", *(volatile uint8_t *)(addr + i));
        if ((i & 15u) == 15u) log_printf("\r\n");
    }
    if (len & 15u) log_printf("\r\n");
}

static void cmd_dumpmem(const char *a1, const char *a2)
{
    int ok1, ok2;
    uint32_t addr = parse_num(a1, &ok1);
    uint32_t len = parse_num(a2, &ok2);
    if (!ok1 || !ok2) { log_printf("usage: dumpmem <addr> <len>\r\n"); return; }
    if (len > 4096u) len = 4096u;
    if (!mem_ok(addr) || !mem_ok(addr + len)) { log_printf("address not allowed\r\n"); return; }
    hexdump(addr, len);
}

static void cmd_src(void)
{
    log_printf("SRC_SRSR=%08x SRC_SBMR1=%08x SRC_SBMR2=%08x\r\n",
               (unsigned)*(volatile uint32_t *)0x400F8008u,
               (unsigned)*(volatile uint32_t *)0x400F8004u,
               (unsigned)*(volatile uint32_t *)0x400F801Cu);
}

static void cmd_help(void)
{
    log_puts("commands:\r\n"
#ifndef FB200_RECOVERY
             "  audio : usb | sai | codec | creg <reg> [val] | gain [db] | mute [on|off]\r\n"
             "          testgen off|sine|white|impulse [freq] | tin <same> (into the chain, -20 dBFS)\r\n"
             "          meters on|off | x | cpu\r\n"
             "  led   : led on|off|scan | ledpin <gpio> <pin>\r\n"
             "  ui    : ui | uimon on|off | disp <text> | kled <0-15> on|off | power\r\n"
             "          preset [0-39] | save | rgb 0xRRGGBB [led] | rgb cfg 0xIIS0S1\r\n"
             "  bt    : bt | bt send <AT+...>\r\n"
             "  music : tuner on|off | drums [on|off|<1-40>|bpm <n>]\r\n"
             "  tests : crash | hang\r\n"
#endif
             "  debug : stats | src | hb on|off | clocks | crumbs | crashdump | crashclear\r\n"
             "          peek <addr> [len] | dumpmem <addr> <len> | poke <addr> <u8>\r\n"
             "          peek32 <addr> [n] | poke32 <addr> <u32> | crc <addr> <len>\r\n"
             "  i2c   : scan | dump [bus addr]\r\n"
             "  flash : fwinfo | fwtest | fwbegin|fwrec <len> <crc32>\r\n"
             "  boot  : recovery | boot | reset\r\n");
}

#ifndef FB200_RECOVERY

static void cmd_ui(void)
{
    log_printf("fsw A=%d B=%d C=%d D=%d (1 = down)\r\n", fsw_down(0), fsw_down(1),
               fsw_down(2), fsw_down(3));
    for (int k = 0; k < KNOB_COUNT; k++)
        log_printf("k%d=%u%s", k, (unsigned)knob_value(k), (k % 8 == 7) ? "\r\n" : " ");
}

#endif

static void cmd_peek32(const char *a1, const char *a2)
{
    int ok;
    const char *name;
    uint32_t addr = parse_num(a1, &ok);
    if (!ok) { log_printf("usage: peek32 <addr> [n]\r\n"); return; }
    uint32_t n = 1;
    if (a2) { n = parse_num(a2, &ok); if (!ok || n == 0u) { log_printf("bad n\r\n"); return; } }
    if (n > 64u) n = 64u;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t a = addr + 4u * i;
        if (!reg_access_ok(a, &name)) return;
        if ((a < 0x40000000u || (a >= 0x60000000u && a < 0xE0000000u)) && !mem_ok(a)) {
            log_printf("%08x: address not allowed\r\n", (unsigned)a);
            return;
        }
        if ((i & 3u) == 0u) log_printf("%s%08x:", i ? "\r\n" : "", (unsigned)a);
        log_printf(" %08x", (unsigned)*(volatile uint32_t *)a);
    }
    log_printf("  %s\r\n", name);
}

static void cmd_poke32(const char *a1, const char *a2)
{
    int ok1, ok2;
    const char *name;
    uint32_t addr = parse_num(a1, &ok1);
    uint32_t val = parse_num(a2, &ok2);
    if (!ok1 || !ok2) { log_printf("usage: poke32 <addr> <u32>\r\n"); return; }
    if (!reg_access_ok(addr, &name)) return;
    if (addr >= 0x60000000u && addr < 0xE0000000u) { log_printf("flash: use fwbegin\r\n"); return; }
    if (addr < 0x40000000u && !mem_ok(addr)) { log_printf("address not allowed\r\n"); return; }
    volatile uint32_t *r = (volatile uint32_t *)addr;
    uint32_t before = *r;
    *r = val;
    log_printf("%08x: %08x -> %08x (reads %08x)  %s\r\n", (unsigned)addr, (unsigned)before,
               (unsigned)val, (unsigned)*r, name);
}

static void cmd_crc(const char *a1, const char *a2)
{
    int ok1, ok2;
    uint32_t addr = parse_num(a1, &ok1);
    uint32_t len = parse_num(a2, &ok2);
    if (!ok1 || !ok2 || !mem_ok(addr) || !mem_ok(addr + len)) {
        log_printf("usage: crc <addr> <len>\r\n");
        return;
    }
    log_printf("crc %08x %u = %08x\r\n", (unsigned)addr, (unsigned)len,
               (unsigned)fw_crc32((const uint8_t *)addr, len));
}

static void cmd_fwbegin(int recovery, const char *a1, const char *a2)
{
    int ok1, ok2;
    uint32_t len = parse_num(a1, &ok1);
    uint32_t crc = parse_num(a2, &ok2);
    if (!ok1 || !ok2) { log_printf("usage: fwbegin <len> <crc32>\r\n"); return; }
    fw_begin(recovery, len, crc);
}

static void cmd_stats(void)
{
    log_printf("bss_writable=%d heartbeat=%d line_len=%u\r\n",
               g_bss_writable, heartbeat_on, (unsigned)line_len);
#ifndef FB200_RECOVERY
    engine_stats_t es;
    engine_get_stats(&es);
    log_printf("engine: gain=%d dB mute=%d drops=%lu inserts=%lu dma_errs=%lu skips=%lu resets=%lu\r\n",
               (int)engine_get_gain_db(), engine_get_mute() ? 1 : 0,
               (unsigned long)es.fifo_drops, (unsigned long)es.fifo_inserts,
               (unsigned long)es.dma_errors, (unsigned long)es.latency_skips,
               (unsigned long)es.dsp_resets);
    log_printf("meters: peak L=%d R=%d (x1000)\r\n",
               (int)(g_meter_peak[0] * 1000.0f),
               (int)(g_meter_peak[1] * 1000.0f));
#endif
}

#ifndef FB200_RECOVERY
static void cmd_usb(void)
{
    uint32_t pf, cf, ovf, unf;
    uint8_t spk_alt, mic_alt;
    usb_audio_stats(&pf, &cf, &ovf, &unf, &spk_alt, &mic_alt);
    log_printf("usb: spk_alt=%u mic_alt=%u play_fill=%lu cap_fill=%lu ovf=%lu unf=%lu\r\n",
               (unsigned)spk_alt, (unsigned)mic_alt, (unsigned long)pf,
               (unsigned long)cf, (unsigned long)ovf, (unsigned long)unf);
    uint8_t mute;
    int16_t vol;
    uint32_t stalls;
    usb_audio_host_controls(&mute, &vol, &stalls);
    log_printf("usb: host mute=%u volume=%d dB ctrl_stalls=%lu\r\n", (unsigned)mute,
               (int)(vol / 256), (unsigned long)stalls);
}
#endif

/* NAU88L21 register access: `creg <reg> [val]` (16-bit register, 16-bit data). */
#ifndef FB200_RECOVERY
static void cmd_creg(const char *a1, const char *a2)
{
    int ok;
    uint32_t reg = parse_num(a1, &ok);
    if (!ok || reg > 0xFF) {
        log_printf("usage: creg <reg> [val]\r\n");
        return;
    }
    if (a2) {
        uint32_t val = parse_num(a2, &ok);
        if (!ok || val > 0xFFFF) {
            log_printf("bad val\r\n");
            return;
        }
        log_printf("creg %02lx <- %04lx: %s\r\n", (unsigned long)reg,
                   (unsigned long)val,
                   codec_write((uint16_t)reg, (uint16_t)val) ? "ok" : "NAK");
    } else {
        uint16_t v = 0;
        if (codec_read((uint16_t)reg, &v)) {
            log_printf("creg %02lx = %04x\r\n", (unsigned long)reg, v);
        } else {
            log_printf("creg %02lx: ERR\r\n", (unsigned long)reg);
        }
    }
}
#endif

#ifndef FB200_RECOVERY
static void cmd_led(const char *a1, const char *a2)
{
    (void)a2;
    if (streq(a1, "on")) {
        led_set(true);
        log_printf("led on\r\n");
    } else if (streq(a1, "off")) {
        led_set(false);
        log_printf("led off\r\n");
    } else if (streq(a1, "scan")) {
        led_scan_start();
        log_printf("ledscan started (%d candidates)\r\n", led_candidate_count());
    } else {
        log_printf("led: %s | scan=%d\r\n", led_get() ? "on" : "off",
                   led_scan_active() ? 1 : 0);
    }
}
#endif

#ifndef FB200_RECOVERY
static void cmd_ledpin(const char *a1, const char *a2)
{
    int ok1, ok2;
    long g = (long)parse_num(a1, &ok1);
    long pin = (long)parse_num(a2, &ok2);
    if (!ok1 || !ok2 || g < 0 || g > 4 || pin < 0 || pin > 31) {
        log_printf("usage: ledpin <gpio 1..4> <pin 0..31> (0 0 = none)\r\n");
        return;
    }
    led_select((int)g, (int)pin);
    log_printf("led pin GPIO%ld_IO%ld selected\r\n", g, pin);
}
#endif

#ifndef FB200_RECOVERY
static void cmd_gain(const char *a1)
{
    if (a1) {
        int ok;
        long db = (long)parse_num(a1, &ok);
        if (!ok) { log_printf("usage: gain [db]\r\n"); return; }
        engine_set_gain_db((float)db);
    } else {
        float db = engine_get_gain_db();
        if (db > -3.0f) db = -6.0f;
        else if (db > -9.0f) db = -12.0f;
        else db = 0.0f;
        engine_set_gain_db(db);
    }
    log_printf("gain %d dB\r\n", (int)engine_get_gain_db());
}
#endif

#ifndef FB200_RECOVERY
static void cmd_testgen(const char *a1, const char *a2)
{
    if (!a1) { log_printf("usage: testgen off|sine|white|impulse [freq]\r\n"); return; }
    int mode = 0;
    if (streq(a1, "sine")) mode = 1;
    else if (streq(a1, "white")) mode = 2;
    else if (streq(a1, "impulse")) mode = 3;
    int ok = 0;
    float freq = a2 ? (float)parse_num(a2, &ok) : 1000.0f;
    if (!ok) freq = 1000.0f;
    engine_set_testgen(mode, 0.5f, freq);
    log_printf("testgen %s %d Hz\r\n", a1, (int)freq);
}
#endif

#ifndef FB200_RECOVERY
static void cmd_mute(const char *a1)
{
    bool on = !engine_get_mute();
    if (a1) {
        if (streq(a1, "on")) on = true;
        else if (streq(a1, "off")) on = false;
    }
    engine_set_mute(on);
    log_printf("mute %s\r\n", on ? "on" : "off");
}
#endif

#ifndef FB200_RECOVERY
static void cmd_meters(const char *a1)
{
    engine_set_meters(!a1 || streq(a1, "on"));
    log_printf("meters %s\r\n", (!a1 || streq(a1, "on")) ? "on" : "off");
}
#endif

#ifndef FB200_RECOVERY
static void cmd_x(void)
{
    engine_drop_tx(1500); /* ~2 s of TX blocks: underrun check */
    log_printf("x: TX refill paused for ~2 s\r\n");
}
#endif

#ifndef FB200_RECOVERY
static void cmd_sai(void)
{
    uint32_t rxf, txf, rxb, txb, over, under;
    sai_stats(&rxf, &txf, &rxb, &txb, &over, &under);
    log_printf("sai: rx_fill=%lu tx_fill=%lu rx_blocks=%lu tx_blocks=%lu ovf=%lu unf=%lu\r\n",
               (unsigned long)rxf, (unsigned long)txf, (unsigned long)rxb,
               (unsigned long)txb, (unsigned long)over, (unsigned long)under);
}
#endif

#ifndef FB200_RECOVERY
static void cmd_codec(void)
{
    uint16_t id = 0;
    if (codec_read(0x58, &id)) {
        log_printf("codec id reg58 = %04x\r\n", id);
    } else {
        log_printf("codec id reg58: ERR\r\n");
    }
    static const uint16_t regs[] = {0x00, 0x01, 0x03, 0x1C, 0x1D, 0x2B,
                                    0x2C, 0x31, 0x34, 0x35, 0x4B, 0x66,
                                    0x73, 0x76, 0x7F, 0x80};
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        uint16_t v = 0;
        if (codec_read(regs[i], &v)) {
            log_printf(" %02x=%04x", regs[i], v);
        } else {
            log_printf(" %02x=ERR", regs[i]);
        }
        if ((i & 3) == 3) {
            log_printf("\r\n");
        }
    }
    log_printf("\r\ncodec init: %s\r\n", codec_init() ? "ok" : "FAILED");
}
#endif

static void cmd_peek(const char *a1, const char *a2)
{
    int ok;
    uint32_t addr = parse_num(a1, &ok);
    if (!ok) { log_printf("usage: peek <addr> [len]\r\n"); return; }
    uint32_t len = 16;
    if (a2) { len = parse_num(a2, &ok); if (!ok) { log_printf("bad len\r\n"); return; } }
    if (len > 256) len = 256;
    if (!mem_ok(addr) || !mem_ok(addr + len)) { log_printf("address not allowed\r\n"); return; }
    hexdump(addr, len);
}

static void cmd_poke(const char *a1, const char *a2)
{
    int ok1, ok2;
    uint32_t addr = parse_num(a1, &ok1);
    uint32_t val = parse_num(a2, &ok2);
    if (!ok1 || !ok2) { log_printf("usage: poke <addr> <val>\r\n"); return; }
    if (!mem_ok(addr)) { log_printf("address not allowed\r\n"); return; }
    *(volatile uint8_t *)addr = (uint8_t)val;
    log_printf("ok %08x = %02x\r\n", (unsigned)addr, (unsigned)(val & 0xFFu));
}

static void cmd_dump(const char *a1, const char *a2)
{
    if (a1 && a2) {
        int ok1, ok2;
        uint32_t bus = parse_num(a1, &ok1);
        uint32_t addr = parse_num(a2, &ok2);
        if (!ok1 || !ok2) { log_printf("usage: dump [bus addr]\r\n"); return; }
        i2c_dump((uint8_t)bus, (uint8_t)addr);
    } else {
        i2c_dump_found();
    }
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int tokenize(char *s, char **argv, int max)
{
    int argc = 0;
    while (*s && argc < max) {
        while (*s == ' ') s++;
        if (!*s) break;
        argv[argc++] = s;
        while (*s && *s != ' ') s++;
        if (*s) *s++ = 0;
    }
    return argc;
}

static void dispatch(char *cmd)
{
    char *argv[3] = {0, 0, 0};
    int argc = tokenize(cmd, argv, 3);
    if (argc == 0) { cmd_help(); return; }

    if (streq(argv[0], "help")) cmd_help();
    else if (streq(argv[0], "stats")) cmd_stats();
#ifndef FB200_RECOVERY
    else if (streq(argv[0], "usb")) cmd_usb();
    else if (streq(argv[0], "codec")) cmd_codec();
    else if (streq(argv[0], "sai")) cmd_sai();
    else if (streq(argv[0], "gain") || streq(argv[0], "g")) cmd_gain(argv[1]);
    else if (streq(argv[0], "testgen") || streq(argv[0], "t")) {
        engine_testgen_input(false);
        cmd_testgen(argv[1], argv[2]);
    }
    else if (streq(argv[0], "tin")) {   /* test signal into the chain input, -20 dBFS */
        cmd_testgen(argv[1], argv[2]);
        int ok = 0;
        float f = argv[2] ? (float)parse_num(argv[2], &ok) : 1000.0f;
        int mode = streq(argv[1] ? argv[1] : "", "sine") ? 1 : streq(argv[1] ? argv[1] : "", "white") ? 2
                 : streq(argv[1] ? argv[1] : "", "impulse") ? 3 : 0;
        engine_set_testgen(mode, 0.1f, ok ? f : 1000.0f);
        engine_testgen_input(mode != 0);
    }
    else if (streq(argv[0], "cpu")) {
        uint32_t avg, max, budget;
        engine_cycles(&avg, &max, &budget);
        log_printf("cpu: engine block avg %lu max %lu cycles of %lu (%lu%% / %lu%%)\r\n",
                   (unsigned long)avg, (unsigned long)max, (unsigned long)budget,
                   (unsigned long)(100u * avg / budget), (unsigned long)(100u * max / budget));
    }
    else if (streq(argv[0], "mute") || streq(argv[0], "m")) cmd_mute(argv[1]);
    else if (streq(argv[0], "meters")) cmd_meters(argv[1]);
    else if (streq(argv[0], "x")) cmd_x();
    else if (streq(argv[0], "led")) cmd_led(argv[1], argv[2]);
    else if (streq(argv[0], "ledpin")) cmd_ledpin(argv[1], argv[2]);
    else if (streq(argv[0], "creg")) cmd_creg(argv[1], argv[2]);
#endif
    else if (streq(argv[0], "hb")) {
        heartbeat_on = (argc > 1 && streq(argv[1], "on"));
        log_printf("heartbeat %s\r\n", heartbeat_on ? "on" : "off");
    }
    else if (streq(argv[0], "scan")) i2c_scan_all();
    else if (streq(argv[0], "dump")) cmd_dump(argv[1], argv[2]);
    else if (streq(argv[0], "peek")) cmd_peek(argv[1], argv[2]);
    else if (streq(argv[0], "dumpmem")) cmd_dumpmem(argv[1], argv[2]);
    else if (streq(argv[0], "src")) cmd_src();
    else if (streq(argv[0], "poke")) cmd_poke(argv[1], argv[2]);
    else if (streq(argv[0], "crc")) cmd_crc(argv[1], argv[2]);
    else if (streq(argv[0], "fwinfo")) fw_info();
    else if (streq(argv[0], "fwtest")) fw_test();
    else if (streq(argv[0], "crumbs")) crumbs_print();
#ifndef FB200_RECOVERY
    else if (streq(argv[0], "ui")) cmd_ui();
    else if (streq(argv[0], "preset")) {
        int ok;
        uint32_t n = parse_num(argv[1], &ok);
        if (argc > 1 && ok && n < PRESET_COUNT) ui_select(n);
        const preset_t *p = ui_edit_preset();
        log_printf("preset %u: amp en=%u model=%u gain=%u  cab en=%u type=%u  mod en=%u type=%u  rev en=%u type=%u  master=%u\r\n",
                   ui_preset_index(), pget(p, P_AMP_EN), pget(p, P_AMP_MODEL), pget(p, P_AMP_GAIN),
                   pget(p, P_CAB_EN), pget(p, P_CAB_TYPE), pget(p, P_MOD_EN), pget(p, P_MOD_TYPE),
                   pget(p, P_REV_EN), pget(p, P_REV_TYPE), ui_master());
    }
    else if (streq(argv[0], "save")) ui_save();
    else if (streq(argv[0], "tuner")) {
        bool on = argc > 1 && streq(argv[1], "on");
        engine_set_tuner(on);
        tuner_result_t r;
        if (engine_tuner_poll(&r) || 1)
            log_printf("tuner %s: valid=%d silent=%d note=%d oct=%d cents=%d freq=%d.%02d Hz\r\n",
                       on ? "on" : "off", r.valid, r.silent, r.note, r.octave, (int)r.cents,
                       (int)r.freq, (int)((r.freq - (int)r.freq) * 100.0f));
    }
    else if (streq(argv[0], "drums")) {
        drums_t *d = engine_drums();
        int ok;
        if (argc > 1 && streq(argv[1], "on")) drums_start(d);
        else if (argc > 1 && streq(argv[1], "off")) drums_stop(d);
        else if (argc > 2 && streq(argv[1], "bpm")) drums_set_tempo(d, parse_num(argv[2], &ok));
        else if (argc > 1) { uint32_t r = parse_num(argv[1], &ok); if (ok && r >= 1 && r <= 40) drums_set_rhythm(d, r - 1u); }
        log_printf("drums %s rhythm %u bpm %u level %u samples %lu patterns %s\r\n", d->on ? "on" : "off",
                   (unsigned)d->rhythm + 1u, (unsigned)d->bpm, (unsigned)d->level,
                   (unsigned long)d->n_samples, d->data ? "yes" : "NO (build with STOCK_MR)");
    }
    else if (streq(argv[0], "bt")) {
        if (argc > 2 && streq(argv[1], "send")) log_printf("bt send %s\r\n", bt_at(argv[2]) == 0 ? "ok" : "busy");
        else bt_status();
    }
    else if (streq(argv[0], "rgb")) {   /* rgb <rrggbb> | rgb cfg <inv> <sym0> <sym1> */
        int ok, ok2, ok3;
        if (argc > 1 && streq(argv[1], "cfg")) {
            /* argv holds 3 tokens max: "rgb cfg <inv:sym0:sym1 as 0xIIS0S1>" */
            uint32_t v = parse_num(argv[2], &ok);
            if (ok) rgb_config((v >> 16) & 1u, (uint8_t)(v >> 8), (uint8_t)v);
            log_printf("rgb cfg %s\r\n", ok ? "ok" : "usage: rgb cfg 0x<inv><sym0><sym1>");
        } else {
            uint32_t c = parse_num(argv[1], &ok);
            uint32_t i = parse_num(argv[2], &ok2);
            (void)ok3;
            if (!ok) { log_printf("usage: rgb 0xRRGGBB [led]\r\n"); }
            else {
                if (argc > 2 && ok2) rgb_set((int)i, (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c);
                else rgb_fill((uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c);
                rgb_show();
                log_printf("ok\r\n");
            }
        }
    }
    else if (streq(argv[0], "power")) {
        const power_state_t *p = power_state();
        log_printf("power: battery=%u (level %u) supply=%u%s charging=%u\r\n",
                   (unsigned)p->battery_raw, (unsigned)p->level, (unsigned)p->supply_raw,
                   p->supply_low ? " LOW" : "", (unsigned)p->charging);
    }
    else if (streq(argv[0], "uimon")) {
        ui_set_log(argc > 1 && streq(argv[1], "on"));
        log_printf("ui monitor %s\r\n", (argc > 1 && streq(argv[1], "on")) ? "on" : "off");
    }
    else if (streq(argv[0], "disp")) { display_text(argc > 1 ? argv[1] : ""); log_printf("ok\r\n"); }
    else if (streq(argv[0], "kled")) {
        int ok;
        uint32_t n = parse_num(argv[1], &ok);
        if (ok && argc > 2) { knob_led((int)n, streq(argv[2], "on")); log_printf("ok\r\n"); }
        else log_printf("usage: kled <0-15> on|off\r\n");
    }
#endif
    else if (streq(argv[0], "crashdump")) crashdump_print();
    else if (streq(argv[0], "crashclear")) { crashdump_clear(); log_printf("cleared\r\n"); }
    else if (streq(argv[0], "clocks")) clocks_print();
    else if (streq(argv[0], "peek32")) cmd_peek32(argv[1], argv[2]);
    else if (streq(argv[0], "poke32")) cmd_poke32(argv[1], argv[2]);
    else if (streq(argv[0], "fwbegin")) cmd_fwbegin(0, argv[1], argv[2]);
    else if (streq(argv[0], "fwrec")) cmd_fwbegin(1, argv[1], argv[2]);
    else if (streq(argv[0], "recovery")) {
#ifdef FB200_RECOVERY
        log_printf("already in recovery (`boot` starts the app)\r\n");
#else
        log_printf("rebooting into recovery\r\n");
        recovery_request();
#endif
    }
#ifndef FB200_RECOVERY
    /* Recovery tests: a fault and a hang must both end in recovery. */
    else if (streq(argv[0], "crash")) { log_printf("crash: udf\r\n"); cdc_log_task(); __builtin_trap(); }
    else if (streq(argv[0], "hang")) {
        log_printf("hang: irqs off, no watchdog feed\r\n");
        cdc_log_task();
        __disable_irq();
        for (;;) {
        }
    }
#endif
#ifdef FB200_RECOVERY
    else if (streq(argv[0], "boot")) {
        const char *why;
        if (!slot_valid(&why)) log_printf("boot refused: %s\r\n", why);
        else {
            log_printf("starting the app\r\n");
            log_flush_ms(300);
            recovery_launch_app(1);
        }
    }
#endif
    else if (streq(argv[0], "reset") || streq(argv[0], "reboot")) {
        log_printf("rebooting\r\n");
        cdc_log_task();
        console_reboot();
    }
    else log_printf("unknown command (try help)\r\n");
}

void console_init(void) { line_len = 0; heartbeat_on = 0; }

void console_task(void)
{
    if (fw_active()) { fw_rx_task(); return; }
    while (tud_cdc_available()) {
        char c = (char)tud_cdc_read_char();
        if (c == '\r' || c == '\n') {
            cdc_log_write("\r\n", 2);
            line[line_len] = 0;
            dispatch(line);
            line_len = 0;
        } else if (c == 8 || c == 127) {
            if (line_len) { line_len--; cdc_log_write("\b \b", 3); }
        } else if (line_len < sizeof line - 1) {
            line[line_len++] = c;
            cdc_log_write(&c, 1);
        }
    }
}

int console_heartbeat_on(void) { return heartbeat_on; }
