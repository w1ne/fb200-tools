/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

/* A simulated W25Q64 for the looper's host tests: see loopflash_sim.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "loopflash_sim.h"
#include "debug/flash_rmw.h"
#include "loopstore/loopstore.h"
#include "loopstore/lsio.h"
#include "dsp/looper.h"

const sim_timing_t SIM_TYPICAL = {400, 45000, 150000, 1};
const sim_timing_t SIM_WORST = {3000, 400000, 2000000, 1};
const sim_timing_t SIM_INSTANT = {0, 0, 0, 1};

#define SIZE 0x800000u
uint64_t sim_us;
unsigned sim_errors;
uint32_t sim_capacity = SIZE;
static void pump_none(void) {}
void (*sim_pump)(void) = pump_none;

static sim_timing_t T;
static uint8_t *mem, *written;
static int wel;
static int prog, er_on;           /* a program runs; an erase runs or is suspended */
static uint64_t prog_end, last;
static int64_t er_left;          /* erase: busy time left */
static uint32_t er_off, er_len;
static int susp;                 /* erase suspended */
static uint64_t susp_ready;      /* WIP clears (tSUS) */

#define ERR(...) do { sim_errors++; if (sim_errors < 20) { printf("SIM ERROR: " __VA_ARGS__); printf("\n"); } } while (0)

void sim_flash_init(const sim_timing_t *t)
{
    T = *t;
    if (!mem) {
        mem = malloc(SIZE);
        written = malloc(SIZE);
    }
    for (uint32_t i = 0; i < SIZE; i++) mem[i] = (uint8_t)(i * 2654435761u >> 24);   /* garbage */
    memset(written, 0, SIZE);
    prog = er_on = 0;
    susp = 0;
    wel = 0;
    last = sim_us;
    lsio_init(t->suspend);
}

void sim_flash_free(void)
{
    free(mem);
    free(written);
    mem = written = NULL;
}

static void update(void)
{
    if (prog && sim_us >= prog_end) prog = 0;
    if (er_on && !susp) {
        er_left -= (int64_t)(sim_us - last);
        if (er_left <= 0) {
            memset(mem + er_off, 0xFF, er_len);
            memset(written + er_off, 0, er_len);  /* nothing readable until programmed */
            er_on = 0;
        }
    }
    last = sim_us;
}

static int in_area(uint32_t off, uint32_t len)
{
    if (len == 0u || off + len < off || off + len > SIZE) return 0;
    if (off >= FLASH_LOOP_BASE) return 1;
    /* the loop-save sector, and nothing between it and the loop area */
    return off >= FLASH_LOOP_META && off + len <= FLASH_LOOP_BASE;
}

/* a command other than status/suspend: allowed now? */
static int free_for(const char *what, uint32_t off, uint32_t len)
{
    update();
    if (prog || (er_on && (!susp || sim_us < susp_ready))) {
        ERR("%s at %06x while busy", what, off);
        return 0;
    }
    if (er_on && susp && off < er_off + er_len && off + len > er_off) {
        ERR("%s at %06x inside the suspended erase %06x+%x", what, off, er_off, er_len);
        return 0;
    }
    return 1;
}

int flash_cmd_init(void)
{
    lsio_quiesce();   /* same as the pedal: a suspended erase ends first */
    return 1;
}

int flash_write_enable(void)
{
    sim_us += 2;
    update();
    if (prog || (er_on && !susp)) return 0;
    wel = 1;
    return 1;
}

int flash_read_status(uint32_t *sr)
{
    sim_us += 3;
    update();
    int wip = prog || (er_on && (!susp || sim_us < susp_ready));
    *sr = (wip ? 1u : 0u) | (wel ? 2u : 0u);
    return 1;
}

int flash_read_status2(uint32_t *sr2)
{
    sim_us += 3;
    update();
    *sr2 = susp ? 0x80u : 0u;
    return 1;
}

int flash_cmd_read(uint32_t off, void *dst, uint32_t len)
{
    sim_us += 5 + len / 12u;
    if (!free_for("read", off, len)) {
        memset(dst, 0xEE, len);
        return 1;
    }
    for (uint32_t i = 0; i < len; i++) {
        if (!written[off + i]) {
            ERR("read of unwritten flash at %06x", off + i);
            break;
        }
    }
    memcpy(dst, mem + off, len);
    return 1;
}

int flash_cmd_program(uint32_t page, const uint32_t *data)
{
    sim_us += 30;
    if (!free_for("program", page, FLASH_PAGE)) return 1;
    if (!wel || page % FLASH_PAGE || !in_area(page, FLASH_PAGE)) {
        ERR("program at %06x (wel %d)", page, wel);
        return 1;
    }
    const uint8_t *d = (const uint8_t *)data;
    for (uint32_t i = 0; i < FLASH_PAGE; i++) {
        if ((mem[page + i] & d[i]) != d[i]) {
            ERR("program over data at %06x (%02x -> %02x)", page + i, mem[page + i], d[i]);
            break;
        }
    }
    for (uint32_t i = 0; i < FLASH_PAGE; i++) {
        mem[page + i] &= d[i];
        written[page + i] = 1;
    }
    wel = 0;
    prog = 1;
    prog_end = sim_us + T.prog_us;
    update();                          /* instant timing: done at once */
    return 1;
}

static int erase(uint32_t off, uint32_t len, uint32_t us)
{
    sim_us += 5;
    update();
    if (prog || er_on) {
        ERR("erase at %06x while %s", off, susp ? "an erase is suspended" : "busy");
        return 1;
    }
    if (!wel || off % len || !in_area(off, len)) {
        ERR("erase at %06x+%x (wel %d)", off, len, wel);
        return 1;
    }
    wel = 0;
    er_on = 1;
    er_off = off;
    er_len = len;
    er_left = us;
    susp = 0;
    last = sim_us;
    update();                          /* instant timing: done at once */
    return 1;
}

int flash_cmd_erase(uint32_t sector) { return erase(sector, FLASH_SECTOR, T.sector_us); }
int flash_cmd_erase_block(uint32_t block) { return erase(block, 0x10000u, T.block_us); }

int flash_cmd_suspend(void)
{
    sim_us += 2;
    update();
    if (er_on && !susp && !prog && T.suspend) {
        susp = 1;
        susp_ready = sim_us + 20;
    }
    return 1;
}

int flash_cmd_resume(void)
{
    sim_us += 2;
    update();
    if (er_on && susp) {
        if (sim_us < susp_ready) sim_us = susp_ready;
        susp = 0;
        last = sim_us;
    }
    return 1;
}

void flash_refresh(uint32_t offset, uint32_t len) { (void)offset; (void)len; }
const void *flash_map(uint32_t offset) { return mem + offset; }
uint32_t flash_now_ms(void) { return (uint32_t)(sim_us / 1000u); }
uint32_t flash_capacity(void) { return sim_capacity; }
void flash_pump(void) { sim_pump(); }

/* ---- the whole looper on it ---- */
static loopio_t s_io;
static loopstore_t s_ls;
static uint16_t s_map[2][LS_MAX_CHUNKS];

struct loopstore *sim_loop_store(void) { return &s_ls; }
void *sim_loop_io(void) { return &s_io; }

void sim_loop_setup(void *looper)
{
    uint32_t end = flash_loop_end();
    (void)ls_init(&s_ls, &s_io, s_map[0], s_map[1], FLASH_LOOP_BASE, end);
    looper_init((looper_t *)looper, &s_io, &s_ls);
    ls_arm(&s_ls);
}
