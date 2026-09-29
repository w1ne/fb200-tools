/* Host fuzzer for the firmware's untrusted-input parsers, run under
 * ASan/UBSan (tests/test_fuzz_host.py builds it with clang and with gcc):
 *
 *   proto    app protocol (src/proto/proto.c): frame reader, every fn
 *            handler (IR upload 0x61, preset writes 0x97, rename 0x99,
 *            settings 0xB0/B7/B8, ...), both transports, on top of the
 *            real UI state (ui.c) and a fake flash that may be corrupt
 *   console  USB console (src/debug/console.c): line reader and every
 *            command's argument parsing; each memory access it makes is
 *            checked against the memory that exists on the pedal
 *   preset   preset/settings from flash (preset_check.c) and what the DSP
 *            does with them (the calls of engine_apply_preset): an erased,
 *            random or out-of-range record must play finite audio
 *   stock    stock data blob (dsp/stock_data.c): truncated, corrupt, and
 *            CRC-valid blobs with bad tables; accepted blobs drive the drums
 *
 *   fuzz_host_test <mode> <seed> <seconds> [max_iters]
 *   fuzz_host_test cases          regression cases, one "ok <name>" each
 *
 * A fuzz input is a byte string read as a small program (per-mode ops), so
 * random generation and mutation work on the same inputs. With clang the
 * build adds -fsanitize-coverage=trace-pc-guard (FUZZ_COVERAGE): inputs that
 * reach new edges are kept and mutated (a small coverage-guided loop, no
 * libFuzzer needed). Any oracle failure aborts with the input in hex. */
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* console.c memory access -> checked fakes (see host_rd8 below) */
static uint8_t host_rd8(uint32_t a);
static uint32_t host_rd32(uint32_t a);
static void host_wr8(uint32_t a, uint32_t v);
static void host_wr32(uint32_t a, uint32_t v);
static uint32_t host_crc(uint32_t a, uint32_t n);
#define CONSOLE_RD8(a) host_rd8(a)
#define CONSOLE_RD32(a) host_rd32(a)
#define CONSOLE_WR8(a, v) host_wr8((a), (v))
#define CONSOLE_WR32(a, v) host_wr32((a), (v))
#define CONSOLE_CRC(a, n) host_crc((a), (n))
#include "debug/console.c"

#include "proto/proto.h"
#include "preset/preset.h"
#include "ui/ui.h"
#include "ui/controls.h"
#include "dsp/stock_data.h"
#include "dsp/drums.h"
#include "dsp/eq.h"
#include "dsp/testgen.h"
#include "dsp/math.h"
#include "dsp/amp.h"
#include "dsp/cab.h"
#include "dsp/gate.h"
#include "dsp/comp.h"
#include "dsp/mod.h"
#include "dsp/reverb.h"
#include "dsp/delay.h"
#include "dsp/looper.h"
#include "irstore/irstore.h"
#include "crc32.h"

#define FAIL(...) do { fprintf(stderr, "FAIL: " __VA_ARGS__); fprintf(stderr, "\n"); fail_dump(); abort(); } while (0)
#define CHECK(c, ...) do { if (!(c)) FAIL(__VA_ARGS__); } while (0)

/* ---- the current input (dumped on failure) ---------------------------- */
static const uint8_t *in_p;
static size_t in_n, in_pos;
static void fail_dump(void)
{
    fprintf(stderr, "input (%zu bytes): ", in_n);
    for (size_t i = 0; i < in_n; i++) fprintf(stderr, "%02x", in_p[i]);
    fprintf(stderr, "\n");
}
static uint8_t rd8(void) { return in_pos < in_n ? in_p[in_pos++] : 0; }
static uint16_t rd16(void) { uint16_t v = rd8(); return (uint16_t)(v | rd8() << 8); }
static int more(void) { return in_pos < in_n; }

/* ---- deterministic PRNG ------------------------------------------------ */
static uint64_t rng = 1;
static uint32_t rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}

/* ---- fake flash + stock data ------------------------------------------- */
#define FLASH_SIZE 0x100000u
static uint8_t flash[FLASH_SIZE];
static stock_factory_t fake_factory;
static int stores;

void preset_read(unsigned index, preset_t *out)
{
    memcpy(out, flash + PRESET_FLASH + (index % PRESET_COUNT) * PRESET_STRIDE, sizeof *out);
}
int preset_write(unsigned index, const preset_t *p)
{
    if (index >= PRESET_COUNT) return -1;
    memcpy(flash + PRESET_FLASH + index * PRESET_STRIDE, p, sizeof *p);
    stores++;
    return 0;
}
void settings_read(settings_t *out) { memcpy(out, flash + SETTINGS_FLASH, sizeof *out); }
int settings_write(const settings_t *s) { memcpy(flash + SETTINGS_FLASH, s, sizeof *s); stores++; return 0; }
void rhythm_settings_read(uint8_t out[RHYTHM_SIZE]) { memcpy(out, flash + RHYTHM_FLASH, RHYTHM_SIZE); }
int rhythm_settings_write(const uint8_t in[RHYTHM_SIZE])
{
    memcpy(flash + RHYTHM_FLASH, in, RHYTHM_SIZE);
    stores++;
    return 0;
}
void proto_flash_read(uint32_t off, void *dst, uint32_t n)
{
    CHECK(off < FLASH_SIZE && n <= FLASH_SIZE - off, "proto flash read %x+%u", off, n);
    memcpy(dst, flash + off, n);
}
int proto_flash_write(uint32_t off, const void *src, uint32_t n)
{
    /* flash_rmw.h: the data store only, never the vendor update flag */
    CHECK(off >= 0x71000u && n <= 0xA1800u - off, "proto flash write outside the store %x+%u", off, n);
    CHECK(!(off < 0x87000u && off + n > 0x86000u), "proto flash write hits the update flag %x+%u", off, n);
    memcpy(flash + off, src, n);
    stores++;
    return 0;
}

/* ---- UI / engine / board stubs ----------------------------------------- */
static char out_log[256];
void log_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out_log, sizeof out_log, fmt, ap);   /* formats every argument */
    va_end(ap);
}
void log_puts(const char *s) { (void)s; }
void log_flush_ms(uint32_t ms) { (void)ms; }
void cdc_log_write(const char *d, size_t n) { (void)d; (void)n; }
void cdc_log_task(void) {}
void wdog_feed(void) {}
void display_text(const char *s) { (void)strlen(s); }
void knob_led(int led, bool on) { (void)led; (void)on; }
static uint16_t knob_raw[16];
static bool knob_moved[16];
uint16_t knob_value(int k) { CHECK(k >= 0 && k < 16, "knob %d", k); return knob_raw[k]; }
bool knob_changed(int k) { bool m = knob_moved[k]; knob_moved[k] = false; return m; }
static struct { fsw_event_t ev; int sw; } evq[8];
static unsigned evq_n;
static bool sw_held[4];
fsw_event_t fsw_event(int *sw)
{
    if (!evq_n) return FSW_NONE;
    fsw_event_t ev = evq[0].ev;
    *sw = evq[0].sw;
    memmove(evq, evq + 1, --evq_n * sizeof evq[0]);
    return ev;
}
bool fsw_down(int sw) { return sw >= 0 && sw < 4 && sw_held[sw]; }
void rgb_set(int i, uint8_t r, uint8_t g, uint8_t b) { (void)i; (void)r; (void)g; (void)b; }
void rgb_fill(uint8_t r, uint8_t g, uint8_t b) { (void)r; (void)g; (void)b; }
bool rgb_show(void) { return true; }
void rgb_config(bool invert, uint8_t sym0, uint8_t sym1) { (void)invert; (void)sym0; (void)sym1; }

