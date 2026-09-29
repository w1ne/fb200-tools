/* Host test for src/debug/flash_rmw.c on a fake NOR flash.
 *
 * The fake models what matters for the audio: after an erase or a page
 * program the flash stays busy (WIP) for a number of status polls, and every
 * busy poll must be followed by one audio pump (flash_pump). Reading the
 * flash (flash_map) or sending a command while it is busy is an error, and
 * so is flash access from inside the pump. tests/test_flash_rmw.py runs it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "debug/flash_rmw.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

#define FLASH_SIZE 0x00600000u   /* 6 MB: the long IR store ends at 0x502000 */
static uint8_t flash[FLASH_SIZE];
static uint32_t busy;                   /* status polls left until WIP clears */
static int wel, in_pump;
static uint32_t erase_polls = 40, program_polls = 3;
static uint32_t now_ms, ms_per_poll;
static int fail_status, drop_bit;
/* counters */
static uint32_t busy_polls, pumps, violations, erases, programs, refreshes;
static uint32_t refresh_off, refresh_len;

static void reset_counters(void)
{
    busy_polls = pumps = violations = erases = programs = refreshes = 0;
    refresh_off = refresh_len = 0;
}

int flash_cmd_init(void) { if (busy || in_pump) violations++; return 1; }
int flash_write_enable(void) { if (busy || in_pump) violations++; wel = 1; return 1; }
int flash_cmd_erase(uint32_t sector)
{
    if (busy || in_pump || !wel || (sector & (FLASH_SECTOR - 1u))) violations++;
    memset(flash + sector, 0xFF, FLASH_SECTOR);
    wel = 0;
    busy = erase_polls;
    erases++;
    return 1;
}
int flash_cmd_program(uint32_t page, const uint32_t *data)
{
    if (busy || in_pump || !wel || (page & (FLASH_PAGE - 1u))) violations++;
    const uint8_t *d = (const uint8_t *)data;
    for (uint32_t i = 0; i < FLASH_PAGE; i++) flash[page + i] &= d[i];   /* NOR: 1 -> 0 only */
    if (drop_bit) flash[page] |= 0x01u;
    wel = 0;
    busy = program_polls;
    programs++;
    return 1;
}
int flash_read_status(uint32_t *sr)
{
    if (in_pump) violations++;
    now_ms += ms_per_poll;
    if (fail_status) return 0;
    *sr = busy ? 0x03u : 0x00u;
    if (busy) { busy--; busy_polls++; }
    return 1;
}
void flash_refresh(uint32_t offset, uint32_t len)
{
    if (busy) violations++;
    refreshes++;
    refresh_off = offset;
    refresh_len = len;
}
const void *flash_map(uint32_t offset)
{
    if (busy || in_pump) violations++;   /* garbage while busy, and cache pollution */
    return flash + offset;
}
uint32_t flash_now_ms(void) { return now_ms; }
static uint32_t capacity = 0x01000000u;   /* 16 MB chip */
static uint32_t capacity_calls;
uint32_t flash_capacity(void)
{
    if (busy || in_pump) violations++;   /* a JEDEC read is a flash command */
    capacity_calls++;
    return capacity;
}
void flash_pump(void)
{
    if (in_pump) violations++;
    in_pump = 1;
    pumps++;
    in_pump = 0;
}

static void fill_pattern(void)
{
    for (uint32_t i = 0; i < FLASH_SIZE; i++) flash[i] = (uint8_t)(i * 7u + 3u);
}

static void test_write(void)
{
    fill_pattern();
    static uint8_t before[FLASH_SIZE];
    memcpy(before, flash, FLASH_SIZE);
    uint8_t data[300];
    for (unsigned i = 0; i < sizeof data; i++) data[i] = (uint8_t)(0xA0u ^ i);
    uint32_t off = 0x71000u + 0x400u + 5u;   /* a preset-sized write inside one sector */
    reset_counters();
    int r = flash_rmw(off, data, sizeof data);
    CHECK(r == 0, "flash_rmw -> %d", r);
    CHECK(memcmp(flash + off, data, sizeof data) == 0, "data written");
    CHECK(memcmp(flash + 0x71000u, before + 0x71000u, off - 0x71000u) == 0, "sector head kept");
    CHECK(memcmp(flash + off + sizeof data, before + off + sizeof data,
                 0x72000u - off - sizeof data) == 0, "sector tail kept");
    CHECK(memcmp(flash, before, 0x71000u) == 0 &&
          memcmp(flash + 0x72000u, before + 0x72000u, FLASH_SIZE - 0x72000u) == 0,
          "other sectors untouched");
    CHECK(erases == 1 && programs == FLASH_SECTOR / FLASH_PAGE, "erases %u programs %u",
          erases, programs);
    uint32_t want = erase_polls + programs * program_polls;
    CHECK(busy_polls == want, "busy polls %u, want %u", busy_polls, want);
    CHECK(pumps == busy_polls, "one audio pump per busy poll: pumps %u busy polls %u",
          pumps, busy_polls);
    CHECK(refreshes == 1 && refresh_off == 0x71000u && refresh_len == FLASH_SECTOR,
          "refresh %u (%x, %u)", refreshes, refresh_off, refresh_len);
    CHECK(violations == 0, "%u flash accesses while busy or from the pump", violations);
}

