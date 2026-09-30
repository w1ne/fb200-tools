/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

/* fb200-audio console. Commands (see `help`):
 *   help | stats | hb on|off | scan | dump [bus addr] | peek <addr> [len]
 *   poke <addr> <val> | crc <addr> <len> | fwinfo | fwtest | crumbs
 *   fwbegin|fwrec|fwstock <len> <crc32> (USB self-update, see selfupdate.h)
 *   jedec (flash chip) | irput|irls|irdel (long IR store, irstore/irstore.h)
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
#include "flash_rmw.h"
#include "recovery.h"
#include "regs.h"
#ifndef FB200_RECOVERY
#include "loopstore/lsio.h"
#include "ui/display.h"
#include "ui/controls.h"
#include "ui/power.h"
#include "ui/ui.h"
#include "ui/rgb.h"
#include "bt/bt.h"
#include "audio/bt_audio.h"
#include "dsp/stock_data.h"
#include "dsp/delay.h"
#include "dsp/eq.h"
#include "proto/proto.h"
#include "irstore/irstore.h"
#include "cpu_power.h"
#endif

#define STR_(x) #x
#define XSTR(x) STR_(x)   /* a macro's value as a string literal */

extern int g_bss_writable;

static int streq(const char *a, const char *b);
static void cmd_stack(void);

/* Raw memory access, overridable by the host fuzz harness
 * (tests/fuzz_host_test.c), which checks that every access the console
 * makes lies in memory that exists. */
#ifndef CONSOLE_RD8
#define CONSOLE_RD8(a)      (*(volatile uint8_t *)(uintptr_t)(a))
#define CONSOLE_RD32(a)     (*(volatile uint32_t *)(uintptr_t)(a))
#define CONSOLE_WR8(a, v)   (*(volatile uint8_t *)(uintptr_t)(a) = (uint8_t)(v))
#define CONSOLE_WR32(a, v)  (*(volatile uint32_t *)(uintptr_t)(a) = (uint32_t)(v))
#define CONSOLE_CRC(a, n)   crc32_ieee((const uint8_t *)(uintptr_t)(a), (n))
#endif

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

/* Memory that exists (linker.ld: FlexRAM as the vendor loader sets it up)
 * and the always-readable peripherals. A read of a hole (ITCM above 128 kB,
 * DTCM above 0x20058000, OCRAM above 32 kB) is a bus fault. */
static const struct { uint32_t base, end; uint8_t ram; } kMem[] = {
    {0x00000000u, 0x00020000u, 1},   /* ITCM */
    {0x20000000u, 0x20058000u, 1},   /* DTCM */
    {0x20200000u, 0x20208000u, 1},   /* OCRAM */
    {0x400F8000u, 0x400F9000u, 0},   /* SRC (always on) */
    {0x401F4000u, 0x401F4A00u, 0},   /* OCOTP fuse shadows */
    {0x401B8000u, 0x401C8000u, 0},   /* GPIO1-4 */
    {0x60000000u, 0x60800000u, 0},   /* flash (read only through AHB) */
};

/* [addr, addr + len) lies inside one region (len 0: addr inside one). With
 * ram: writable RAM only. */
static int mem_range(uint32_t addr, uint32_t len, int ram)
{
    for (unsigned i = 0; i < sizeof kMem / sizeof kMem[0]; i++) {
        if (ram && !kMem[i].ram) continue;
        if (addr >= kMem[i].base && addr < kMem[i].end && len <= kMem[i].end - addr) return 1;
    }
    return 0;
}


static void hexdump(uint32_t addr, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        if ((i & 15u) == 0) log_printf("%08x:", (unsigned)(addr + i));
        log_printf(" %02x", CONSOLE_RD8(addr + i));
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
    if (!mem_range(addr, len, 0)) { log_printf("address not allowed\r\n"); return; }
    hexdump(addr, len);
}

static void cmd_src(void)
{
    log_printf("SRC_SRSR=%08x SRC_SBMR1=%08x SRC_SBMR2=%08x\r\n",
               (unsigned)CONSOLE_RD32(0x400F8008u),
               (unsigned)CONSOLE_RD32(0x400F8004u),
               (unsigned)CONSOLE_RD32(0x400F801Cu));
}