static drums_t drums;
static eq_t eq;
static testgen_ctx_t tgen;
static float gain_db;
static int usb_route;
drums_t *engine_drums(void) { return &drums; }
struct eq_s *engine_eq(void) { return &eq; }
void engine_set_tuner(bool on) { (void)on; }
bool engine_tuner_poll(tuner_result_t *out) { (void)out; return false; }
void engine_get_stats(engine_stats_t *out) { memset(out, 0, sizeof *out); }
void engine_set_gain_db(float db)
{
    float g = dsp_db_to_gain(db);
    CHECK(g == g && g < 1e6f, "engine gain %g dB", (double)db);
    gain_db = db;
}
float engine_get_gain_db(void) { return gain_db; }
void engine_set_testgen(int mode, float amp, float freq)
{
    CHECK(mode >= 0 && mode <= 3 && freq >= 0.0f && freq <= 24000.0f, "testgen %d %g", mode, (double)freq);
    testgen_set(&tgen, (testgen_mode_t)mode, amp, freq);
}
void engine_testgen_input(bool on) { (void)on; }
void engine_set_usb_route(int r) { CHECK(r >= 0 && r <= 2, "usb route %d", r); usb_route = r; }
int engine_get_usb_route(void) { return usb_route; }
void engine_profile(void) {}
void engine_cycles(uint32_t *avg, uint32_t *max, uint32_t *budget) { *avg = 1; *max = 2; *budget = 13600; }
static bool muted;
void engine_set_mute(bool m) { muted = m; }
bool engine_get_mute(void) { return muted; }
void engine_set_meters(bool on) { (void)on; }
void engine_drop_tx(uint32_t blocks) { (void)blocks; }
int engine_cab_long(unsigned taps) { return taps > ENGINE_IR_TAPS ? -2 : 0; }
/* the looper (console `loop`, ui.c looper mode): the real state machine on
 * a small memory, attached and detached as the engine does */
static looper_t loop;
static uint8_t loop_mem[2][LOOPER_BLK_BYTES * 8 + 3];
int engine_loop(int a)
{
    CHECK(a >= LOOPER_TAP && a <= LOOPER_CLEAR_A, "loop action %d", a);
    if (loop.state == LOOPER_OFF && (a == LOOPER_REC_A || a == LOOPER_TAP))
        looper_attach(&loop, loop_mem[0], sizeof loop_mem[0], loop_mem[1], sizeof loop_mem[1]);
    return looper_cmd(&loop, a);
}
void engine_loop_poll(void)
{
    float l[DSP_BLOCK] = {0}, r[DSP_BLOCK] = {0};
    looper_process(&loop, l, r, DSP_BLOCK);   /* a block per pass: the fades finish */
    looper_poll(&loop);
    if (loop.state == LOOPER_EMPTY) looper_detach(&loop);
}
void engine_loop_info(looper_info_t *out) { looper_info(&loop, out); }
bool engine_loop_has_mem(void) { return loop.state != LOOPER_OFF; }
int engine_loop_hq(int on) { return looper_set_hq(&loop, on); }
void engine_loop_level(unsigned pct) { CHECK(pct <= 100u, "loop level %u", pct); looper_set_level(&loop, pct); }
volatile float g_meter_peak[2];
int g_bss_writable = 1;
void usb_audio_stats(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d, uint8_t *e, uint8_t *f)
{ *a = *b = *c = *d = 0; *e = *f = 0; }
void usb_audio_host_controls(uint8_t *mute, int16_t *vol, uint32_t *stalls) { *mute = 0; *vol = -256; *stalls = 0; }
bool codec_write(uint16_t reg, uint16_t v) { CHECK(reg <= 0xFF, "codec reg %x", reg); (void)v; return true; }
bool codec_read(uint16_t reg, uint16_t *v) { CHECK(reg <= 0xFF, "codec reg %x", reg); *v = 0; return true; }
bool codec_init(void) { return true; }
void sai_stats(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d, uint32_t *e, uint32_t *f)
{ *a = *b = *c = *d = *e = *f = 0; }
void i2c_scan_all(void) {}
void i2c_dump(uint8_t bus, uint8_t addr) { (void)bus; (void)addr; }
void i2c_dump_found(void) {}
void led_set(bool on) { (void)on; }
bool led_get(void) { return false; }
void led_select(int gpio, int pin) { CHECK(gpio >= 0 && gpio <= 4 && pin >= 0 && pin <= 31, "led pin"); }
void led_scan_start(void) {}
bool led_scan_active(void) { return false; }
int led_candidate_count(void) { return 3; }
void fw_begin(fw_target_t t, uint32_t len, uint32_t crc) { (void)t; (void)len; (void)crc; }
int fw_active(void) { return 0; }
void fw_rx_task(void) {}
void fw_info(void) {}
void fw_test(void) {}
void crumbs_print(void) {}
void crashdump_print(void) {}
void crashdump_clear(void) {}
void clocks_print(void) {}
int slot_valid(const char **why) { *why = "no"; return 0; }
void recovery_launch_app(int usb_up) { (void)usb_up; abort(); }
static jmp_buf reboot_jmp;
void recovery_request(void) { longjmp(reboot_jmp, 1); }
void console_reboot(void) { longjmp(reboot_jmp, 1); }
static power_state_t pwr;
const power_state_t *power_state(void) { return &pwr; }
void power_activity(void) {}
void power_console(int argc, char **argv) { CHECK(argc >= 1 && argv[0], "power argv"); }   /* power.c: hardware */
void cpu_busy(uint32_t *busy, uint32_t *wakes) { *busy = 0; *wakes = 0; }
bool cpu_sleep_enabled(void) { return true; }
unsigned cpu_clock_mhz(void) { return 600; }
static bool dither;
void engine_set_dither(bool on) { dither = on; }
bool engine_get_dither(void) { return dither; }
int bt_at(const char *cmd) { CHECK(strlen(cmd) < 100, "bt at"); return 0; }
void bt_status(void) {}
void bt_audio_stats(uint32_t *b, uint32_t *f, int32_t *p) { *b = *f = 0; *p = 0; }
void proto_battery(uint8_t *percent, uint8_t *charging) { *percent = 50; *charging = 0; }
void proto_hook_bt_name(const uint8_t name[20]) { (void)name; }
void proto_hook_bt_enable(bool on) { (void)on; }
void proto_hook_bootloader(void) {}
int proto_hook_factory_reset(void) { return ui_factory_reset(); }
void proto_hook_ir_changed(unsigned slot) { CHECK(slot < IR_SLOTS, "ir hook slot %u", slot); }

