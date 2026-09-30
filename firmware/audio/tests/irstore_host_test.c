/* Host harness for the long IR store (src/irstore/irstore.c) on a fake
 * 8 MB flash, driven line by line from stdin by tests/test_irstore_host.py:
 *
 *   cap <bytes>              usable flash size (irstore_capacity)
 *   put <cab> <taps> <crc> <name> [rate]   irstore_put_begin
 *   rx <file>                queue the file's bytes as console input
 *   task [n]                 irstore_put_task n times (default 1); prints "active 0|1"
 *   tick <ms>                advance the clock
 *   get <slot>               irstore_get: "get <r> taps rate gain crc name"
 *   load <slot> <max> <file> irstore_load into a buffer, write the taps to file
 *   del <slot>               irstore_delete
 *   current                  irstore_current: "current <copy> <seq>"
 *   flash <off> <len> <file> write fake flash bytes to a file
 *   poke <off> <file>        write a file's bytes into the fake flash
 *   failwrite <n>            the n-th next flash write fails (1 = the next)
 *   writes                   "writes <count> <off>..." since the last `writes`
 *   buf busy|free            the engine's IR buffer is taken / free
 *   gain <g>                 what the stock gain rule returns
 *   gainseen                 nonzero taps (of 512) the gain rule saw; buffer taken
 *   irls | irdel <cab>       the console output of irls / irdel (irstore_print_*)
 *   pumps                    audio pump calls since the last `pumps`
 *   slot <cab>               irstore_slot_of
 *   nameok <name>            irstore_name_ok
 *
 * Every command's output ends with "." on its own line. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#include "irstore/irstore.h"

#define FLASH_SIZE 0x800000u
static uint8_t *flash;
static uint32_t cap = FLASH_SIZE, clock_ms = 1000;
static uint8_t *rxq;
static size_t rxq_len, rxq_pos;
static unsigned fail_at, nwrites;
static uint32_t woff[64];
static int buf_busy, buf_taken;
static float ibuf[IRSTORE_TAPS];
static float gain_value = 0.75f;
static int gain_seen = -1;          /* nonzero taps of the 512 the gain rule saw */
static unsigned rx_chunk = 1000;    /* bytes per irstore_rx call (CDC packets) */

void log_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

const uint8_t *irstore_map(uint32_t off) { return flash + off; }

int irstore_flash_write(uint32_t off, const void *data, uint32_t len)
{
    if (nwrites < 64) woff[nwrites] = off;
    nwrites++;
    if (fail_at && --fail_at == 0) return -2;
    /* flash_store's contract: inside one sector, within the chip */
    if (len == 0 || off / IRSTORE_SECTOR != (off + len - 1) / IRSTORE_SECTOR || off + len > cap)
        return -1;
    memcpy(flash + off, data, len);
    return 0;
}

uint32_t irstore_capacity(void) { return cap; }

uint32_t irstore_rx(uint8_t *dst, uint32_t max)
{
    size_t n = rxq_len - rxq_pos;
    if (n > max) n = max;
    if (n > rx_chunk) n = rx_chunk;
    memcpy(dst, rxq + rxq_pos, n);
    rxq_pos += n;
    return (uint32_t)n;
}

uint32_t irstore_now_ms(void) { return clock_ms; }

float irstore_gain(const float *ir)
{
    gain_seen = 0;
    for (unsigned i = 0; i < 512; i++) gain_seen += ir[i] != 0.0f;
    return gain_value;
}

float *irstore_buf_get(void)
{
    if (buf_busy || buf_taken) return NULL;
    buf_taken = 1;
    for (unsigned i = 0; i < IRSTORE_TAPS; i++) ibuf[i] = 12345.0f;   /* stale content */
    return ibuf;
}

void irstore_buf_put(void) { buf_taken = 0; }

static unsigned pumps;
void irstore_pump(void) { pumps++; }

static unsigned char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *p = malloc(n > 0 ? (size_t)n : 1u);
    *len = fread(p, 1, (size_t)n, f);
    fclose(f);
    return p;
}

static void write_file(const char *path, const void *p, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(p, 1, n, f); fclose(f); }
}