static void cmd_help(void)
{
    log_puts("commands:\r\n"
#ifndef FB200_RECOVERY
             "  audio : usb [out|in|mix] | sai | codec | creg <reg> [val] | gain [db] | mute [on|off]\r\n"
             "          testgen off|sine|white|impulse [freq] | tin <same> (into the chain, -20 dBFS)\r\n"
             "          usb in = reamping: host playback into the chain (mix: + instrument, out: default)\r\n"
             "          meters on|off | x | cpu | prof | cab long <0-" XSTR(ENGINE_IR_TAPS) "> | dither [on|off]\r\n"
             "          cab [<1-83>] (on, type: 1-10 stock, 11-19 user IR, 20-83 long IR)\r\n"
             "  led   : led on|off|scan | ledpin <gpio> <pin>\r\n"
             "  ui    : ui | uimon on|off | disp <text> | kled <0-15> on|off\r\n"
             "  power : power [sleep on|off | clock 600|528|396 | led 100|66|33 | idle <min> | log <s>]\r\n"
             "          preset [0-39] | save | factory [yes] | rgb 0xRRGGBB [led] | rgb cfg 0xIIS0S1\r\n"
             "  bt    : bt | bt send <AT+...> | btaudio\r\n"
             "  music : stock | tuner on|off | drums [on|off|<1-40>|bpm <n>|level <0-100>]\r\n"
             "  loop  : loop [rec|play|dub|stop|undo|clear|tap] | loop save|load <1-2>\r\n"
             "          loop level <0-100> | loop stats\r\n"
             "  delay : delay [on|off] [time " XSTR(DELAY_MS_MIN) "-" XSTR(DELAY_MS_MAX) " ms] [fb 0-100] [mix 0-100] [lowcut 0-100] [tone 0-100]\r\n"
             "  eq    : eq [on|off] | eq hpf <20-200 Hz|0> | eq lpf <2000-20000 Hz|0>\r\n"
             "          eq <band 1-5> <30-10000 Hz> <gain -15..15 dB> [q 0.3-4]\r\n"
             "  tests : crash | hang\r\n"
#endif
             "  debug : stats | src | hb on|off | clocks | crumbs | crashdump | crashclear | stack\r\n"
             "          peek <addr> [len] | dumpmem <addr> <len> | poke <addr> <u8>\r\n"
             "          peek32 <addr> [n] | poke32 <addr> <u32> | crc <addr> <len>\r\n"
             "  i2c   : scan | dump [bus addr]\r\n"
             "  flash : fwinfo | fwtest | jedec | fwbegin|fwrec|fwstock <len> <crc32>\r\n"
#ifndef FB200_RECOVERY
             "  irs   : irls | irdel <20-83> | irput <20-83> <taps 1-4096> <crc32> <name> [rate]\r\n"
             "          (long IRs, cab types 20-83; irput then takes taps x 4 raw float32 bytes)\r\n"
#endif
             "  boot  : recovery | boot | reset\r\n");
}

#ifndef FB200_RECOVERY

/* delay [on|off] [time ms] [fb] [mix] [lowcut] [tone]: edits the delay block
 * of the edit buffer (save with `save` or a held footswitch). A preset
 * without our marker (every stock preset) does not play its stock delay
 * fields; the first edit writes the marker and our defaults
 * (src/preset/preset.h preset_delay_on). */
static void cmd_delay(int argc, char **argv)
{
    const preset_t *e = ui_edit_preset();
    bool marked = pget(e, P_DLY_MARK) == DLY_MARK;
    unsigned en = marked && pget(e, P_DLY_EN) != 0;
    unsigned v[5] = {DELAY_DEF_MS, DELAY_DEF_FB, DELAY_DEF_MIX, DELAY_DEF_LOWCUT, DELAY_DEF_TONE};
    if (marked) {
        v[0] = pget(e, P_DLY_TIME); v[1] = pget(e, P_DLY_FB); v[2] = pget(e, P_DLY_MIX);
        v[3] = pget(e, P_DLY_LOWCUT); v[4] = pget(e, P_DLY_TONE);
        /* a stored time over DELAY_MS_MAX (RAM, dsp/delay.h) plays clamped:
         * show that; the next edit stores it */
        if (v[0] > DELAY_MS_MAX) v[0] = DELAY_MS_MAX;
        if (v[0] < DELAY_MS_MIN) v[0] = DELAY_MS_MIN;
    }
    int i = 1, change = 0;
    if (i < argc && (streq(argv[i], "on") || streq(argv[i], "off"))) {
        en = streq(argv[i], "on");
        change = en || marked;          /* off on a stock preset: nothing to do */
        i++;
    }
    for (unsigned k = 0; i < argc && k < 5; i++, k++) {
        int ok;
        uint32_t n = parse_num(argv[i], &ok);
        if (!ok) {
            log_printf("usage: delay [on|off] [time " XSTR(DELAY_MS_MIN) "-" XSTR(DELAY_MS_MAX) " ms] [fb 0-100] [mix 0-100] "
                       "[lowcut 0-100] [tone 0-100]\r\n");
            return;
        }
        v[k] = k == 0 ? (n < DELAY_MS_MIN ? DELAY_MS_MIN : n > DELAY_MS_MAX ? DELAY_MS_MAX : n)
                      : (n > 100u ? 100u : n);
        change = 1;
    }
    if (change) {
        uint16_t w[8] = {(uint16_t)en, pget(e, P_DLY_TYPE), (uint16_t)v[2], (uint16_t)v[1],
                         (uint16_t)v[0], DLY_MARK, (uint16_t)v[3], (uint16_t)v[4]};
        uint8_t b[16];
        for (unsigned k = 0; k < 8; k++) { b[2 * k] = (uint8_t)w[k]; b[2 * k + 1] = (uint8_t)(w[k] >> 8); }
        ui_edit_write(P_DLY_EN, b, sizeof b);
        marked = true;
    }
    float hz = delay_lowcut_hz(v[3]);
    log_printf("delay %s%s: time %u ms fb %u mix %u lowcut %u (%d Hz) tone %u%s\r\n",
               en ? "on" : "off", marked ? "" : " (stock preset, never plays)",
               v[0], v[1], v[2], v[3], (int)hz, v[4], v[4] >= 100u ? " (off)" : "");
}