/* Peripherals the console may touch: the stubbed reg_access_ok() lets these
 * through (the target checks the clock gate too). */
static const struct { uint32_t base, end; } kPeriph[] = {
    {0x400F8000u, 0x400FC000u},   /* SRC */
    {0x401B8000u, 0x401C8000u},   /* GPIO1-4 */
    {0x401F4000u, 0x401F8000u},   /* OCOTP */
    {0x400FC000u, 0x40100000u},   /* CCM */
    {0xE000E000u, 0xE000F000u},   /* SCS */
};
static int periph_range(uint32_t a, uint32_t n)
{
    for (unsigned i = 0; i < sizeof kPeriph / sizeof kPeriph[0]; i++)
        if (a >= kPeriph[i].base && a < kPeriph[i].end && n <= kPeriph[i].end - a) return 1;
    return 0;
}
static int periph(uint32_t a) { return periph_range(a, 1); }
int reg_access_ok(uint32_t addr, const char **name)
{
    *name = "";
    if (addr & 3u) return 0;
    if (addr < 0x40000000u || (addr >= 0x60000000u && addr < 0xE0000000u)) { *name = "memory"; return 1; }
    return periph(addr);
}

/* Memory that exists on the pedal (linker.ld), independent of console.c's
 * own table: RAM and the flash window. */
static const struct { uint32_t base, end; int ram; } kReal[] = {
    {0x00000000u, 0x00020000u, 1}, {0x20000000u, 0x20058000u, 1}, {0x20200000u, 0x20208000u, 1},
    {0x60000000u, 0x60800000u, 0},
};
static int real(uint32_t a, uint32_t n, int ram)
{
    for (unsigned i = 0; i < sizeof kReal / sizeof kReal[0]; i++)
        if ((!ram || kReal[i].ram) && a >= kReal[i].base && a < kReal[i].end && n <= kReal[i].end - a) return 1;
    return 0;
}
static int accesses;
static uint8_t host_rd8(uint32_t a)
{
    CHECK(real(a, 1, 0) || periph(a), "console reads %08x: no memory there", a);
    accesses++;
    return (uint8_t)a;
}
static uint32_t host_rd32(uint32_t a)
{
    CHECK(real(a, 4, 0) || (periph(a) && !(a & 3u)), "console reads u32 %08x: no memory there", a);
    accesses++;
    return a;
}
static void host_wr8(uint32_t a, uint32_t v)
{
    (void)v;
    CHECK(real(a, 1, 1), "console writes %08x: not RAM", a);
    accesses++;
}
static void host_wr32(uint32_t a, uint32_t v)
{
    (void)v;
    CHECK(real(a, 4, 1) || (periph(a) && !(a & 3u)), "console writes u32 %08x", a);
    accesses++;
}
static uint32_t host_crc(uint32_t a, uint32_t n)
{
    CHECK(real(a, n, 0) || periph_range(a, n), "console crc %08x+%u: not one memory region", a, n);
    accesses++;
    return a ^ n;
}

/* fake CDC input for console_task() */
static uint8_t cdc[512];
static uint32_t cdc_head, cdc_tail;
uint32_t tud_cdc_available(void) { return cdc_head - cdc_tail; }
int32_t tud_cdc_read_char(void) { return cdc_tail < cdc_head ? cdc[cdc_tail++] : -1; }

/* ---- flash chip + long IR store (irstore.c, real, on its own fake flash) -- */
int flash_read_id(uint8_t id[3]) { id[0] = 0xEF; id[1] = 0x40; id[2] = 0x18; return 1; }
uint32_t flash_window(void) { return 0x01000000u; }
uint32_t flash_chip_size(const uint8_t id[3]) { return 1u << id[2]; }
uint32_t flash_capacity(void) { return 0x01000000u; }
static uint8_t ir_flash[IRSTORE_END - IRSTORE_BASE];
static float ir_buf[IRSTORE_TAPS];
static int ir_buf_busy;
static uint32_t ir_ms;
const uint8_t *irstore_map(uint32_t off)
{
    CHECK(off >= IRSTORE_BASE && off < IRSTORE_END, "irstore map %x", off);
    return ir_flash + (off - IRSTORE_BASE);
}
int irstore_flash_write(uint32_t off, const void *data, uint32_t len)
{
    /* flash_rmw.h: inside the store, inside one sector */
    CHECK(off >= IRSTORE_BASE && len >= 1u && len <= IRSTORE_END - off &&
          (off & (IRSTORE_SECTOR - 1u)) + len <= IRSTORE_SECTOR, "irstore write %x+%u", off, len);
    memcpy(ir_flash + (off - IRSTORE_BASE), data, len);
    stores++;
    return 0;
}
uint32_t irstore_capacity(void) { return flash_capacity(); }
uint32_t irstore_rx(uint8_t *dst, uint32_t max)
{
    uint32_t n = 0;
    while (n < max && cdc_tail < cdc_head) dst[n++] = cdc[cdc_tail++];
    return n;
}
uint32_t irstore_now_ms(void) { return ir_ms += 500u; }   /* an idle upload times out */
float irstore_gain(const float *ir) { return cab_user_ir_gain(ir); }
void irstore_pump(void) {}
float *irstore_buf_get(void)
{
    if (ir_buf_busy) return NULL;
    ir_buf_busy = 1;
    return ir_buf;
}
void irstore_buf_put(void)
{
    CHECK(ir_buf_busy, "irstore buffer given back twice");
    ir_buf_busy = 0;
}

/* ---- invariants after every op ------------------------------------------ */
static void check_state(void)
{
    preset_t e = *ui_edit_preset();
    CHECK(preset_sanitize(&e) == 0, "edit buffer holds out-of-range fields");
    settings_t s = *ui_settings();
    CHECK(settings_sanitize(&s) == 0, "settings hold out-of-range fields");
    CHECK(ui_preset_index() < PRESET_COUNT, "preset index %u", ui_preset_index());
    CHECK(drums.rhythm < DRUMS_RHYTHMS && drums.level <= 100 && drums.bpm >= DRUMS_BPM_MIN &&
          drums.bpm <= DRUMS_BPM_MAX, "drum settings out of range");
}