static void test_long_erase(void)
{
    /* a slow sector erase (e.g. 400 ms at ~5 us per poll): the audio keeps
     * being pumped the whole time */
    fill_pattern();
    erase_polls = 80000;
    uint8_t b = 0x5A;
    reset_counters();
    CHECK(flash_rmw(0x89000u + 0x2800u, &b, 1) == 0, "IR slot byte");
    CHECK(pumps == busy_polls && pumps >= 80000, "pumps %u busy polls %u", pumps, busy_polls);
    CHECK(violations == 0, "%u violations", violations);
    erase_polls = 40;
}

static void test_rejects(void)
{
    static const struct { uint32_t off, len; } bad[] = {
        {0x86000u, 1}, {0x86FFFu, 1},             /* the vendor loader's update flag */
        {0x70FFFu, 1}, {0x00000u, 16},            /* below the store */
        {0xA1800u, 1}, {0xA17FFu, 2},             /* past the store */
        {0x71FFFu, 2},                            /* crosses a sector */
        {0x71000u, 0},                            /* empty */
        {0xFFFFF000u, 16}, {0x71000u, 0xFFFFFFF0u},   /* wrap-around */
    };
    uint8_t d[16] = {0};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        reset_counters();
        int r = flash_rmw(bad[i].off, d, bad[i].len);
        CHECK(r == -1 && erases == 0 && programs == 0,
              "reject %x+%u -> %d (erases %u)", bad[i].off, bad[i].len, r, erases);
    }
    reset_counters();
    CHECK(flash_rmw(0xA17FFu, d, 1) == 0, "last byte of the store");
    CHECK(flash_rmw(0x85FFFu, d, 1) == 0, "the byte below the update flag sector");
}

static void test_long_ir_store_range(void)
{
    /* the long IR store (irstore.h): F:0x400000..0x502000, only on a chip
     * that holds all of it */
    static const struct { uint32_t off, len; } bad[] = {
        {0x3FFFFFu, 1}, {0x3FFFFFu, 2},           /* below the store */
        {0x502000u, 1}, {0x501FFFu, 2},           /* past the store */
        {0x0A1800u, 16}, {0x100000u, 16},         /* between the two stores */
        {0x400FFFu, 2},                           /* crosses a sector */
        {0x400000u, 0},
    };
    uint8_t d[16] = {1, 2, 3, 4};
    capacity = 0x01000000u;
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        reset_counters();
        int r = flash_rmw(bad[i].off, d, bad[i].len);
        CHECK(r == -1 && erases == 0, "reject %x+%u -> %d", bad[i].off, bad[i].len, r);
    }
    reset_counters();
    CHECK(flash_rmw(0x400000u, d, sizeof d) == 0 && memcmp(flash + 0x400000u, d, sizeof d) == 0,
          "first bytes of the IR store");
    static uint8_t sec[FLASH_SECTOR];
    memset(sec, 0x3C, sizeof sec);
    CHECK(flash_rmw(0x501000u, sec, FLASH_SECTOR) == 0, "the last sector, whole");
    CHECK(pumps == busy_polls && violations == 0, "pumps %u busy %u violations %u", pumps,
          busy_polls, violations);
    /* a 4 MB chip (or an unknown one): the store is off, the data store works */
    capacity_calls = 0;
    capacity = 0x00400000u;
    reset_counters();
    CHECK(flash_rmw(0x400000u, d, sizeof d) == -1 && erases == 0, "4 MB chip: refused");
    capacity = 0;
    CHECK(flash_rmw(0x480000u, d, sizeof d) == -1 && erases == 0, "unknown chip: refused");
    CHECK(capacity_calls == 2, "capacity asked %u times", capacity_calls);
    CHECK(flash_rmw(0x71000u, d, sizeof d) == 0 && capacity_calls == 2,
          "the data store needs no capacity");
    capacity = 0x01000000u;
}

static void test_failures(void)
{
    uint8_t d[4] = {2, 4, 6, 8};   /* bit 0 clear: drop_bit shows */
    /* the erase never finishes: timeout (2 s), audio pumped all along */
    erase_polls = 0xFFFFFFFFu;
    ms_per_poll = 1;
    reset_counters();
    CHECK(flash_rmw(0x80000u, d, sizeof d) == -2, "erase timeout -> -2");
    CHECK(pumps >= 1999 && pumps == busy_polls, "pumps %u busy polls %u", pumps, busy_polls);
    busy = 0;
    erase_polls = 40;
    ms_per_poll = 0;
    /* status read fails */
    fail_status = 1;
    CHECK(flash_rmw(0x80000u, d, sizeof d) == -2, "status failure -> -2");
    fail_status = 0;
    busy = 0;
    /* the read-back differs */
    drop_bit = 1;
    CHECK(flash_rmw(0x80000u, d, sizeof d) == -4, "read-back mismatch -> -4");
    drop_bit = 0;
}

int main(void)
{
    test_write();
    test_long_erase();
    test_rejects();
    test_long_ir_store_range();
    test_failures();
    printf(fails ? "flash_rmw host tests FAILED\n" : "flash_rmw host tests OK\n");
    return fails ? 1 : 0;
}
