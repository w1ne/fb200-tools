/* FB200 app protocol: frame parser/encoder and command dispatcher, byte
 * compatible with the stock V1.0.1 firmware (dispatcher ITCM 0x46c4, reply
 * builder 0xfad0; docs/PROTOCOL.md has the command reference). */
#include <string.h>
#include "proto/proto.h"
#include "preset/preset.h"
#include "ui/ui.h"

/* ---- CRC16 (table CRC-CCITT, init 0, final XOR 0xFFFF) ---------------- */
static uint16_t crc_table[256];

static void crc_init(void)
{
    for (unsigned i = 0; i < 256u; i++) {
        uint16_t c = (uint16_t)(i << 8);
        for (int b = 0; b < 8; b++) c = (uint16_t)((c & 0x8000u) ? (uint16_t)(c << 1) ^ 0x1021u : (uint16_t)(c << 1));
        crc_table[i] = c;
    }
}

uint16_t proto_crc16(const uint8_t *d, uint32_t n)
{
    if (!crc_table[1]) crc_init();
    uint16_t crc = 0;
    while (n--) crc = (uint16_t)(crc_table[((crc >> 8) ^ *d++) & 0xFFu] ^ (crc << 8));
    return (uint16_t)(crc ^ 0xFFFFu);
}

uint32_t proto_encode(uint8_t fn, const uint8_t *payload, uint32_t n, uint8_t *out, uint32_t cap)
{
    if (n + 1u > PROTO_MAX_LEN || cap < n + 7u) return 0;
    uint32_t len = n + 1u;
    out[0] = 0xAA;
    out[1] = 0x55;
    out[2] = (uint8_t)len;
    out[3] = (uint8_t)(len >> 8);
    out[4] = fn;
    if (n) memmove(out + 5, payload, n);
    uint16_t crc = proto_crc16(out + 2, len + 2u);
    out[4 + len] = (uint8_t)(crc >> 8);
    out[5 + len] = (uint8_t)crc;
    return len + 6u;
}

/* ---- weak action hooks -------------------------------------------------- */
__attribute__((weak)) void proto_hook_bt_name(const uint8_t name[20]) { (void)name; }
__attribute__((weak)) void proto_hook_bt_enable(bool on) { (void)on; }
__attribute__((weak)) void proto_hook_bootloader(void) {}
__attribute__((weak)) int proto_hook_factory_reset(void) { return -1; }
__attribute__((weak)) void proto_hook_ir_changed(unsigned slot) { (void)slot; }

/* ---- state -------------------------------------------------------------- */
typedef struct { uint8_t buf[PROTO_MAX_FRAME]; uint32_t n; } rx_t;
static rx_t rx[2];
static proto_send_fn senders[2];
static uint8_t notify_mask = PROTO_BLE;
static uint8_t tx[PROTO_MAX_FRAME];
static uint8_t pl[PROTO_MAX_LEN];

static uint8_t rhythm[6] = {0, 0, 0, 100, 110, 0};
static uint8_t aux[30];
static uint8_t ir_names[IR_SLOTS * IR_NAME_LEN];
static uint8_t ir_used[IR_SLOTS];
static uint16_t a3_value;   /* fn 0xA3 value; purpose unknown, kept like the stock */
static uint8_t d6_value;    /* fn 0xD6 byte (BLE flow flag in the stock) */

/* IR upload assembly (stock 0x20018bc0 buffer). */
static struct {
    uint8_t name[IR_NAME_LEN];
    uint8_t data[IR_DATA_SIZE];
    uint32_t fill;
    uint16_t slot;
    uint8_t expect;
} up;

static const char kEmpty[] = "Empty";

void proto_set_sender(proto_transport_t t, proto_send_fn fn) { senders[(t & 3u) - 1u] = fn; }
void proto_set_notify_mask(uint8_t mask) { notify_mask = mask; }
const uint8_t *proto_rhythm(void) { return rhythm; }

static void send(uint8_t mask, uint8_t fn, const uint8_t *p, uint32_t n)
{
    uint32_t len = proto_encode(fn, p, n, tx, sizeof tx);
    if (!len) return;
    for (unsigned t = 0; t < 2u; t++)
        if ((mask & (1u << t)) && senders[t]) senders[t](tx, len);
}

static bool erased(const uint8_t *p, uint32_t n)
{
    while (n--) if (*p++ != 0xFF) return false;
    return true;
}