/* ---- state reset (a boot with the current flash) ----------------------- */
static void seed_flash(void)
{
    memset(flash, 0xFF, sizeof flash);
    for (unsigned i = 0; i < PRESET_COUNT; i++) {
        preset_t p;
        memset(&p, 0, sizeof p);
        snprintf((char *)p.b, 20, "Preset %02u", i);
        pset(&p, P_AMP_EN, 1);
        pset(&p, P_AMP_MODEL, (uint16_t)(1 + i % 10));
        pset(&p, P_CAB_EN, 1);
        pset(&p, P_CAB_TYPE, (uint16_t)(1 + i % 19));
        pset(&p, P_REV_TYPE, (uint16_t)(i % 6));
        memcpy(flash + PRESET_FLASH + i * PRESET_STRIDE, &p, sizeof p);
    }
    memcpy(flash + SETTINGS_FLASH, settings_default, SETTINGS_SIZE);
}

static void boot(void)
{
    memset(&drums, 0, sizeof drums);
    drums.bpm = DRUMS_BPM_DEFAULT;
    eq_init(&eq, 44100.0f);
    testgen_init(&tgen, 44100.0f);
    evq_n = 0;
    memset(sw_held, 0, sizeof sw_held);
    cdc_head = cdc_tail = 0;
    ui_init();
    proto_init();
    console_init();
}

/* ---- proto mode ---------------------------------------------------------- */
static void on_frame(const uint8_t *f, uint32_t n)
{
    CHECK(n >= 7 && n <= PROTO_MAX_FRAME && f[0] == 0xAA && f[1] == 0x55, "bad frame out (%u)", n);
    uint32_t len = (uint32_t)(f[2] | f[3] << 8);
    CHECK(len >= 1 && len <= PROTO_MAX_LEN && len + 6u == n, "frame length %u/%u", len, n);
    uint16_t crc = proto_crc16(f + 2, len + 2u);
    CHECK(f[4 + len] == (uint8_t)(crc >> 8) && f[5 + len] == (uint8_t)crc, "frame CRC out");
}

static const uint8_t kFns[] = {
    0x00, 0xFA, 0xC3, 0x61, 0x63, 0x65, 0x67, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x94,
    0x96, 0x97, 0x98, 0x99, 0xA0, 0xA3, 0xB0, 0xB2, 0xB3, 0xB7, 0xB8, 0xBA, 0xC1, 0xC4, 0xC9,
    0xD6, 0xD9, 0xDA, 0x10, 0x11, 0xFF,
};

static void feed_split(proto_transport_t t, const uint8_t *b, uint32_t n)
{
    uint32_t chunk = rd8() % 70u + 1u;   /* HID reports are 63, BLE UART reads 64 */
    for (uint32_t o = 0; o < n; o += chunk) proto_feed(t, b + o, n - o < chunk ? n - o : chunk);
}

static void send_frame(proto_transport_t t, uint8_t fn, const uint8_t *p, uint32_t n)
{
    static uint8_t f[PROTO_MAX_FRAME + 8];
    uint32_t len = proto_encode(fn, p, n, f, sizeof f);
    if (len) feed_split(t, f, len);
}

static void payload(uint8_t *p, uint32_t n)
{
    uint8_t fill = rd8();
    for (uint32_t i = 0; i < n; i++) p[i] = more() ? rd8() : (uint8_t)(fill + i * 7u);
}

static void op_flash_damage(void)
{
    static const uint32_t where[] = {PRESET_FLASH, SETTINGS_FLASH, RHYTHM_FLASH, IR_NAMES_FLASH,
                                     IR_FLAGS_FLASH, AUX_FLASH};
    uint32_t base = where[rd8() % 6u];
    if (base == PRESET_FLASH) base += (rd8() % PRESET_COUNT) * PRESET_STRIDE;
    uint32_t n = rd8() % 3u == 0 ? 0x1000u - (base & 0xFFFu) : rd8() % 0x200u + 1u;
    uint8_t how = rd8() % 3u;
    for (uint32_t i = 0; i < n; i++)
        flash[base + i] = how == 0 ? 0xFF : how == 1 ? (uint8_t)rnd() : rd8();
    boot();   /* power loss during the write, then a reboot */
}

static void op_ui(void)
{
    uint8_t k = rd8();
    if (k & 1u) {
        if (evq_n < 8) { evq[evq_n].ev = (fsw_event_t)(1 + rd8() % 3u); evq[evq_n].sw = rd8() % 4u; evq_n++; }
        sw_held[rd8() % 4u] = rd8() & 1u;
    } else {
        unsigned kn = rd8() % 16u;
        knob_raw[kn] = rd16() & 0xFFFu;
        knob_moved[kn] = true;
    }
    static uint32_t now = 1000;
    now += rd16();
    ui_task(now);
}

static void run_proto(void)
{
    static uint8_t p[PROTO_MAX_LEN + 16];
    while (more()) {
        proto_transport_t t = (rd8() & 1u) ? PROTO_BLE : PROTO_USB;
        switch (rd8() % 8u) {
        case 0: {                                           /* raw bytes */
            uint32_t n = rd8();
            payload(p, n);
            feed_split(t, p, n);
            break;
        }
        case 1: case 2: case 3: {                           /* a valid frame */
            uint8_t fn = kFns[rd8() % sizeof kFns];
            uint8_t lm = rd8();
            uint32_t n = lm < 128 ? lm : lm < 192 ? 257u + (lm & 7u) : rd16() % (PROTO_MAX_LEN + 2u);
            if (n > PROTO_MAX_LEN) n = PROTO_MAX_LEN;
            payload(p, n);
            if (fn == 0xB2 && (rd8() & 7u)) break;          /* factory reset: rarely (slow) */
            send_frame(t, fn, p, n);
            break;
        }
        case 4: {                                           /* an IR upload (0x61) */
            uint8_t slot = rd8() % 11u, total = rd8() % 24u, frames = rd8() % 24u;
            for (uint8_t idx = 0; idx < frames; idx++) {
                uint32_t len = rd8() % 3u ? 512u : rd16() % 1100u;
                if (len + 7u > PROTO_MAX_LEN) len = PROTO_MAX_LEN - 7u;
                uint8_t kind = rd8() % 5u ? 1u : rd8();
                uint8_t at = rd8() % 5u ? idx : rd8();
                uint8_t hdr[7] = {kind, slot, 0, total, at, (uint8_t)len, (uint8_t)(len >> 8)};
                if (!(rd8() % 8u)) hdr[5] = rd8(), hdr[6] = rd8();   /* length field lies */
                memcpy(p, hdr, 7);
                payload(p + 7, len);
                send_frame(t, 0x61, p, len + 7u);
            }
            break;
        }
        case 5: {                                           /* a damaged frame */
            static uint8_t f[PROTO_MAX_FRAME + 8];
            uint32_t n = rd8();
            payload(p, n);
            uint32_t len = proto_encode(kFns[rd8() % sizeof kFns], p, n, f, sizeof f);
            if (!len) break;
            uint8_t how = rd8() % 3u;
            if (how == 0) f[rd16() % len] ^= (uint8_t)(1u << (rd8() & 7u));
            else if (how == 1) len = rd16() % len;
            else { f[2] = rd8(); f[3] = rd8(); }
            feed_split(t, f, len);
            break;
        }
        case 6: op_flash_damage(); break;
        default: op_ui(); break;
        }
        check_state();
    }
}