/* loop [rec|play|dub|stop|undo|clear|tap] | loop save|load <1-2> | loop
 * level <0-100> | loop stats: the looper (dsp/looper.h, the loop in the
 * flash: loopstore/loopstore.h), then its state. prep_ms: erased flash
 * ready to record into. save/load: two loops kept across power-off.
 * `loop stats`: the flash side's counters. */
static void cmd_loop_stats(void)
{
    engine_loop_stats_t st;
    engine_loop_stats(&st);
    uint8_t id[3] = {0, 0, 0};
    (void)flash_read_id(id);
    const ls_info_t *li = &st.store;
    log_printf("loop flash: jedec %02x %02x %02x area 0x%06x..0x%06x chunks=%u used=%u erased=%u "
               "garbage=%u suspend=%d\r\n", id[0], id[1], id[2], (unsigned)li->base,
               (unsigned)li->end, (unsigned)li->chunks, (unsigned)li->n_used,
               (unsigned)li->n_erased, (unsigned)li->n_garbage, flash_suspend_ok());
    log_printf("loop io: pages=%lu prog_slow=%lu prog_max_ms=%lu verify_errs=%lu prog_errs=%lu "
               "erases=%lu erase_ms=%lu erase_max_ms=%lu erase_errs=%lu slices=%lu suspends=%lu "
               "reads=%lu\r\n", (unsigned long)g_lsio.pages, (unsigned long)g_lsio.prog_slow,
               (unsigned long)g_lsio.prog_max_ms, (unsigned long)g_lsio.verify_errs,
               (unsigned long)g_lsio.prog_errs, (unsigned long)g_lsio.erases,
               (unsigned long)g_lsio.erase_ms, (unsigned long)g_lsio.erase_max_ms,
               (unsigned long)g_lsio.erase_errs, (unsigned long)g_lsio.slices,
               (unsigned long)g_lsio.suspends, (unsigned long)g_lsio.reads);
    log_printf("loop streams: underruns=%lu overruns=%lu drops=%lu cut=%lu\r\n",
               (unsigned long)st.io->rd_under, (unsigned long)st.io->wr_over,
               (unsigned long)li->drops, (unsigned long)st.cut);
}