void proto_init(void)
{
    crc_init();
    memset(rx, 0, sizeof rx);
    uint8_t r[6];
    proto_flash_read(RHYTHM_FLASH, r, sizeof r);
    /* stock validation (0x18db2): on<=1, ?<=1, pattern<=39, volume<=100 */
    if (!erased(r, 3) && r[0] <= 1 && r[1] <= 1 && r[2] <= 39 && r[3] <= 100) memcpy(rhythm, r, 6);
    rhythm[0] = 0;                               /* the stock always boots with rhythm off */
    rhythm[3] = (uint8_t)(rhythm[3] / 10u * 10u);
    proto_flash_read(AUX_FLASH, aux, sizeof aux);
    proto_flash_read(IR_NAMES_FLASH, ir_names, sizeof ir_names);
    proto_flash_read(IR_FLAGS_FLASH, ir_used, sizeof ir_used);
    if (ir_names[0] == 0xFF && ir_names[1] == 0xFF) {
        memset(ir_names, 0, sizeof ir_names);
        for (unsigned s = 0; s < IR_SLOTS; s++) memcpy(ir_names + s * IR_NAME_LEN, kEmpty, 5);
    }
    if (ir_used[0] == 0xFF && ir_used[1] == 0xFF) memset(ir_used, 0, sizeof ir_used);
}

/* ---- payload helpers ---------------------------------------------------- */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static void put_str(uint8_t *dst, const char *s, uint32_t field)
{
    memset(dst, 0, field);
    uint32_t n = (uint32_t)strlen(s);
    memcpy(dst, s, n < field ? n : field - 1u);
}

/* Module blocks: base offset in the preset and u16 count (fn 0x80..0x86). */
static const struct { uint8_t off, words; } kModule[7] = {
    {0x14, 6}, {0x5c, 3}, {0x2c, 8}, {0x44, 6}, {0x74, 7}, {0x8c, 5}, {0xa4, 6},
};

static uint32_t module_block(unsigned m, uint8_t *out)
{
    const preset_t *e = ui_edit_preset();
    memcpy(out, e->b + kModule[m].off, kModule[m].words * 2u);
    return kModule[m].words * 2u;
}

static uint32_t settings_block(uint8_t *o)   /* 0xB0, 13 bytes (builder 0x101ae) */
{
    const uint8_t *s = ui_settings()->b;
    unsigned slot = s[S_SLOT] & 3u;
    o[0] = s[S_PRESET];
    o[1] = s[0x19];
    memcpy(o + 2, s + 0x1a, 5);
    memcpy(o + 7, s + 0x2c, 3);
    o[10] = s[S_BT];
    o[11] = s[0x24 + slot];
    o[12] = s[0x28 + slot];
    return 13;
}

static uint32_t version_block(uint8_t *o)    /* 0x01, 55 bytes */
{
    put_str(o, PROTO_PRODUCT, 32);
    put_str(o + 32, PROTO_APP_VERSION, 7);
    put_str(o + 39, PROTO_FW_VERSION, 7);
    put_str(o + 46, PROTO_BT_VERSION, 7);
    put_str(o + 53, PROTO_HW_REV, 2);
    return 55;
}

static uint32_t ir_record(unsigned slot, uint8_t *o)   /* 59 bytes (builder 0x1037c) */
{
    memset(o, 0, 59);
    o[0] = 1;
    wr16(o + 1, (uint16_t)(slot + 1u));
    if (slot < IR_SLOTS && ir_used[slot] == 1) {
        o[3] = 1;
        o[4] = 2;
        memcpy(o + 5, ir_names + slot * IR_NAME_LEN, IR_NAME_LEN);
        o[55] = IR_NAME_LEN;
    }
    return 59;
}

/* ---- notifications ------------------------------------------------------ */
void proto_notify_module(unsigned m)
{
    if (m < 7u) send(notify_mask, (uint8_t)(0x80u + m), pl, module_block(m, pl));
}

void proto_notify_preset(void)
{
    uint8_t i = (uint8_t)ui_preset_index();
    send(notify_mask, 0x98, &i, 1);
    proto_notify_module(3);
}

void proto_notify_settings(void) { send(notify_mask, 0xB0, pl, settings_block(pl)); }
void proto_notify_rhythm(void) { send(notify_mask, 0xBA, rhythm, 6); }
void proto_notify_rhythm_mode(void) { send(notify_mask, 0xC9, &ui_settings()->b[S_RHYTHM], 1); }