/* ---- console mode --------------------------------------------------------- */
static const char *const kWords[] = {
    "help", "stats", "usb", "codec", "sai", "gain", "g", "testgen", "t", "tin", "cpu", "mute", "m",
    "meters", "x", "led", "ledpin", "creg", "hb", "scan", "dump", "peek", "dumpmem", "src", "poke",
    "crc", "fwinfo", "fwtest", "crumbs", "stack", "ui", "preset", "save", "delay", "eq", "factory",
    "tuner", "prof", "cab", "stock", "drums", "btaudio", "bt", "rgb", "power", "uimon", "disp", "kled",
    "crashdump", "crashclear", "clocks", "peek32", "poke32", "fwbegin", "fwrec", "fwstock", "recovery",
    "reset", "reboot", "boot",
    "on", "off", "scan", "cfg", "send", "long", "bpm", "level", "yes", "out", "in", "mix", "sine",
    "white", "impulse", "hpf", "lpf", "AT+CZ",
    "0", "1", "2", "5", "9", "15", "16", "40", "99", "100", "101", "255", "256", "1000", "2500",
    "4096", "4097", "65535", "65536", "24000", "24001", "4294967295", "4294967296", "99999999999",
    "-1", "-4.5", "3.5", "0.3", "+15", "1e9", "0x", "0x1G", "0xFFFFFFFF", "0x0",
    "0x1FFFF", "0x1FFF0", "0x20000", "0x20000000", "0x20057FFC", "0x20057FFF", "0x20058000",
    "0x20200000", "0x20207FFF", "0x20208000", "0x400F8000", "0x400F8FFF", "0x401B8000",
    "0x60000000", "0x607FFFFF", "0x60800000", "0x3FFFFF", "0x400000", "0xE000ED00", "0x40000000",
    "0xFFFFFF00", "0x60000", "0x7FFFFF", "0x800000",
    "loop", "rec", "play", "dub", "stop", "undo", "clear", "tap", "hq",
    "irput", "irls", "irdel", "jedec", "19", "20", "83", "84", "44100", "0xdeadbeef", "ir_name",
};

static void cdc_put(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (cdc_head - cdc_tail >= sizeof cdc) { cdc_tail = cdc_head = 0; }
        if (cdc_head == sizeof cdc) {   /* compact */
            memmove(cdc, cdc + cdc_tail, cdc_head - cdc_tail);
            cdc_head -= cdc_tail;
            cdc_tail = 0;
        }
        cdc[cdc_head++] = (uint8_t)s[i];
    }
}

static int blocked(const char *w)
{
    /* intentional faults/hangs: tested on the pedal, not here; `stock` reads
     * the blob at its fixed flash address (the stock target covers it) */
    static const char *const no[] = {"crash", "hang", "stock"};
    for (unsigned i = 0; i < 3; i++) if (!strcmp(w, no[i])) return 1;
    return 0;
}

static void run_console(void)
{
    char line[160];
    while (more()) {
        size_t n = 0;
        unsigned words = rd8() % 8u;
        for (unsigned w = 0; w < words && n < sizeof line - 40; w++) {
            uint8_t k = rd8();
            char tmp[40];
            const char *word;
            if (k < 200) word = kWords[k % (sizeof kWords / sizeof kWords[0])];
            else if (k < 220) { snprintf(tmp, sizeof tmp, "%u", (unsigned)(rd16() | (uint32_t)rd16() << 16)); word = tmp; }
            else if (k < 240) { snprintf(tmp, sizeof tmp, "0x%x", (unsigned)(rd16() | (uint32_t)rd16() << 16)); word = tmp; }
            else {                                          /* raw bytes, no line ends */
                size_t m = rd8() % 20u;
                for (size_t i = 0; i < m; i++) { char c = (char)rd8(); tmp[i] = (c == '\r' || c == '\n') ? 'x' : c; }
                tmp[m] = 0;
                word = tmp;
            }
            if (w == 0 && blocked(word)) word = "help";
            size_t l = strlen(word);
            if (n) line[n++] = rd8() % 8u ? ' ' : (char)8;   /* sometimes a backspace */
            memcpy(line + n, word, l);
            n += l;
        }
        if (rd8() % 16u == 0) {                             /* an overlong line */
            size_t pad = rd8();
            while (pad-- && n < sizeof line - 2) line[n++] = 'a';
        }
        line[n++] = rd8() & 1u ? '\r' : '\n';
        line[n] = 0;
        /* the first token after edits could still be a blocked one: check it */
        char probe[160];
        size_t pn = 0;
        for (size_t i = 0; i < n; i++) {
            if (line[i] == 8 || line[i] == 127) { if (pn) pn--; }
            else if (line[i] != '\r' && line[i] != '\n' && pn < 95) probe[pn++] = line[i];
        }
        probe[pn] = 0;
        char *first = probe;
        while (*first == ' ') first++;
        char *end = first;
        while (*end && *end != ' ') end++;
        *end = 0;
        if (blocked(first)) continue;
        cdc_put(line, n);
        if (!setjmp(reboot_jmp)) console_task();
        else boot();   /* reset/reboot/recovery: the pedal reboots */
        check_state();
    }
}

/* ---- preset mode ------------------------------------------------------------ */
static amp_t s_amp;
static cab_t s_cab;
static gate_t s_gate;
static comp_t s_comp;
static mod_t s_mod;
static reverb_t s_rev;
static delay_t s_dly;
static eq_t s_eq;
static int16_t s_line[DELAY_LEN];
static int dsp_ready;

static void dsp_init(void)
{
    amp_init(&s_amp, 44100.0f);
    cab_init(&s_cab);
    gate_init(&s_gate, 44100.0f);
    comp_init(&s_comp, 44100.0f);
    mod_init(&s_mod, 44100.0f);
    reverb_init(&s_rev, 44100.0f);
    delay_init(&s_dly, 44100.0f, s_line);
    eq_init(&s_eq, 44100.0f);
    dsp_ready = 1;
}

