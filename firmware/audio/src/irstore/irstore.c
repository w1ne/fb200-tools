/* Long IR store: see irstore.h. Cold code (XIP): it runs only from the main
 * loop and calls flash_store (RAM code) for every write, as proto.c does. */
#include <string.h>
#include "irstore/irstore.h"
#include "crc32.h"
#include "debug/cdc_log.h"

/* Session state: CPU-only, in OCRAM (the DTCM is full). */
#ifdef IRSTORE_HOST_TEST
#define IRSTORE_RAM
#else
#define IRSTORE_RAM __attribute__((section(".ocram")))
#endif

static void print_gain(float g)
{
    uint32_t m = (uint32_t)(g * 10000.0f + 0.5f);
    log_printf("%lu.%04lu", (unsigned long)(m / 10000u), (unsigned long)(m % 10000u));
}

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t get32(const uint8_t *p) { return get16(p) | get16(p + 2) << 16; }

void irstore_encode_entry(uint8_t out[IRSTORE_ENTRY], const irstore_entry_t *e)
{
    uint32_t g;
    memset(out, 0, IRSTORE_ENTRY);
    put16(out + 0, e->taps);
    put16(out + 2, e->flags);
    put32(out + 4, e->rate);
    memcpy(&g, &e->gain, 4);
    put32(out + 8, g);
    put32(out + 12, e->crc);
    memcpy(out + 16, e->name, IRSTORE_NAME);
    out[16 + IRSTORE_NAME - 1] = 0;
}

int irstore_decode_entry(const uint8_t in[IRSTORE_ENTRY], irstore_entry_t *e)
{
    uint32_t g = get32(in + 8);
    memset(e, 0, sizeof *e);
    e->taps = (uint16_t)get16(in);
    e->flags = (uint16_t)get16(in + 2);
    e->rate = get32(in + 4);
    memcpy(&e->gain, &g, 4);
    e->crc = get32(in + 12);
    memcpy(e->name, in + 16, IRSTORE_NAME);
    if (e->taps == 0u || e->taps > IRSTORE_TAPS || e->name[IRSTORE_NAME - 1u] != 0 ||
        !(e->gain > 0.0f && e->gain < 1e6f)) {
        memset(e, 0, sizeof *e);
        return 0;
    }
    return 1;
}

void irstore_table_init(uint8_t *t, uint32_t seq)
{
    memset(t, 0, IRSTORE_TABLE);
    put32(t + 0, IRSTORE_MAGIC);
    put16(t + 4, IRSTORE_VERSION);
    put16(t + 6, IRSTORE_SLOTS);
    put32(t + 8, seq);
    put16(t + 12, IRSTORE_ENTRY);
    irstore_table_seal(t);
}

void irstore_table_seal(uint8_t *t)
{
    put32(t + IRSTORE_TABLE - 4u, crc32_ieee(t, IRSTORE_TABLE - 4u));
}

int irstore_table_valid(const uint8_t *t, uint32_t *seq)
{
    if (get32(t) != IRSTORE_MAGIC || get16(t + 4) != IRSTORE_VERSION ||
        get16(t + 6) != IRSTORE_SLOTS || get16(t + 12) != IRSTORE_ENTRY ||
        get32(t + IRSTORE_TABLE - 4u) != crc32_ieee(t, IRSTORE_TABLE - 4u)) {
        return 0;
    }
    if (seq) *seq = get32(t + 8);
    return 1;
}

int irstore_slot_of(unsigned cab_type)
{
    return cab_type >= IRSTORE_FIRST && cab_type <= IRSTORE_LAST ? (int)(cab_type - IRSTORE_FIRST) : -1;
}

int irstore_ready(void) { return irstore_capacity() >= IRSTORE_END; }

int irstore_current(uint32_t *seq)
{
    uint32_t s[2];
    int ok0 = irstore_table_valid(irstore_map(IRSTORE_INDEX(0)), &s[0]);
    int ok1 = irstore_table_valid(irstore_map(IRSTORE_INDEX(1)), &s[1]);
    int c = ok0 && ok1 ? ((int32_t)(s[1] - s[0]) > 0) : ok1 ? 1 : ok0 ? 0 : -1;
    if (c >= 0 && seq) *seq = s[c];
    return c;
}

int irstore_get(unsigned slot, irstore_entry_t *e)
{
    memset(e, 0, sizeof *e);
    if (!irstore_ready()) return -1;
    int c = irstore_current(NULL);
    if (slot >= IRSTORE_SLOTS || c < 0) return 0;
    return irstore_decode_entry(irstore_map(IRSTORE_INDEX(c)) + IRSTORE_HDR + slot * IRSTORE_ENTRY, e);
}