void proto_notify_battery(void)
{
    uint8_t b[2];
    proto_battery(&b[0], &b[1]);
    send(notify_mask, 0xBB, b, 2);
}

static void send_preset_from_flash(uint8_t mask, uint8_t fn, unsigned idx)
{
    preset_t p;
    preset_read(idx, &p);
    pl[0] = (uint8_t)idx;
    memcpy(pl + 1, p.b, PRESET_SIZE);
    send(mask, fn, pl, PRESET_SIZE + 1u);
}

static void send_edit(uint8_t mask, unsigned idx)   /* 0xA1 [index][edit buffer] */
{
    pl[0] = (uint8_t)idx;
    memcpy(pl + 1, ui_edit_preset()->b, PRESET_SIZE);
    send(mask, 0xA1, pl, PRESET_SIZE + 1u);
}

void proto_notify_saved(void)
{
    unsigned i = ui_preset_index();
    send_preset_from_flash(notify_mask, 0x97, i);
    uint8_t b = (uint8_t)i;
    send(notify_mask, 0x98, &b, 1);
}

/* ---- module writes (fn 0x80..0x86) -------------------------------------- */
static const uint8_t kAmpDef[55][6] = {
    {80,50,50,1,50,50}, {80,50,50,1,50,50}, {100,35,50,1,40,65}, {85,65,50,1,50,50},
    {75,50,50,1,40,70}, {75,50,30,1,50,70}, {75,50,30,1,40,70}, {75,50,45,1,40,70},
    {74,50,45,1,55,70}, {75,60,50,1,40,70}, {75,50,40,1,50,75}, {100,65,50,1,38,40},
    {35,35,35,1,50,40}, {55,60,50,1,40,60}, {70,45,40,1,50,60}, {75,45,26,1,50,65},
    {45,50,40,1,55,65}, {100,65,25,1,30,50}, {60,45,30,1,45,60}, {100,45,35,1,47,75},
    {65,55,35,1,50,55}, {100,70,45,1,35,40}, {70,50,30,1,60,65}, {40,50,50,1,50,60},
    {70,40,35,1,65,60}, {60,50,40,1,30,60}, {50,40,45,1,60,60}, {100,50,50,1,50,60},
    {100,50,50,1,50,40}, {90,50,50,1,20,50}, {90,50,50,1,30,50}, {100,50,50,1,50,50},
    {100,50,50,1,50,50}, {80,50,50,1,50,50}, {100,40,50,1,50,55}, {40,55,35,1,60,70},
    {50,55,55,1,45,45}, {100,50,50,1,50,50}, {30,50,50,1,50,50}, {50,50,50,1,50,50},
    {100,55,55,1,50,50}, {20,50,50,1,65,65}, {25,50,50,1,50,70}, {100,50,50,1,50,50},
    {70,50,50,1,60,65}, {70,50,50,1,50,50}, {70,50,50,1,50,50}, {20,50,50,1,50,50},
    {30,50,50,1,50,50}, {30,50,50,1,50,50}, {75,50,30,1,50,50}, {75,35,50,1,50,50},
    {0,50,0,1,20,55}, {0,50,0,1,20,55}, {0,50,0,1,20,55},
};
static const uint16_t kModDef[22][5] = {
    {60,50,55,50,120}, {32,50,100,50,120}, {4,35,50,50,120}, {4,50,90,50,120},
    {25,50,50,50,120}, {25,50,50,50,120}, {25,35,50,50,120}, {50,35,50,50,120},
    {42,50,20,50,120}, {50,60,70,75,120}, {50,35,70,50,120}, {65,50,70,50,120},
    {3,80,90,50,120}, {20,50,35,50,120}, {0,100,50,50,120}, {32,50,55,50,120},
    {32,50,100,50,120}, {4,35,50,50,120}, {4,50,90,50,120}, {25,50,50,50,120},
    {25,50,50,50,120}, {5,35,55,60,25},
};
static const uint16_t kDlyDef[7][3] = {
    {40,27,500}, {20,27,500}, {50,50,600}, {30,27,500},
    {50,30,900}, {40,27,900}, {0,0,0},
};
static const uint8_t kRevDef[6][4] = {
    {5,35,55,60}, {25,35,53,35}, {45,60,53,10}, {0,60,25,75},
    {0,47,53,60}, {0,50,60,60},
};