/* engine_apply_preset (audio/engine.c), the DSP calls only */
static void apply(const preset_t *p)
{
    int model = pget(p, P_AMP_MODEL);
    (void)amp_set_model(&s_amp, model);
    amp_set_params(&s_amp, pget(p, P_AMP_GAIN), pget(p, P_AMP_BASS), pget(p, P_AMP_MID),
                   pget(p, P_AMP_MIDFREQ), pget(p, P_AMP_TREBLE), pget(p, P_AMP_VOLUME));
    int cab = pget(p, P_CAB_TYPE);
    if (cab >= 1 && cab <= 10) (void)cab_set_model(&s_cab, cab);
    gate_set_params(&s_gate, pget(p, P_GATE_THRESH));
    comp_set_params(&s_comp, pget(p, P_COMP_TYPE), pget(p, P_COMP_ATTACK), pget(p, P_COMP_THRESH),
                    pget(p, P_COMP_RATIO), pget(p, P_COMP_LEVEL));
    mod_set_params(&s_mod, pget(p, P_MOD_TYPE), pget(p, P_MOD_P1), pget(p, P_MOD_P2),
                   pget(p, P_MOD_P3), pget(p, P_MOD_P4));
    reverb_set_params(&s_rev, pget(p, P_REV_TYPE), pget(p, P_REV_LEVEL), pget(p, P_REV_DECAY),
                      pget(p, P_REV_AE), pget(p, P_REV_A8));
    delay_set_params(&s_dly, pget(p, P_DLY_TIME), pget(p, P_DLY_FB), pget(p, P_DLY_MIX),
                     pget(p, P_DLY_LOWCUT), pget(p, P_DLY_TONE));
    eq_load(&s_eq, preset_eq(p));
}

/* Play blocks of a loud input through every module that is on; returns the
 * peak of the stereo output (inf/NaN included). */
static float play(const preset_t *p, unsigned blocks)
{
    float peak = 0.0f;
    for (unsigned b = 0; b < blocks; b++) {
        float x[DSP_BLOCK], l[DSP_BLOCK], r[DSP_BLOCK];
        for (unsigned i = 0; i < DSP_BLOCK; i++) x[i] = (i & 8u) ? 0.9f : -0.9f;   /* a square wave */
        if (pget(p, P_GATE_EN)) gate_process(&s_gate, x, DSP_BLOCK);
        if (pget(p, P_COMP_EN)) comp_process(&s_comp, x, DSP_BLOCK);
        if (pget(p, P_AMP_EN)) amp_process(&s_amp, x, DSP_BLOCK);
        if (pget(p, P_CAB_EN)) cab_process(&s_cab, x, DSP_BLOCK);
        eq_process(&s_eq, x, DSP_BLOCK);
        if (pget(p, P_MOD_EN)) mod_process(&s_mod, x, DSP_BLOCK);
        if (preset_delay_on(p)) delay_process(&s_dly, x, DSP_BLOCK);
        if (pget(p, P_REV_EN)) reverb_process(&s_rev, x, l, r, DSP_BLOCK);
        else { memcpy(l, x, sizeof l); memcpy(r, x, sizeof r); }
        for (unsigned i = 0; i < DSP_BLOCK; i++) {
            float a = l[i] < 0 ? -l[i] : l[i], c = r[i] < 0 ? -r[i] : r[i];
            if (!(a == a) || !(c == c)) return 1e30f;
            if (a > peak) peak = a;
            if (c > peak) peak = c;
        }
    }
    return peak;
}

static void run_preset(void)
{
    if (!dsp_ready) dsp_init();
    while (more()) {
        preset_t p;
        uint8_t how = rd8() % 4u;
        if (how == 0) memset(p.b, 0xFF, sizeof p.b);                 /* erased */
        else if (how == 1) for (unsigned i = 0; i < PRESET_SIZE; i++) p.b[i] = rd8();
        else {                                                       /* a valid one, some words off */
            memcpy(p.b, flash + PRESET_FLASH + (rd8() % PRESET_COUNT) * PRESET_STRIDE, PRESET_SIZE);
            for (unsigned k = rd8() % 8u; k; k--) pset(&p, (rd8() % (PRESET_SIZE / 2u)) * 2u, rd16());
            if (how == 3) { pset(&p, P_DLY_MARK, DLY_MARK); pset(&p, P_EQ_MARK, EQ_MARK); }
        }
        preset_t q = p;
        unsigned fixed = preset_sanitize(&q);
        preset_t q2 = q;
        CHECK(preset_sanitize(&q2) == 0 && !memcmp(&q, &q2, sizeof q), "sanitize not idempotent");
        CHECK(fixed || !memcmp(&p, &q, sizeof p), "sanitize changed bytes without counting them");
        if (preset_erased(&p)) CHECK(fixed == PRESET_SIZE && !preset_erased(&q), "erased preset kept");
        apply(&q);
        float peak = play(&q, 1u + rd8() % 64u);
        /* finite (5 EQ bands at +15 dB on one frequency are +75 dB: allowed) */
        CHECK(peak < 1e6f, "a sanitized preset plays %g", (double)peak);

        settings_t s;
        if (rd8() & 1u) memset(s.b, 0xFF, sizeof s.b);
        else for (unsigned i = 0; i < SETTINGS_SIZE; i++) s.b[i] = rd8();
        settings_t t = s;
        (void)settings_sanitize(&t);
        settings_t t2 = t;
        CHECK(settings_sanitize(&t2) == 0, "settings sanitize not idempotent");
    }
}

/* ---- stock mode ------------------------------------------------------------- */
static struct { stock_data_t d; stock_factory_t f; } blob;

static void stock_fix_crc(void *p, uint32_t size)
{
    stock_data_t *s = p;
    const uint32_t body = offsetof(stock_data_t, crc) + 4u;
    s->crc = crc32_ieee((const uint8_t *)p + body, size - body);
}

static void stock_synthetic(void)
{
    memset(&blob, 0, sizeof blob);
    stock_data_t *s = &blob.d;
    s->magic = STOCK_MAGIC;
    s->version = 2;
    s->size = sizeof blob.d + sizeof blob.f;
    uint32_t off = 0;
    for (unsigned p = 0; p < STOCK_DRUM_PATTERNS; p++) {
        uint32_t n = p < STOCK_DRUM_PATTERNS - 1u ? 52u : STOCK_DRUM_EVENTS - off;
        for (uint32_t i = 0; i < n; i++)   /* note 1..4 every 30 ticks, then the end marker */
            s->drum_events[off + i] = i + 1u < n ? (30u << 16) | ((1u + i % 4u) << 8) | 100u : 0xFF00u;
        s->drum_lens[p] = (uint16_t)n;
        s->drum_beats[p] = (uint8_t)(1u + p % 9u);
        off += n;
    }
    for (unsigned r = 0; r < STOCK_DRUM_RHYTHMS; r++) s->drum_rhythm[r] = (uint8_t)(r * 2u);
    for (unsigned c = 0; c < STOCK_CABS; c++) { s->cab_taps[c][0] = 1.0f; s->cab_gain[c] = 0.5f; }
    stock_fix_crc(&blob, s->size);
}