int irstore_load(unsigned slot, float *dst, unsigned max, irstore_entry_t *e)
{
    int r = irstore_get(slot, e);
    if (r <= 0) return r;
    const uint8_t *data = irstore_map(IRSTORE_DATA(slot));
    if (crc32_ieee(data, e->taps * 4u) != e->crc) return 0;
    unsigned n = e->taps < max ? e->taps : max;
    memcpy(dst, data, n * sizeof dst[0]);
    for (unsigned i = 0; i < n; i++)
        if (!(dst[i] > -1e6f && dst[i] < 1e6f)) dst[i] = 0.0f;   /* NaN/inf -> 0 */
    return (int)n;
}

int irstore_write_entry(unsigned slot, const irstore_entry_t *e, uint8_t *scratch)
{
    if (!irstore_ready() || slot >= IRSTORE_SLOTS) return -1;
    uint32_t seq = 0;
    int c = irstore_current(&seq);
    if (c >= 0) memcpy(scratch, irstore_map(IRSTORE_INDEX(c)), IRSTORE_TABLE);
    else irstore_table_init(scratch, 0);
    put32(scratch + 8, seq + 1u);
    uint8_t *ent = scratch + IRSTORE_HDR + slot * IRSTORE_ENTRY;
    if (e) irstore_encode_entry(ent, e);
    else memset(ent, 0, IRSTORE_ENTRY);
    irstore_table_seal(scratch);
    uint32_t dst = IRSTORE_INDEX(c == 0 ? 1 : 0);   /* the other copy */
    if (irstore_flash_write(dst, scratch, IRSTORE_TABLE) != 0) return -3;
    return irstore_current(NULL) == (c == 0 ? 1 : 0) ? 0 : -4;
}

int irstore_delete(unsigned slot)
{
    irstore_entry_t e;
    int r = irstore_get(slot, &e);
    if (r < 0) return -1;
    uint32_t seq;
    if (r == 0 && irstore_current(&seq) < 0) return 0;   /* nothing stored at all */
    if (r == 0) {
        /* empty or malformed: clear the entry only when it holds anything */
        const uint8_t *ent = irstore_map(IRSTORE_INDEX(irstore_current(NULL))) + IRSTORE_HDR +
                             slot * IRSTORE_ENTRY;
        unsigned i = 0;
        while (i < IRSTORE_ENTRY && ent[i] == 0) i++;
        if (i == IRSTORE_ENTRY) return 0;
    }
    float *buf = irstore_buf_get();
    if (!buf) return -2;
    int w = irstore_write_entry(slot, NULL, (uint8_t *)buf);
    irstore_buf_put();
    return w == 0 ? 1 : -3;
}

static unsigned irstore_name_len(const char *name)
{
    unsigned n = 0;
    while (n < IRSTORE_NAME && name[n]) n++;
    return n;
}

/* ---- console output (irls, irdel) ---- */

void irstore_print_list(void)
{
    if (!irstore_ready()) {
        log_printf("ir store: not available: flash %lu kB, needs %lu kB\r\n",
                   (unsigned long)(irstore_capacity() / 1024u), (unsigned long)(IRSTORE_END / 1024u));
        return;
    }
    uint32_t seq = 0;
    int copy = irstore_current(&seq);
    unsigned used = 0;
    for (unsigned i = 0; i < IRSTORE_SLOTS; i++) {
        irstore_entry_t e;
        if (irstore_get(i, &e) <= 0) continue;
        used++;
        /* ~0.8 ms per 4096-tap slot: keep the audio going between slots */
        int good = crc32_ieee(irstore_map(IRSTORE_DATA(i)), e.taps * 4u) == e.crc;
        irstore_pump();
        log_printf("ir %u taps %u rate %lu gain ", i + IRSTORE_FIRST, (unsigned)e.taps,
                   (unsigned long)e.rate);
        print_gain(e.gain);
        log_printf(" crc %08lx %s name %s\r\n", (unsigned long)e.crc, good ? "ok" : "BAD", e.name);
    }
    log_printf("ir store: %u of %u slots used (cab %u-%u, %u taps), table %s seq %lu\r\n", used,
               IRSTORE_SLOTS, IRSTORE_FIRST, IRSTORE_LAST, IRSTORE_TAPS,
               copy < 0 ? "empty" : copy ? "B" : "A", (unsigned long)seq);
}

void irstore_print_delete(unsigned cab_type)
{
    int slot = irstore_slot_of(cab_type);
    int r = slot < 0 ? -1 : irstore_delete((unsigned)slot);
    log_printf(r == 1 ? "ir %u deleted\r\n" : r == 0 ? "ir %u: empty\r\n"
               : r == -1 ? "ir %u: FAILED (store not available)\r\n"
               : r == -2 ? "ir %u: FAILED (busy)\r\n" : "ir %u: FAILED (flash write)\r\n",
               cab_type);
}

int irstore_name_ok(const char *name)
{
    unsigned n = irstore_name_len(name);
    if (n == 0u || n > IRSTORE_NAME - 1u) return 0;
    for (unsigned i = 0; i < n; i++)
        if (name[i] <= ' ' || name[i] > '~') return 0;
    return 1;
}