static void set_module(unsigned m, const uint8_t *p, uint32_t n)
{
    unsigned words = kModule[m].words, off = kModule[m].off;
    if (n < words * 2u) return;
    const preset_t *e = ui_edit_preset();
    uint16_t old_type = pget(e, off + 2u);
    uint16_t v[8];
    for (unsigned i = 0; i < words; i++) v[i] = rd16(p + 2u * i);
    if (v[0] > 1) v[0] = 1;
    bool defaults = false;
    switch (m) {
    case 0:   /* 0x80 +0x14: en, type<=21, 4 params<=100 */
        if (v[1] > 21) v[1] = 1;
        for (unsigned i = 2; i < 6; i++) if (v[i] > 100) v[i] = 100;
        break;
    case 1:   /* 0x81 +0x5c: en, type<=4, level<=100 */
        if (v[1] > 4) v[1] = 1;
        if (v[2] > 100) v[2] = 100;
        break;
    case 2:   /* 0x82 +0x2c: en, amp type 1..120, 6 params<=100 */
        if (v[1] > 120) v[1] = 1;
        for (unsigned i = 2; i < 8; i++) if (v[i] > 100) v[i] = 100;
        if (v[0] && v[1] == 0) v[1] = 1;
        if (v[1] != old_type && v[1] >= 1 && v[1] <= 55) {
            for (unsigned i = 0; i < 6; i++) v[2 + i] = kAmpDef[v[1] - 1u][i];
            defaults = true;
        }
        break;
    case 3:   /* 0x83 +0x44: en, cab type 1..120 (11..19 = user IR), p1<=4, p2,p3<=100, p4<=9 */
        if (v[1] > 120) v[1] = 1;
        if (v[2] > 4) v[2] = 4;
        if (v[3] > 100) v[3] = 100;
        if (v[4] > 100) v[4] = 100;
        if (v[5] > 9) v[5] = 9;
        if (v[0] && v[1] == 0) v[1] = 1;
        break;
    case 4:   /* 0x84 +0x74: en, mod type<=21, 4 params<=100, last<=240 */
        if (v[1] > 21) v[1] = 1;
        for (unsigned i = 2; i < 6; i++) if (v[i] > 100) v[i] = 100;
        if (v[6] > 240) v[6] = 100;
        if (v[1] != old_type) {
            for (unsigned i = 0; i < 5; i++) v[2 + i] = kModDef[v[1]][i];
            defaults = true;
        }
        break;
    case 5:   /* 0x85 +0x8c: en, delay type<=6, 2 params<=100, time 40..2500 ms */
        if (v[1] > 6) v[1] = 1;
        if (v[2] > 100) v[2] = 100;
        if (v[3] > 100) v[3] = 100;
        if (v[4] > 2500) v[4] = 2500;
        else if (v[4] < 40) v[4] = 40;
        if (v[1] != old_type) {
            for (unsigned i = 0; i < 3; i++) v[2 + i] = kDlyDef[v[1]][i];
            defaults = true;
        }
        break;
    default:  /* 0x86 +0xa4: en, reverb type<=5, p1<=200, 3 params<=100 */
        if (v[1] > 5) v[1] = 1;
        if (v[2] > 200) v[2] = 200;
        for (unsigned i = 3; i < 6; i++) if (v[i] > 100) v[i] = 100;
        if (v[1] != old_type) {
            for (unsigned i = 0; i < 4; i++) v[2 + i] = kRevDef[v[1]][i];
            defaults = true;
        }
        break;
    }
    uint8_t b[16];
    for (unsigned i = 0; i < words; i++) wr16(b + 2u * i, v[i]);
    ui_edit_write(off, b, words * 2u);
    /* the stock echoes the block (with the new model's defaults) only when a
     * type change loaded defaults */
    if (defaults) proto_notify_module(m);
}

/* ---- IR store (fn 0x61..0x68) ------------------------------------------- */
static void ir_save_meta(void)
{
    proto_flash_write(IR_NAMES_FLASH, ir_names, sizeof ir_names);
    proto_flash_write(IR_FLAGS_FLASH, ir_used, sizeof ir_used);
}

static void ir_notify(unsigned slot)   /* 0x69 record to the app (stock 0x6b, BLE) */
{
    uint8_t r[59];
    send(notify_mask, 0x69, r, ir_record(slot, r));
}