static void cmd_loop(int argc, char **argv)
{
    static const char *const kAct[] = {"tap", "rec", "play", "dub", "stop", "undo", "clear"};
    static const char *const kState[] = {"off", "empty", "rec", "play", "dub", "stop"};
    static const char *const kUndo[] = {"none", "undo", "redo"};
    static const char *kUse =
        "usage: loop [rec|play|dub|stop|undo|clear|tap] | loop save|load <1-2> | "
        "loop level <0-100> | loop stats\r\n";
    int r = 0;
    unsigned slot = 0;
    int stored = 0;                 /* 1 save, 2 load */
    if (argc > 1) {
        int a = -1;
        for (int i = 0; i < 7; i++) if (streq(argv[1], kAct[i])) a = i;
        if (a >= 0) {
            r = engine_loop(a);
        } else if ((streq(argv[1], "save") || streq(argv[1], "load")) && argc > 2) {
            int ok;
            uint32_t n = parse_num(argv[2], &ok);
            if (!ok || n < 1u || n > 2u) {
                log_printf("usage: loop save <1-2> | loop load <1-2>\r\n");
                return;
            }
            slot = (unsigned)n;
            stored = streq(argv[1], "save") ? 1 : 2;
            r = stored == 1 ? engine_loop_save(n - 1u) : engine_loop_load(n - 1u);
        } else if (streq(argv[1], "level") && argc > 2) {
            int ok;
            uint32_t n = parse_num(argv[2], &ok);
            if (!ok || n > 100u) {
                log_printf("usage: loop level <0-100>\r\n");
                return;
            }
            engine_loop_level(n);
        } else if (streq(argv[1], "stats")) {
            cmd_loop_stats();
            return;
        } else {
            log_printf("%s", kUse);
            return;
        }
    }
    looper_info_t in;
    engine_loop_info(&in);
    if (r != 0) {
        log_printf("loop %s: %s\r\n", argv[1],
                   r == -2 ? "not available (no flash area for loops)"
                   : r == -3 ? "not allowed now: preparing the flash, try again in a moment"
                   : r == -5 ? "bad flash write"
                   : "not allowed now");
    } else if (stored) {
        log_printf("loop %s %u: ok\r\n", stored == 1 ? "save" : "load", slot);
    }
    log_printf("loop %s: len_ms=%u pos_ms=%u max_ms=%u undo_max_ms=%u undo=%s level=%u "
               "prep_ms=%u flash=%d\r\n", kState[in.state <= LOOPER_STOP ? in.state : 0],
               in.len_ms, in.pos_ms, in.max_ms, in.undo_max_ms, kUndo[in.undo], in.level,
               in.prep_ms, in.flash);
}

/* Signed decimal: "-4.5", "3", "0.71". */
static float parse_dec(const char *s, int *ok)
{
    float v = 0.0f, scale = 1.0f, sign = 1.0f;
    int digits = 0, point = 0;
    *ok = 0;
    if (!s) return 0.0f;
    if (*s == '-' || *s == '+') { if (*s == '-') sign = -1.0f; s++; }
    for (; *s; s++) {
        if (*s == '.' && !point) { point = 1; continue; }
        if (*s < '0' || *s > '9') return 0.0f;
        if (point) { scale *= 0.1f; v += (float)(*s - '0') * scale; }
        else v = v * 10.0f + (float)(*s - '0');
        digits++;
    }
    *ok = digits > 0;
    return sign * v;
}

/* x with 1 or 2 decimals and the sign: "-4.5", "+3.0", "0.71" */
static void print_dec(const char *pre, float x, int decimals, int plus)
{
    int m = decimals == 2 ? 100 : 10;
    int t = (int)(x * (float)m + (x < 0.0f ? -0.5f : 0.5f));
    int a = t < 0 ? -t : t;
    log_printf(decimals == 2 ? "%s%s%d.%02d" : "%s%s%d.%d", pre, t < 0 ? "-" : plus ? "+" : "",
               a / m, a % m);
}

/* eq [on|off] | eq hpf <hz> | eq lpf <hz> | eq <band> <hz> <gain dB> [q]: our
 * EQ after the cab (dsp/eq.h). A change goes into the edit buffer with our
 * marker (preset.h P_EQ_DATA; `save` stores it), on the preset's grid. */