/* A drum sample bank as in flash block 1: header + 4 samples. */
static uint32_t bank[4096];
static void bank_init(void)
{
    bank[0] = 4;
    bank[1] = 0;
    for (unsigned i = 0; i < 4; i++) { bank[2 + 2 * i] = 1u + i; bank[3 + 2 * i] = 256u; }
    for (unsigned i = 128; i < 4096; i++) { float f = 0.25f; memcpy(&bank[i], &f, 4); }
}

static void run_stock(void)
{
    static uint8_t *cut;
    while (more()) {
        stock_synthetic();
        uint8_t how = rd8() % 4u;
        uint32_t size = blob.d.size;
        if (how == 1) {                                     /* a truncated blob, exact-size buffer */
            uint32_t n = (uint32_t)(rd16() | (uint32_t)rd8() << 16) % (size + 1u);
            cut = realloc(cut, n ? n : 1u);
            memcpy(cut, &blob, n);
            CHECK(stock_check(cut, n) != 0 || n >= size, "a truncated blob passed");
            continue;
        }
        for (unsigned k = rd8() % 16u; k; k--) {            /* damage words */
            uint32_t w = (uint32_t)(rd16() | (uint32_t)rd8() << 16) % (size / 4u);
            if (how != 3 || w >= 4u) ((uint32_t *)(void *)&blob)[w] = (uint32_t)rd16() | (uint32_t)rd16() << 16;
        }
        if (how >= 2) stock_fix_crc(&blob, size);           /* CRC-valid: the tables must be checked */
        if (stock_check(&blob, size) != 0) continue;
        drums_data_t dd;
        drums_data_from_stock(&dd, &blob.d);
        drums_t d;
        CHECK(drums_init(&d, bank, &dd) == 0, "drums_init");
        drums_set_rhythm(&d, rd8());
        drums_set_tempo(&d, DRUMS_BPM_MAX);
        drums_start(&d);
        float out[256];
        for (unsigned b = 0, nb = 1u + rd8() % 8u; b < nb; b++) drums_render(&d, out, 256);
    }
}

/* ---- coverage feedback (clang -fsanitize-coverage=trace-pc-guard) ---------- */
#ifdef FUZZ_COVERAGE
#define MAX_EDGES (1u << 18)
static uint8_t edge_hit[MAX_EDGES];
static uint32_t edges, edges_new;
__attribute__((no_sanitize("coverage"))) void __sanitizer_cov_trace_pc_guard_init(uint32_t *start, uint32_t *stop)
{
    if (start == stop || *start) return;
    for (uint32_t *g = start; g < stop; g++) *g = ++edges < MAX_EDGES ? edges : MAX_EDGES - 1u;
}
__attribute__((no_sanitize("coverage"))) void __sanitizer_cov_trace_pc_guard(uint32_t *guard)
{
    if (!edge_hit[*guard]) { edge_hit[*guard] = 1; edges_new++; }
}
#endif

/* ---- driver ------------------------------------------------------------------- */
typedef void (*runner_t)(void);

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
#define CORPUS 256
#define MAX_IN 4096
static uint8_t corpus[CORPUS][MAX_IN];
static size_t corpus_n[CORPUS], ncorpus;

static void run_one(runner_t run, const uint8_t *d, size_t n)
{
    in_p = d;
    in_n = n;
    in_pos = 0;
    if (run == run_proto || run == run_console) {
        seed_flash();
        boot();
    }
    run();
}

static size_t make_input(uint8_t *d)
{
    if (ncorpus && rnd() % 4u) {                            /* mutate a kept input */
        unsigned c = rnd() % (unsigned)ncorpus;
        size_t n = corpus_n[c];
        memcpy(d, corpus[c], MAX_IN);
        if (n == 0) n = 1;
        for (unsigned k = 1u + rnd() % 8u; k; k--) {
            uint32_t pos = rnd() % (uint32_t)n;
            switch (rnd() % 5u) {
            case 0: d[pos] ^= (uint8_t)(1u << (rnd() & 7u)); break;
            case 1: d[pos] = (uint8_t)rnd(); break;
            case 2: d[pos] = (uint8_t)(rnd() & 1u ? 0xFF : 0); break;
            case 3: if (n < MAX_IN) { memmove(d + pos + 1, d + pos, n - pos); d[pos] = (uint8_t)rnd(); n++; } break;
            default: if (n > 1) { memmove(d + pos, d + pos + 1, n - pos - 1); n--; } break;
            }
        }
        return n;
    }
    size_t n = 1u + rnd() % (rnd() % 8u ? 256u : MAX_IN);
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)rnd();
    return n;
}

static runner_t runner(const char *mode)
{
    if (!strcmp(mode, "proto")) return run_proto;
    if (!strcmp(mode, "console")) return run_console;
    if (!strcmp(mode, "preset")) return run_preset;
    if (!strcmp(mode, "stock")) return run_stock;
    return NULL;
}

static void setup(void)
{
    for (unsigned i = 0; i <= STOCK_FACTORY_NAMED; i++) {
        memset(fake_factory.preset[i], 0, STOCK_PRESET_SIZE);
        snprintf((char *)fake_factory.preset[i], 20, i < STOCK_FACTORY_NAMED ? "Factory %02u" : "EMPTY", i);
    }
    g_stock_factory = &fake_factory;
    g_stock = NULL;
    looper_init(&loop);
    memset(ir_flash, 0xFF, sizeof ir_flash);   /* erased: an empty long IR store */
    seed_flash();
    bank_init();
    proto_set_sender(PROTO_USB, on_frame);
    proto_set_sender(PROTO_BLE, on_frame);
}

/* ---- regression cases (one per bug found; tests/test_fuzz_host.py) ---------- */
static void console_line(const char *s)
{
    cdc_put(s, strlen(s));
    cdc_put("\r", 1);
    accesses = 0;
    out_log[0] = 0;
    console_task();
}