static void ir_upload(uint8_t src, const uint8_t *p, uint32_t n)
{
    if (n < 7) return;
    uint16_t slot = rd16(p + 1), len = rd16(p + 5);
    uint8_t total = p[3], idx = p[4];
    if (p[0] != 1 || slot == 0 || slot > IR_SLOTS || len > n - 7u) return;
    if (idx == 0) {
        memset(up.name, 0, sizeof up.name);
        memcpy(up.name, p + 7, len < IR_NAME_LEN ? len : IR_NAME_LEN);
        up.fill = 0;
        up.slot = slot;
        up.expect = 1;
        memset(up.data, 0, sizeof up.data);
    } else if (slot == up.slot) {
        uint32_t room = IR_DATA_SIZE - up.fill, k = len < room ? len : room;
        memcpy(up.data + up.fill, p + 7, k);
        up.fill += k;
        if (idx == up.expect && idx == (uint8_t)(total - 1u)) {
            unsigned s = slot - 1u;
            memcpy(ir_names + s * IR_NAME_LEN, up.name, IR_NAME_LEN);
            ir_used[s] = 1;
            ir_save_meta();
            proto_flash_write(IR_DATA_FLASH + s * IR_DATA_SIZE, up.data, IR_DATA_SIZE);
            proto_hook_ir_changed(s);
            ir_notify(s);
        }
        up.expect++;
    }
    uint8_t r[4] = {1, (uint8_t)slot, 0, idx};
    send(src, 0x62, r, 4);
}

static void ir_query(uint8_t src, const uint8_t *p, uint32_t n)
{
    if (n < 4) return;
    uint16_t slot = rd16(p + 1);
    uint8_t op = p[3];
    pl[0] = p[0];
    wr16(pl + 1, slot);
    pl[3] = 0;
    uint32_t len = 4;
    if (p[0] == 1 && slot >= 1 && slot <= IR_SLOTS && ir_used[slot - 1u] == 1) {
        unsigned s = slot - 1u;
        pl[3] = 1;
        pl[4] = 21;                    /* frames: 1 name + 20 x 512 data (0x2800) */
        pl[5] = op;
        if (op == 0) {
            wr16(pl + 6, IR_NAME_LEN);
            memcpy(pl + 8, ir_names + s * IR_NAME_LEN, IR_NAME_LEN);
            len = 8 + IR_NAME_LEN;
        } else if (op <= 20) {
            wr16(pl + 6, 0x200);
            proto_flash_read(IR_DATA_FLASH + s * IR_DATA_SIZE + (op - 1u) * 0x200u, pl + 8, 0x200);
            len = 8 + 0x200;
        } else {
            wr16(pl + 6, 0);
            len = 8;
        }
    }
    send(src, 0x64, pl, len);
}

static void ir_list(uint8_t src, const uint8_t *p, uint32_t n)
{
    if (n < 5 || p[0] != 1) return;
    uint16_t first = rd16(p + 1), last = rd16(p + 3);
    if (first == 0 || last > IR_SLOTS || first > last) return;
    uint32_t len = 0;
    for (unsigned s = first; s <= last; s++) len += ir_record(s - 1u, pl + len);
    send(src, 0x66, pl, len);
}

static void ir_edit(uint8_t src, const uint8_t *p, uint32_t n)
{
    if (n < 4) return;
    uint16_t slot = rd16(p + 1);
    uint8_t op = p[3];
    if (p[0] != 1 || slot == 0 || slot > IR_SLOTS || (op != 1 && op != 2)) return;
    unsigned s = slot - 1u;
    uint8_t ok = 1;
    if (op == 1) {                     /* delete */
        ir_used[s] = 0;
        memset(ir_names + s * IR_NAME_LEN, 0, IR_NAME_LEN);
        memcpy(ir_names + s * IR_NAME_LEN, kEmpty, 5);
        ir_save_meta();
        proto_hook_ir_changed(s);
    } else if (ir_used[s] == 1) {      /* rename */
        uint32_t k = n > 4u ? n - 4u : 0u;
        memset(ir_names + s * IR_NAME_LEN, 0, IR_NAME_LEN);
        memcpy(ir_names + s * IR_NAME_LEN, p + 4, k < IR_NAME_LEN ? k : IR_NAME_LEN);
        proto_flash_write(IR_NAMES_FLASH, ir_names, sizeof ir_names);
    } else {
        ok = 0;
    }
    uint8_t r[4] = {1, (uint8_t)slot, (uint8_t)(slot >> 8), ok};
    send(src, 0x68, r, 4);
    ir_notify(s);
}