static void cmd_eq(int argc, char **argv)
{
    eq_t *e = engine_eq();
    int ok = 1, ok2 = 1, ok3 = 1, ok4 = 1;
    if (argc > 1 && (streq(argv[1], "on") || streq(argv[1], "off"))) {
        eq_set_on(e, streq(argv[1], "on"));
    } else if (argc > 2 && (streq(argv[1], "hpf") || streq(argv[1], "lpf"))) {
        float hz = streq(argv[2], "off") ? 0.0f : parse_dec(argv[2], &ok);
        if (ok && streq(argv[1], "hpf")) eq_set_hpf(e, hz);
        else if (ok) eq_set_lpf(e, hz);
    } else if (argc > 3) {
        uint32_t b = parse_num(argv[1], &ok);
        float hz = parse_dec(argv[2], &ok2), g = parse_dec(argv[3], &ok3), q = EQ_Q_DEF;
        if (argc > 4) q = parse_dec(argv[4], &ok4);
        ok = ok && ok2 && ok3 && ok4 && b >= 1u && b <= EQ_BANDS;
        if (ok) eq_set_band(e, b - 1u, hz, g, q);
    } else if (argc > 1) {
        ok = 0;
    }
    if (!ok) {
        log_printf("usage: eq [on|off] | eq hpf <20-200 Hz|0> | eq lpf <2000-20000 Hz|0> | "
                   "eq <band 1-5> <30-10000 Hz> <gain -15..15 dB> [q 0.3-4]\r\n");
        return;
    }
    if (argc > 1) {
        uint8_t r[2 + EQ_REC] = {(uint8_t)EQ_MARK, (uint8_t)(EQ_MARK >> 8)};
        eq_save(e, r + 2);
        eq_load(e, r + 2);              /* show what the preset keeps */
        ui_edit_write(P_EQ_MARK, r, sizeof r);
    }
    log_printf("eq %s%s: hpf ", e->on ? "on" : "off", e->on && e->bypass ? " (flat)" : "");
    if (e->hpf > 0.0f) log_printf("%d Hz", (int)(e->hpf + 0.5f)); else log_printf("off");
    log_printf(" lpf ");
    if (e->lpf > 0.0f) log_printf("%d Hz\r\n", (int)(e->lpf + 0.5f)); else log_printf("off\r\n");
    for (unsigned b = 0; b < EQ_BANDS; b++) {
        log_printf("  %u: %d Hz", b + 1u, (int)(e->f[b] + 0.5f));
        print_dec(" ", e->g[b], 2, 1);
        print_dec(" dB q ", e->q[b], 2, 0);
        log_printf("\r\n");
    }
}

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
        if ((a < 0x40000000u || (a >= 0x60000000u && a < 0xE0000000u)) && !mem_range(a, 4, 0)) {
            log_printf("%08x: address not allowed\r\n", (unsigned)a);
            return;
        }
        if ((i & 3u) == 0u) log_printf("%s%08x:", i ? "\r\n" : "", (unsigned)a);
        log_printf(" %08x", (unsigned)CONSOLE_RD32(a));
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
    if (addr < 0x40000000u && !mem_range(addr, 4, 1)) { log_printf("address not allowed\r\n"); return; }
    uint32_t before = CONSOLE_RD32(addr);
    CONSOLE_WR32(addr, val);
    log_printf("%08x: %08x -> %08x (reads %08x)  %s\r\n", (unsigned)addr, (unsigned)before,
               (unsigned)val, (unsigned)CONSOLE_RD32(addr), name);
}

static void cmd_crc(const char *a1, const char *a2)
{
    int ok1, ok2;
    uint32_t addr = parse_num(a1, &ok1);
    uint32_t len = parse_num(a2, &ok2);
    if (!ok1 || !ok2) { log_printf("usage: crc <addr> <len>\r\n"); return; }
    if (!mem_range(addr, len, 0)) { log_printf("address not allowed\r\n"); return; }
    log_printf("crc %08x %u = %08x\r\n", (unsigned)addr, (unsigned)len, (unsigned)CONSOLE_CRC(addr, len));
}

static void cmd_fwbegin(fw_target_t target, const char *a1, const char *a2)
{
    int ok1, ok2;
    uint32_t len = parse_num(a1, &ok1);
    uint32_t crc = parse_num(a2, &ok2);
    if (!ok1 || !ok2) { log_printf("usage: fwbegin|fwrec|fwstock <len> <crc32>\r\n"); return; }
    fw_begin(target, len, crc);
}

/* `jedec`: the flash chip's JEDEC ID, its size, the FlexSPI window and the
 * long IR store's state (docs/HARDWARE.md). */
static void cmd_jedec(void)
{
    static const struct { uint8_t id; const char *name; } vendors[] = {
        {0xEF, "Winbond"}, {0xC8, "GigaDevice"}, {0xC2, "Macronix"}, {0x9D, "ISSI"},
        {0x20, "Micron/XMC"}, {0x68, "Boya"}, {0x0B, "XTX"}, {0x85, "Puya"}, {0x1F, "Adesto"},
        {0x5E, "Zbit"}, {0xA1, "Fudan"}, {0xBA, "Zetta"},
    };
    uint8_t id[3];
    if (!flash_read_id(id)) { log_printf("jedec: read FAILED\r\n"); return; }
    const char *vendor = "unknown";
    for (unsigned i = 0; i < sizeof vendors / sizeof vendors[0]; i++)
        if (vendors[i].id == id[0]) vendor = vendors[i].name;
    uint32_t chip = flash_chip_size(id), win = flash_window(), cap = flash_capacity();
    log_printf("jedec %02x %02x %02x: manufacturer %s, type %02x, capacity %02x = %lu kB\r\n",
               id[0], id[1], id[2], vendor, id[1], id[2], (unsigned long)(chip / 1024u));
    log_printf("flexspi A1 window %lu kB; usable %lu kB\r\n", (unsigned long)(win / 1024u),
               (unsigned long)(cap / 1024u));
#ifndef FB200_RECOVERY
    log_printf("ir store F:0x%06lx..0x%06lx: %s\r\n", (unsigned long)IRSTORE_BASE,
               (unsigned long)IRSTORE_END, cap >= IRSTORE_END ? "fits" : "does NOT fit (off)");
#endif
}