/* ---- upload session ---- */

enum { PUT_IDLE, PUT_RX, PUT_WRITE, PUT_TABLE };

static struct {
    int state;
    unsigned slot, sector;
    uint32_t len, rx, last_ms;
    float *buf;
    irstore_entry_t e;
} s IRSTORE_RAM;

int irstore_put_begin(unsigned cab_type, unsigned taps, uint32_t crc, const char *name,
                      uint32_t rate)
{
    int slot = irstore_slot_of(cab_type);
    if (slot < 0 || taps == 0u || taps > IRSTORE_TAPS || !irstore_name_ok(name)) {
        log_printf("ir: usage: irput <slot %u-%u> <taps 1-%u> <crc32> <name, 1-23 chars, "
                   "no spaces> [rate]\r\n", IRSTORE_FIRST, IRSTORE_LAST, IRSTORE_TAPS);
        return -1;
    }
    if (!irstore_ready()) {
        log_printf("ir: store not available: flash %lu kB, needs %lu kB\r\n",
                   (unsigned long)(irstore_capacity() / 1024u), (unsigned long)(IRSTORE_END / 1024u));
        return -2;
    }
    if (s.state != PUT_IDLE || (s.buf = irstore_buf_get()) == NULL) {
        log_printf("ir: busy (an upload runs, or this build has no long-IR RAM)\r\n");
        return -3;
    }
    memset(&s.e, 0, sizeof s.e);
    s.e.taps = (uint16_t)taps;
    s.e.rate = rate;
    s.e.crc = crc;
    memcpy(s.e.name, name, irstore_name_len(name));   /* checked above; the rest is 0 */
    s.slot = (unsigned)slot;
    s.len = taps * 4u;
    s.rx = 0;
    s.sector = 0;
    s.last_ms = irstore_now_ms();
    s.state = PUT_RX;
    log_printf("ir ready\r\n");
    return 0;
}

int irstore_put_active(void) { return s.state != PUT_IDLE; }

static void put_end(void)
{
    s.state = PUT_IDLE;
    s.buf = NULL;
    irstore_buf_put();
}

void irstore_put_task(void)
{
    uint32_t now = irstore_now_ms();
    if (s.state == PUT_RX) {
        uint32_t n;
        while (s.rx < s.len && (n = irstore_rx((uint8_t *)s.buf + s.rx, s.len - s.rx)) != 0u) {
            s.rx += n;
            s.last_ms = now;
        }
        if (s.rx < s.len) {
            if (now - s.last_ms > IRSTORE_IDLE_MS) {
                log_printf("ir aborted at %lu/%lu bytes (nothing written)\r\n",
                           (unsigned long)s.rx, (unsigned long)s.len);
                put_end();
            }
            return;
        }
        uint32_t got = crc32_ieee((const uint8_t *)s.buf, s.len);
        if (got != s.e.crc) {
            log_printf("ir done crc=%08lx BAD (nothing written)\r\n", (unsigned long)got);
            put_end();
            return;
        }
        for (unsigned i = s.e.taps; i < 512u; i++) s.buf[i] = 0.0f;   /* the gain reads 512 */
        s.e.gain = irstore_gain(s.buf);
        s.state = PUT_WRITE;
        return;                                         /* one flash sector per call */
    }
    if (s.state == PUT_WRITE) {
        uint32_t off = s.sector * IRSTORE_SECTOR;
        uint32_t n = s.len - off < IRSTORE_SECTOR ? s.len - off : IRSTORE_SECTOR;
        int r = irstore_flash_write(IRSTORE_DATA(s.slot) + off, (const uint8_t *)s.buf + off, n);
        if (r != 0) {
            log_printf("ir FAILED: flash write %d at 0x%08lx\r\n", r,
                       (unsigned long)(IRSTORE_DATA(s.slot) + off));
            put_end();
            return;
        }
        if (off + n < s.len) { s.sector++; return; }
        uint32_t got = crc32_ieee(irstore_map(IRSTORE_DATA(s.slot)), s.len);
        if (got != s.e.crc) {
            log_printf("ir FAILED: flash crc=%08lx\r\n", (unsigned long)got);
            put_end();
            return;
        }
        s.state = PUT_TABLE;
        return;
    }
    if (s.state == PUT_TABLE) {
        /* the data is in flash: the buffer holds the new table now */
        int r = irstore_write_entry(s.slot, &s.e, (uint8_t *)s.buf);
        if (r != 0) {
            log_printf("ir FAILED: table write %d\r\n", r);
        } else {
            log_printf("ir done crc=%08lx ok slot %u taps %u gain ", (unsigned long)s.e.crc,
                       s.slot + IRSTORE_FIRST, (unsigned)s.e.taps);
            print_gain(s.e.gain);
            log_printf("\r\n");
        }
        put_end();
    }
}