/* ---- dispatcher ---------------------------------------------------------- */
static void dispatch(uint8_t src, uint8_t fn, const uint8_t *p, uint32_t n)
{
    uint8_t *s = ui_settings()->b;
    switch (fn) {
    case 0x00:
        send(src, 0x01, pl, version_block(pl));
        break;
    case 0xFA:                        /* short version: product, firmware, hw */
        put_str(pl, PROTO_PRODUCT, 32);
        put_str(pl + 32, PROTO_FW_VERSION, 7);
        put_str(pl + 39, PROTO_HW_REV, 2);
        send(src, 0xFB, pl, 41);
        break;
    case 0xC3: {                      /* device info -> 0xC4 */
        static const uint8_t tail[6] = {5, 0, 10, 10, 0, 0};
        put_str(pl, PROTO_PRODUCT, 32);
        pl[32] = (uint8_t)PROTO_HW_REV[0];
        pl[33] = 1;
        put_str(pl + 34, PROTO_FW_VERSION, 7);
        memcpy(pl + 41, tail, 6);
        send(src, 0xC4, pl, 47);
        break;
    }
    case 0x61: ir_upload(src, p, n); break;
    case 0x63: ir_query(src, p, n); break;
    case 0x65: ir_list(src, p, n); break;
    case 0x67: ir_edit(src, p, n); break;
    case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86:
        set_module(fn - 0x80u, p, n);
        break;
    case 0x94: {                      /* app connect: full state dump (0x95 batch) */
        send_edit(src, s[S_PRESET]);
        send(src, 0xB0, pl, settings_block(pl));
        send(src, 0xB7, s + 0x1b, 4);
        send(src, 0xBA, rhythm, 6);
        uint8_t b[2];
        proto_battery(&b[0], &b[1]);
        send(src, 0xBB, b, 2);
        b[0] = 1;
        send(src, 0xB5, b, 1);
        send(src, 0x83, pl, module_block(3, pl));
        send(src, 0xC9, s + S_RHYTHM, 1);
        break;
    }
    case 0x96:                        /* read preset from flash */
        if (n >= 1 && p[0] < PRESET_COUNT) send_preset_from_flash(src, 0x97, p[0]);
        break;
    case 0x97: {                      /* write preset (0xFF: edit buffer only) */
        if (n < 1u + PRESET_SIZE) break;
        preset_t np;
        memcpy(np.b, p + 1, PRESET_SIZE);
        if (p[0] == 0xFF) { ui_edit_load(&np); break; }
        if (p[0] >= PRESET_COUNT) break;
        uint8_t ok = preset_write(p[0], &np) == 0;
        ui_select(p[0]);
        send(src, 0x99, &ok, 1);
        uint8_t i = p[0];
        send(src, 0x98, &i, 1);
        break;
    }
    case 0x98:                        /* select preset */
        if (n >= 1 && p[0] < PRESET_COUNT) {
            ui_select(p[0]);
            proto_notify_preset();
        }
        break;
    case 0x99:                        /* rename preset: [index][name 20] */
        if (n >= 21 && p[0] < PRESET_COUNT) {
            ui_select(p[0]);
            ui_edit_write(P_NAME, p + 1, 20);
            preset_write(p[0], ui_edit_preset());
            send_preset_from_flash(notify_mask, 0x97, p[0]);
            send_edit(notify_mask, p[0]);
        }
        break;
    case 0xA0:                        /* module order, 7 bytes -> preset+0xbc */
        if (n >= 7) ui_edit_write(0xbc, p, 7);
        break;
    case 0xA3:
        if (n >= 2) a3_value = rd16(p);
        break;
    case 0xB0: {                      /* global settings (13 bytes, see settings_block) */
        if (n < 13) break;
        uint8_t old_bt = s[S_BT], old_19 = s[0x19];
        unsigned slot = s[S_SLOT] & 3u;
        s[0x19] = p[1];
        memcpy(s + 0x1a, p + 2, 5);
        memcpy(s + 0x2c, p + 7, 3);
        s[S_BT] = p[10];
        s[0x24 + slot] = p[11];
        s[0x28 + slot] = p[12];
        for (unsigned i = 0x1b; i <= 0x1e; i++) if (s[i] > 72) s[i] = 9;
        ui_settings_changed();
        if (s[S_BT] != old_bt) proto_hook_bt_enable(s[S_BT] != 0);
        if (s[0x19] != old_19) {      /* global cab switch: 0 forces the cab off */
            uint8_t en[2] = {0, 0};
            if (s[0x19]) {
                preset_t st;
                preset_read(s[S_PRESET], &st);
                memcpy(en, st.b + P_CAB_EN, 2);
            }
            ui_edit_write(P_CAB_EN, en, 2);
            proto_notify_module(3);
        }
        break;
    }
    case 0xB2: {                      /* factory reset */
        uint8_t ok = proto_hook_factory_reset() == 0;
        send((uint8_t)(src | PROTO_USB), 0xB2, &ok, 1);
        break;
    }
    case 0xB3:                        /* BT name, 20 bytes */
        if (n >= 20) {
            memcpy(s + 2, p, 20);
            ui_settings_changed();
            proto_flash_write(BTNAME_FLASH, p, 20);
            proto_hook_bt_name(p);
        }
        break;
    case 0xB7:
        if (n >= 4) {
            memcpy(s + 0x1b, p, 4);
            for (unsigned i = 0x1b; i <= 0x1e; i++) if (s[i] > 72) s[i] = 9;
            ui_settings_changed();
        }
        break;
    case 0xB8:                        /* [+0x2c, (unused), +0x2d, +0x2e] */
        if (n >= 4) {
            s[0x2c] = p[0];
            s[0x2d] = p[2];
            s[0x2e] = p[3];
            ui_settings_changed();
        }
        break;
    case 0xBA:                        /* rhythm block */
        if (n >= 6) {
            memcpy(rhythm, p, 6);
            if (rhythm[0] > 1) rhythm[0] = 0;
            if (rhythm[1] > 1) rhythm[1] = 0;
            if (rhythm[2] > 39) rhythm[2] = 0;
            if (rhythm[3] > 100) rhythm[3] = 100;
            proto_flash_write(RHYTHM_FLASH, rhythm, 6);
        }
        break;
    case 0xC1: case 0xC4: {           /* enter the vendor updater */
        uint8_t z = 0, one = 1;
        proto_flash_write(UPDATE_FLAG_FLASH, &z, 1);
        send(PROTO_USB, 0xC2, &one, 1);
        proto_hook_bootloader();
        break;
    }
    case 0xC9:
        if (n >= 1) { s[S_RHYTHM] = p[0]; ui_settings_changed(); }
        break;
    case 0xD6:
        if (n >= 1) d6_value = p[0];
        break;
    case 0xD9:
        send(src, 0xDA, aux, sizeof aux);
        break;
    case 0xDA:
        if (n >= sizeof aux) {
            memcpy(aux, p, sizeof aux);
            proto_flash_write(AUX_FLASH, aux, sizeof aux);
        }
        break;
    default:
        break;                        /* unknown (incl. an identify byte 0x10/0x11): ignored like the stock */
    }
}