int main(void)
{
    static char line[512];
    flash = malloc(FLASH_SIZE);
    memset(flash, 0xFF, FLASH_SIZE);
#ifdef _WIN32
    /* irls lines already end in CR LF. Text mode would write CR CR LF. */
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    setvbuf(stdout, NULL, _IOLBF, 0);
    while (fgets(line, sizeof line, stdin)) {
        char *argv[8] = {0};
        int argc = 0;
        for (char *t = strtok(line, " \r\n"); t && argc < 8; t = strtok(NULL, " \r\n")) argv[argc++] = t;
        if (!argc) { puts("."); fflush(stdout); continue; }
        const char *c = argv[0];
        unsigned long a1 = argc > 1 ? strtoul(argv[1], NULL, 0) : 0;
        if (!strcmp(c, "cap")) cap = (uint32_t)a1;
        else if (!strcmp(c, "put")) {
            int r = irstore_put_begin((unsigned)a1, (unsigned)strtoul(argv[2], NULL, 0),
                                      (uint32_t)strtoul(argv[3], NULL, 0), argc > 4 ? argv[4] : "",
                                      argc > 5 ? (uint32_t)strtoul(argv[5], NULL, 0) : 44100u);
            printf("put %d\n", r);
        } else if (!strcmp(c, "rx")) {
            free(rxq);
            rxq = read_file(argv[1], &rxq_len);
            rxq_pos = 0;
            if (argc > 2) rx_chunk = (unsigned)strtoul(argv[2], NULL, 0);
        } else if (!strcmp(c, "task")) {
            unsigned n = argc > 1 ? (unsigned)a1 : 1u;
            for (unsigned i = 0; i < n; i++) irstore_put_task();
            printf("active %d\n", irstore_put_active());
        } else if (!strcmp(c, "tick")) clock_ms += (uint32_t)a1;
        else if (!strcmp(c, "get")) {
            irstore_entry_t e;
            int r = irstore_get((unsigned)a1, &e);
            printf("get %d %u %u %.6f %08x %s\n", r, e.taps, e.rate, (double)e.gain, e.crc, e.name);
        } else if (!strcmp(c, "load")) {
            static float dst[IRSTORE_TAPS];
            irstore_entry_t e;
            for (unsigned i = 0; i < IRSTORE_TAPS; i++) dst[i] = -7.0f;
            int r = irstore_load((unsigned)a1, dst, (unsigned)strtoul(argv[2], NULL, 0), &e);
            printf("load %d %.6f\n", r, (double)e.gain);
            if (argc > 3) write_file(argv[3], dst, sizeof dst);
        } else if (!strcmp(c, "del")) printf("del %d\n", irstore_delete((unsigned)a1));
        else if (!strcmp(c, "current")) {
            uint32_t seq = 0;
            int r = irstore_current(&seq);
            printf("current %d %u\n", r, seq);
        } else if (!strcmp(c, "flash")) {
            write_file(argv[3], flash + a1, strtoul(argv[2], NULL, 0));
        } else if (!strcmp(c, "poke")) {
            size_t n;
            unsigned char *p = read_file(argv[2], &n);
            if (p) { memcpy(flash + a1, p, n); free(p); }
        } else if (!strcmp(c, "failwrite")) fail_at = (unsigned)a1;
        else if (!strcmp(c, "writes")) {
            printf("writes %u", nwrites);
            for (unsigned i = 0; i < nwrites && i < 64; i++) printf(" %x", woff[i]);
            printf("\n");
            nwrites = 0;
        } else if (!strcmp(c, "buf")) buf_busy = !strcmp(argv[1], "busy");
        else if (!strcmp(c, "gainseen")) printf("gainseen %d taken %d\n", gain_seen, buf_taken);
        else if (!strcmp(c, "irls")) irstore_print_list();
        else if (!strcmp(c, "irdel")) irstore_print_delete((unsigned)a1);
        else if (!strcmp(c, "pumps")) { printf("pumps %u\n", pumps); pumps = 0; }
        else if (!strcmp(c, "gain")) gain_value = (float)strtod(argv[1], NULL);
        else if (!strcmp(c, "slot")) printf("slot %d\n", irstore_slot_of((unsigned)a1));
        else if (!strcmp(c, "nameok")) printf("nameok %d\n", irstore_name_ok(argc > 1 ? argv[1] : ""));
        else printf("unknown %s\n", c);
        puts(".");
        /* Win32 treats _IOLBF as full buffering, so the pipe stalls without this. */
        fflush(stdout);
    }
    return 0;
}