static void cases(void)
{
    seed_flash();
    boot();
    /* console: reads past the end of ITCM passed the old start/end-only check */
    console_line("peek 0x1fff0 256");
    CHECK(accesses == 0, "peek across the ITCM end");
    console_line("peek 0x100000 16");
    CHECK(accesses == 0, "peek in the ITCM hole");
    puts("ok console_peek_hole");
    /* console: crc from ITCM to flash spanned every hole in between */
    console_line("crc 0 0x60000000");
    CHECK(accesses == 0, "crc across regions");
    console_line("crc 0x60000000 16");
    CHECK(accesses == 1, "crc inside flash refused");
    puts("ok console_crc_span");
    /* console: a byte poke into the flash window (AHB write: bus fault) */
    console_line("poke 0x60000000 1");
    CHECK(accesses == 0, "poke into flash");
    console_line("poke 0x20000000 1");
    CHECK(accesses == 1, "poke into DTCM refused");
    puts("ok console_poke_flash");
    /* console: dumpmem at the DTCM end */
    console_line("dumpmem 0x20057ff0 4096");
    CHECK(accesses == 0, "dumpmem past DTCM");
    puts("ok console_dumpmem_end");
    /* console: gain 4e9 dB -> inf gain (NaN chain), (int) of 4e9 is UB */
    console_line("gain 4000000000");
    CHECK(gain_db <= 24.0f, "gain accepted");
    puts("ok console_gain_range");
    /* console: testgen freq 4e9 Hz -> (int) of 4e9 is UB */
    console_line("testgen sine 4000000000");
    console_line("tin sine 4000000000");
    puts("ok console_testgen_range");
    /* console: `led` with no argument compared a NULL argv[1] (streq) */
    console_line("led");
    console_line("mute");
    console_line("meters");
    CHECK(!strcmp(out_log, "meters on\r\n"), "meters without an argument");
    puts("ok console_missing_arg");

    /* preset: an erased record (power loss mid-write) played all modules on,
     * every knob at 655 %: a runaway reverb */
    if (!dsp_ready) dsp_init();
    preset_t p;
    memset(p.b, 0xFF, sizeof p.b);
    CHECK(preset_sanitize(&p) == PRESET_SIZE && !memcmp(p.b, "EMPTY", 5), "erased preset");
    puts("ok preset_erased");
    /* preset: reverb decay/level out of range (a whole-preset write, 0x97) */
    memset(p.b, 0, sizeof p.b);
    pset(&p, P_REV_EN, 1);
    pset(&p, P_REV_TYPE, 0);
    pset(&p, P_REV_LEVEL, 0xFFFF);
    pset(&p, P_REV_DECAY, 0xFFFF);
    preset_t raw = p;
    reverb_init(&s_rev, 44100.0f);
    apply(&raw);
    float before = play(&raw, 4000);
    CHECK(preset_sanitize(&p) == 2 && pget(&p, P_REV_DECAY) == 100 && pget(&p, P_REV_LEVEL) == 100, "clamp");
    reverb_init(&s_rev, 44100.0f);
    apply(&p);
    float after = play(&p, 4000);
    printf("reverb decay 0xFFFF: peak %g unchecked, %g checked\n", (double)before, (double)after);
    CHECK(after < 1e6f, "checked preset still runs away");
    puts("ok preset_reverb_runaway");
    /* proto: a whole preset into the edit buffer (0x97 [0xFF]) is checked */
    uint8_t w[1 + PRESET_SIZE];
    w[0] = 0xFF;
    memcpy(w + 1, raw.b, PRESET_SIZE);
    send_frame(PROTO_USB, 0x97, w, sizeof w);
    CHECK(pget(ui_edit_preset(), P_REV_DECAY) == 100, "0x97 FF edit buffer unchecked");
    puts("ok proto_edit_load_checked");
    /* settings: erased -> stock defaults, master 0 (not 255 -> full volume) */
    memset(flash + SETTINGS_FLASH, 0xFF, SETTINGS_SIZE);
    boot();
    CHECK(ui_settings()->b[S_MASTER] == 0 && ui_settings()->b[0] == 'B', "erased settings");
    puts("ok settings_erased");
    /* stock: CRC-valid blob whose rhythm map points past the event lists */
    stock_synthetic();
    blob.d.drum_rhythm[0] = 200;
    stock_fix_crc(&blob, blob.d.size);
    CHECK(stock_check(&blob, blob.d.size) == -5, "bad rhythm map accepted");
    stock_synthetic();
    blob.d.drum_beats[0] = 0;   /* tempo_apply divides by it */
    stock_fix_crc(&blob, blob.d.size);
    CHECK(stock_check(&blob, blob.d.size) == -5, "zero beats accepted");
    puts("ok stock_bad_tables");
    /* console: a stored long IR (cab 20..83) while the looper has the
     * long-IR memory: refused, the looper wins; after clear it is taken */
    seed_flash();
    boot();
    unsigned cab0 = pget(ui_edit_preset(), P_CAB_TYPE);
    console_line("loop rec");
    CHECK(engine_loop_has_mem(), "loop rec took no memory");
    console_line("cab 20");
    CHECK(strstr(out_log, "not available") && pget(ui_edit_preset(), P_CAB_TYPE) == cab0,
          "long IR taken during a loop: %s", out_log);
    console_line("cab 12");
    CHECK(pget(ui_edit_preset(), P_CAB_TYPE) == 12, "user IR refused during a loop");
    console_line("loop clear");
    for (unsigned i = 0; i < 200 && engine_loop_has_mem(); i++) engine_loop_poll();
    CHECK(!engine_loop_has_mem(), "loop clear kept the memory");
    console_line("cab 83");
    CHECK(pget(ui_edit_preset(), P_CAB_TYPE) == 83, "long IR refused after clear: %s", out_log);
    puts("ok console_long_ir_looper");
    puts("cases OK");
}

int main(int argc, char **argv)
{
    setup();
    if (argc >= 2 && !strcmp(argv[1], "cases")) { cases(); return 0; }
    if (argc < 4 || !runner(argv[1])) {
        fprintf(stderr, "usage: %s proto|console|preset|stock <seed> <seconds> [max_iters] | cases\n", argv[0]);
        return 2;
    }
    runner_t run = runner(argv[1]);
    rng = strtoull(argv[2], NULL, 0) * 2654435761u + 1u;
    double secs = atof(argv[3]);
    unsigned long max_iters = argc > 4 ? strtoul(argv[4], NULL, 0) : 0;
    unsigned long iters = 0;
    static uint8_t d[MAX_IN];
    double t0 = now_s();   /* wall clock: a loaded machine runs fewer inputs, not longer */
    while (now_s() - t0 < secs && (!max_iters || iters < max_iters)) {
        size_t n = make_input(d);
#ifdef FUZZ_COVERAGE
        edges_new = 0;
#endif
        run_one(run, d, n);
#ifdef FUZZ_COVERAGE
        if (edges_new) {                                    /* new edges: keep it */
            unsigned slot = ncorpus < CORPUS ? (unsigned)ncorpus++ : rnd() % CORPUS;
            memcpy(corpus[slot], d, n);
            corpus_n[slot] = n;
        }
#endif
        iters++;
    }
#ifdef FUZZ_COVERAGE
    unsigned covered = 0;
    for (unsigned i = 0; i < MAX_EDGES; i++) covered += edge_hit[i];
    printf("fuzz %s: %lu inputs, %u of %u edges, corpus %zu\n", argv[1], iters, covered, edges, ncorpus);
#else
    printf("fuzz %s: %lu inputs\n", argv[1], iters);
#endif
    return 0;
}