/* ---- frame reassembly (stock state machine 0x7264: AA, 55, len<=0x3FA,
 * CRC; on any mismatch drop one byte and resync on the next AA) ---------- */
void proto_feed(proto_transport_t t, const uint8_t *data, uint32_t n)
{
    unsigned ti = (t & 3u) - 1u;
    if (ti > 1u) return;
    rx_t *r = &rx[ti];
    while (n) {
        uint32_t k = sizeof r->buf - r->n;
        if (k > n) k = n;
        memcpy(r->buf + r->n, data, k);
        r->n += k;
        data += k;
        n -= k;
        for (;;) {
            uint32_t drop = 0;
            if (r->n == 0) break;
            if (r->buf[0] != 0xAA) drop = 1;
            else if (r->n < 2) break;
            else if (r->buf[1] != 0x55) drop = 1;
            else if (r->n < 4) break;
            else {
                uint32_t len = rd16(r->buf + 2);
                if (len == 0 || len > PROTO_MAX_LEN) drop = 1;
                else if (r->n < len + 6u) break;
                else {
                    uint16_t crc = proto_crc16(r->buf + 2, len + 2u);
                    if (r->buf[4 + len] != (uint8_t)(crc >> 8) || r->buf[5 + len] != (uint8_t)crc) {
                        drop = 1;
                    } else {
                        dispatch((uint8_t)t, r->buf[4], r->buf + 5, len - 1u);
                        drop = len + 6u;
                    }
                }
            }
            memmove(r->buf, r->buf + drop, r->n - drop);
            r->n -= drop;
        }
    }
}