#ifndef FB200_RECOVERY
/* Long IR store (irstore/irstore.h): irput / irls / irdel. */
static void cmd_irput(int argc, char **argv)
{
    int ok1, ok2, ok3, ok4 = 1;
    uint32_t slot = parse_num(argv[1], &ok1), taps = parse_num(argv[2], &ok2);
    uint32_t crc = parse_num(argv[3], &ok3), rate = 44100u;
    if (argc > 5) rate = parse_num(argv[5], &ok4);
    if (argc < 5 || !ok1 || !ok2 || !ok3 || !ok4) {
        log_printf("ir: usage: irput <slot %u-%u> <taps 1-%u> <crc32> <name> [rate]\r\n",
                   IRSTORE_FIRST, IRSTORE_LAST, IRSTORE_TAPS);
        return;
    }
    (void)irstore_put_begin(slot, taps, crc, argv[4], rate);
}

static void cmd_irdel(const char *a1)
{
    int ok;
    uint32_t t = parse_num(a1, &ok);
    if (!ok || irstore_slot_of(t) < 0) {
        log_printf("ir: usage: irdel <slot %u-%u>\r\n", IRSTORE_FIRST, IRSTORE_LAST);
        return;
    }
    irstore_print_delete(t);
}
#endif

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
static const char *const usb_routes[] = {"out", "in", "mix"};

/* `usb [out|in|mix]`: host playback to the DAC (default), into the chain
 * input instead of the instrument (reamping), or summed with it. */
static void cmd_usb(const char *route)
{
    if (route) {
        int r = -1;
        for (int i = 0; i < 3; i++) if (streq(route, usb_routes[i])) r = i;
        if (r < 0) { log_printf("usage: usb [out|in|mix]\r\n"); return; }
        engine_set_usb_route(r);
        log_printf("usb route %s\r\n", usb_routes[r]);
    }
    uint32_t pf, cf, ovf, unf;
    uint8_t spk_alt, mic_alt;
    usb_audio_stats(&pf, &cf, &ovf, &unf, &spk_alt, &mic_alt);
    log_printf("usb: route=%s spk_alt=%u mic_alt=%u play_fill=%lu cap_fill=%lu ovf=%lu unf=%lu\r\n",
               usb_routes[engine_get_usb_route()], (unsigned)spk_alt, (unsigned)mic_alt, (unsigned long)pf,
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
#define GAIN_DB_MAX 24   /* more is a number typo, not a level (inf gain poisons the DSP) */
static void cmd_gain(const char *a1)
{
    if (a1) {
        int ok;
        uint32_t db = parse_num(a1, &ok);
        if (!ok || db > GAIN_DB_MAX) { log_printf("usage: gain [0-" XSTR(GAIN_DB_MAX) " dB]\r\n"); return; }
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
    uint32_t hz = a2 ? parse_num(a2, &ok) : 1000u;
    if (!ok || hz > 24000u) hz = 1000u;   /* up to Nyquist at 48 kHz */
    float freq = (float)hz;
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
    if (!mem_range(addr, len, 0)) { log_printf("address not allowed\r\n"); return; }
    hexdump(addr, len);
}

static void cmd_poke(const char *a1, const char *a2)
{
    int ok1, ok2;
    uint32_t addr = parse_num(a1, &ok1);
    uint32_t val = parse_num(a2, &ok2);
    if (!ok1 || !ok2) { log_printf("usage: poke <addr> <val>\r\n"); return; }
    /* RAM only: a byte write to a peripheral or to the flash window faults */
    if (!mem_range(addr, 1, 1)) { log_printf("address not allowed\r\n"); return; }
    CONSOLE_WR8(addr, val);
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
    if (!a || !b) return 0;   /* a missing argument (argv[i] is NULL past argc) */
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
    char *argv[7] = {0, 0, 0, 0, 0, 0, 0};
    int argc = tokenize(cmd, argv, 7);
    if (argc == 0) { cmd_help(); return; }

    if (streq(argv[0], "help")) cmd_help();
    else if (streq(argv[0], "stats")) cmd_stats();
#ifndef FB200_RECOVERY
    else if (streq(argv[0], "usb")) cmd_usb(argv[1]);
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
        uint32_t hz = argv[2] ? parse_num(argv[2], &ok) : 1000u;
        float f = (ok && hz <= 24000u) ? (float)hz : 1000.0f;
        int mode = streq(argv[1] ? argv[1] : "", "sine") ? 1 : streq(argv[1] ? argv[1] : "", "white") ? 2
                 : streq(argv[1] ? argv[1] : "", "impulse") ? 3 : 0;
        engine_set_testgen(mode, 0.1f, f);
        engine_testgen_input(mode != 0);
    }
    else if (streq(argv[0], "cpu")) {
        uint32_t avg, max, budget;
        engine_cycles(&avg, &max, &budget);
        log_printf("cpu: engine block avg %lu max %lu cycles of %lu (%lu%% / %lu%%)\r\n",
                   (unsigned long)avg, (unsigned long)max, (unsigned long)budget,
                   (unsigned long)(100u * avg / budget), (unsigned long)(100u * max / budget));
        uint32_t busy, wakes;
        cpu_busy(&busy, &wakes);   /* since the last `cpu` */
        log_printf("cpu: loop busy %lu.%lu%% (sleep %s, %lu wakes/s, core %u MHz)\r\n",
                   (unsigned long)(busy / 10u), (unsigned long)(busy % 10u),
                   cpu_sleep_enabled() ? "on" : "off", (unsigned long)wakes, cpu_clock_mhz());
    }
    else if (streq(argv[0], "mute") || streq(argv[0], "m")) cmd_mute(argv[1]);
    else if (streq(argv[0], "dither")) {   /* TPDF dither on the 16-bit DAC/USB output (dsp/outq.h) */
        if (argv[1]) engine_set_dither(streq(argv[1], "on"));
        log_printf("dither %s\r\n", engine_get_dither() ? "on" : "off");
    }
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
    else if (streq(argv[0], "jedec")) cmd_jedec();
    else if (streq(argv[0], "crumbs")) crumbs_print();
    else if (streq(argv[0], "stack")) cmd_stack();
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
    else if (streq(argv[0], "delay")) cmd_delay(argc, argv);
    else if (streq(argv[0], "eq")) cmd_eq(argc, argv);
    else if (streq(argv[0], "factory")) {
        if (argc > 1 && streq(argv[1], "yes"))
            log_printf("factory reset: %s\r\n", proto_factory_reset() == 0 ? "ok" : "FAILED");
        else
            log_printf("factory reset: all 40 presets, the global and drum settings and the IR\r\n"
                       "list go back to the stock defaults (IR data stays). Type: factory yes\r\n");
    }
    else if (streq(argv[0], "tuner")) {
        bool on = argc > 1 && streq(argv[1], "on");
        engine_set_tuner(on);
        tuner_result_t r = {0};
        (void)engine_tuner_poll(&r);   /* no new result: zeros */
        log_printf("tuner %s: valid=%d silent=%d note=%d oct=%d cents=%d freq=%d.%02d Hz\r\n",
                   on ? "on" : "off", r.valid, r.silent, r.note, r.octave, (int)r.cents,
                   (int)r.freq, (int)((r.freq - (int)r.freq) * 100.0f));
    }
    else if (streq(argv[0], "prof")) engine_profile();
    else if (streq(argv[0], "loop")) cmd_loop(argc, argv);
    else if (streq(argv[0], "irput")) cmd_irput(argc, argv);
    else if (streq(argv[0], "irls")) irstore_print_list();
    else if (streq(argv[0], "irdel")) cmd_irdel(argv[1]);
    else if (streq(argv[0], "cab") && (argc == 1 || !streq(argv[1], "long"))) {
        /* cab [<1-83>]: the edit buffer's cab on, with this type (save: `save`) */
        int ok = 1;
        uint32_t t = argc > 1 ? parse_num(argv[1], &ok) : 0u;
        if (argc > 1 && ok && t >= 1u && t <= IRSTORE_LAST) {
            uint8_t b[4] = {1, 0, (uint8_t)t, 0};
            ui_edit_write(P_CAB_EN, b, sizeof b);
            proto_notify_module(3);
        } else if (argc > 1) {
            log_printf("usage: cab [<1-10 stock | 11-19 user IR | 20-83 long IR>] | cab long <taps>\r\n");
            return;
        }
        const preset_t *p = ui_edit_preset();
        log_printf("cab en=%u type=%u\r\n", pget(p, P_CAB_EN), pget(p, P_CAB_TYPE));
    }
    else if (streq(argv[0], "cab") && argc > 2 && streq(argv[1], "long")) {
        int ok;
        uint32_t n = parse_num(argv[2], &ok);
        int r = ok ? engine_cab_long(n) : -1;
        log_printf("cab long %lu: %s\r\n", (unsigned long)n,
                   r == 0 ? "ok" : r == -2 ? "not available (long IRs need more RAM; max " XSTR(ENGINE_IR_TAPS) ")"
                   : r == -4 ? "busy (IR upload)" : "bad taps");
    }
    else if (streq(argv[0], "stock")) {
        int r = stock_check((const void *)STOCK_FLASH, STOCK_FLASH_SIZE);
        log_printf("stock data: flash %s, %s\r\n", stock_error(r),
                   !g_stock ? "not loaded (amp/cab/tone pass through, drums silent)"
                   : g_stock_factory ? "in use" : "in use (version 1: no factory presets)");
    }
    else if (streq(argv[0], "drums")) {
        drums_t *d = engine_drums();
        int ok;
        if (argc > 1 && streq(argv[1], "on")) drums_start(d);
        else if (argc > 1 && streq(argv[1], "off")) drums_stop(d);
        else if (argc > 2 && streq(argv[1], "bpm")) drums_set_tempo(d, parse_num(argv[2], &ok));
        else if (argc > 2 && streq(argv[1], "level")) drums_set_level(d, parse_num(argv[2], &ok));
        else if (argc > 1) { uint32_t r = parse_num(argv[1], &ok); if (ok && r >= 1 && r <= 40) drums_set_rhythm(d, r - 1u); }
        log_printf("drums %s rhythm %u bpm %u level %u samples %lu patterns %s\r\n", d->on ? "on" : "off",
                   (unsigned)d->rhythm + 1u, (unsigned)d->bpm, (unsigned)d->level,
                   (unsigned long)d->n_samples, d->data ? "yes" : "NO (build with STOCK_MR)");
    }
    else if (streq(argv[0], "btaudio")) {
        uint32_t blocks, fill;
        int32_t peak;
        bt_audio_stats(&blocks, &fill, &peak);
        log_printf("bt audio: blocks %lu fill %lu peak %ld (of 32767)\r\n",
                   (unsigned long)blocks, (unsigned long)fill, (long)peak);
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
    else if (streq(argv[0], "power")) power_console(argc, argv);
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
    else if (streq(argv[0], "fwbegin")) cmd_fwbegin(FW_APP, argv[1], argv[2]);
    else if (streq(argv[0], "fwrec")) cmd_fwbegin(FW_RECOVERY, argv[1], argv[2]);
    else if (streq(argv[0], "fwstock") && argc == 1) log_printf("fwstock formats: " FW_STOCK_FORMATS "\r\n");
    else if (streq(argv[0], "fwstock")) cmd_fwbegin(FW_STOCK, argv[1], argv[2]);
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

/* Stack high-water (`stack`): the 8 kB reserve below _estack (linker.ld) is
 * painted at boot; the lowest word that lost the paint is the deepest use
 * since then (interrupts included: they run on the same stack). */
#define STACK_PAINT 0xC0DEF00Du
#if defined(__arm__)
extern uint32_t __stack_limit__[], _estack[];

static void stack_paint(void)
{
    uint32_t sp;
    __asm volatile ("mov %0, sp" : "=r" (sp));
    /* below the caller's frame, with room for an interrupt frame */
    for (volatile uint32_t *w = __stack_limit__; (uint32_t)(uintptr_t)w < sp - 256u; w++) *w = STACK_PAINT;
}

static void cmd_stack(void)
{
    const uint32_t *w = __stack_limit__;
    while (w < _estack && *w == STACK_PAINT) w++;
    uint32_t size = (uint32_t)((uintptr_t)_estack - (uintptr_t)__stack_limit__);
    uint32_t used = (uint32_t)((uintptr_t)_estack - (uintptr_t)w);
    log_printf("stack: used %u of %u bytes, free %u\r\n", (unsigned)used, (unsigned)size,
               (unsigned)(size - used));
}
#else
static void stack_paint(void) {}
static void cmd_stack(void) { log_printf("stack: used 0 of 8192 bytes, free 8192\r\n"); }
#endif

void console_init(void)
{
    line_len = 0;
    heartbeat_on = 0;
    stack_paint();
}

void console_task(void)
{
    if (fw_active()) { fw_rx_task(); return; }
#ifndef FB200_RECOVERY
    if (irstore_put_active()) { irstore_put_task(); return; }
#endif
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
