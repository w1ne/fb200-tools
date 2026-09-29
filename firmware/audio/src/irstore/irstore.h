#ifndef FB200_IRSTORE_H
#define FB200_IRSTORE_H
/* Long IR store (docs/PARITY.md M5, docs/UI_AND_STORAGE.md §5): 64 slots of
 * up to 4096 taps (float32, 44.1 kHz) in their own flash area, selected as
 * cab types 20..83. The 9 stock user IR slots (cab 11..19, F:0x87000..) are
 * not touched.
 *
 * Flash (offsets from 0x60000000), IRSTORE_BASE..IRSTORE_END:
 *   IRSTORE_INDEX(0), IRSTORE_INDEX(1)  two copies of the slot table, one
 *                                       4 kB sector each; the valid copy
 *                                       with the higher seq is current
 *   IRSTORE_DATA(s)                     slot s (0..63): 16 kB of taps
 * A table write goes to the other copy (seq + 1), so a power loss during
 * the write keeps the old table. A slot's data is written before its table
 * entry; its CRC in the entry catches a partial write.
 *
 * Table (little endian, IRSTORE_TABLE bytes):
 *   +0  magic "FBIR"  +4 version u16  +6 slots u16  +8 seq u32
 *   +12 entry size u16  +14 0 u16
 *   +16 IRSTORE_SLOTS entries of IRSTORE_ENTRY bytes:
 *       +0 taps u16 (0 or 0xFFFF: empty)  +2 flags u16 (0)
 *       +4 source rate u32 (Hz, information)  +8 gain f32  +12 crc32 u32
 *       of the taps * 4 data bytes  +16 name[24] (NUL-padded)  +40 8 x 0
 *   +16 + 64 * 48  crc32 u32 of all bytes before it
 *
 * Gain: the stock rule for user IRs (cab_user_ir_gain on the first 512
 * taps), computed by the firmware when the upload completes and stored.
 * The same IR plays at the same level from a stock slot and a long slot.
 *
 * The store needs flash up to IRSTORE_END: irstore_ready() checks the
 * chip's capacity (JEDEC ID and the FlexSPI window, flash_capacity()) and
 * every call refuses the store on a smaller chip.
 *
 * Pure logic on top of the hooks below (irstore_port.c on the target; the
 * host test, tests/irstore_host_test.c, with a fake flash). */
#include <stdint.h>

#define IRSTORE_BASE      0x00400000u   /* flash offset: 4 MB, above the model library */
#define IRSTORE_SLOTS     64u
#define IRSTORE_FIRST     20u           /* cab type of slot 0 */
#define IRSTORE_LAST      (IRSTORE_FIRST + IRSTORE_SLOTS - 1u)   /* 83 */
#define IRSTORE_TAPS      4096u
#define IRSTORE_SECTOR    0x1000u
#define IRSTORE_SLOT_SIZE (IRSTORE_TAPS * 4u)
#define IRSTORE_INDEX(c)  (IRSTORE_BASE + (uint32_t)(c) * IRSTORE_SECTOR)
#define IRSTORE_DATA(s)   (IRSTORE_BASE + 2u * IRSTORE_SECTOR + (uint32_t)(s) * IRSTORE_SLOT_SIZE)
#define IRSTORE_END       IRSTORE_DATA(IRSTORE_SLOTS)   /* 0x502000 */
#define IRSTORE_NAME      24u           /* bytes, NUL-padded; up to 23 characters */
#define IRSTORE_MAGIC     0x52494246u   /* "FBIR" */
#define IRSTORE_VERSION   1u
#define IRSTORE_HDR       16u
#define IRSTORE_ENTRY     48u
#define IRSTORE_TABLE     (IRSTORE_HDR + IRSTORE_SLOTS * IRSTORE_ENTRY + 4u)   /* 3092 */
#define IRSTORE_IDLE_MS   3000u         /* upload: no byte for this long aborts */

typedef struct {
    uint16_t taps;                /* 1..IRSTORE_TAPS; 0 = empty */
    uint16_t flags;
    uint32_t rate;                /* source rate, Hz */
    float gain;
    uint32_t crc;                 /* crc32 of the taps * 4 data bytes */
    char name[IRSTORE_NAME];      /* NUL-terminated */
} irstore_entry_t;

/* ---- table codec (pure) ---- */
void irstore_encode_entry(uint8_t out[IRSTORE_ENTRY], const irstore_entry_t *e);
/* 1 used, 0 empty or malformed (taps out of range, no NUL in the name) */
int irstore_decode_entry(const uint8_t in[IRSTORE_ENTRY], irstore_entry_t *e);
/* A new empty table with this seq (IRSTORE_TABLE bytes). */
void irstore_table_init(uint8_t *t, uint32_t seq);
void irstore_table_seal(uint8_t *t);                /* write the trailing CRC */
int irstore_table_valid(const uint8_t *t, uint32_t *seq);   /* 1 valid */
/* Slot number of a cab type (20..83 -> 0..63), -1 otherwise. */
int irstore_slot_of(unsigned cab_type);

/* ---- store (through the hooks) ---- */
int irstore_ready(void);                            /* 1: the chip holds the store */
/* The current table copy (0/1) and its seq; -1: none valid (empty store). */
int irstore_current(uint32_t *seq);
/* 1 used, 0 empty, -1 store not available. */
int irstore_get(unsigned slot, irstore_entry_t *e);
/* Check the slot's data CRC in flash, then copy up to max taps into dst
 * (NaN/inf -> 0). Returns the taps copied (min(taps, max)), 0 when the
 * slot is empty or its data is bad, -1 when the store is not available. */
int irstore_load(unsigned slot, float *dst, unsigned max, irstore_entry_t *e);
/* Write slot's entry (e NULL: delete it) into the other table copy, built
 * in scratch (IRSTORE_TABLE bytes). 0 on success, <0 on failure. */
int irstore_write_entry(unsigned slot, const irstore_entry_t *e, uint8_t *scratch);
/* Delete a slot (console irdel): 1 deleted, 0 was empty, <0 failure
 * (-1 store not available, -2 buffer busy, -3 flash write failed). */
int irstore_delete(unsigned slot);

/* ---- upload session (console irput; the fwbegin pattern) ----
 * irstore_put_begin checks the arguments, borrows the IR buffer and prints
 * "ir ready"; the host then streams taps * 4 raw bytes (float32 LE). The
 * console calls irstore_put_task instead of its line reader while
 * irstore_put_active. On the last byte the CRC is checked in RAM, then one
 * 4 kB flash sector is written per call (the main loop runs between them),
 * the data is checked again in flash, and the table entry goes last.
 * Result line: "ir done crc=<crc> ok slot <n> taps <n> gain <g>", or
 * "ir done crc=<crc> BAD (nothing written)", "ir FAILED ...", "ir aborted ...".
 * Returns 0 on "ir ready", <0 on a refused request (a line says why). */
int irstore_put_begin(unsigned cab_type, unsigned taps, uint32_t crc, const char *name,
                      uint32_t rate);
int irstore_put_active(void);
void irstore_put_task(void);
/* Console output. irls: one line per used slot, "ir <cab> taps <n> rate
 * <hz> gain <g> crc <crc> ok|BAD name <name>" (ok/BAD: the data CRC), then
 * "ir store: <used> of 64 slots used ..." (or "ir store: not available:
 * ..."). irdel: "ir <cab> deleted" | "ir <cab>: empty" | "ir <cab>: FAILED (...)". */
void irstore_print_list(void);
void irstore_print_delete(unsigned cab_type);
/* A name the store accepts: 1..23 printable ASCII characters, no space. */
int irstore_name_ok(const char *name);

/* ---- hooks (irstore_port.c; the host test fakes them) ---- */
const uint8_t *irstore_map(uint32_t off);           /* memory-mapped flash read */
/* Write inside one 4 kB sector (read-modify-write, flash_store). 0 on success. */
int irstore_flash_write(uint32_t off, const void *data, uint32_t len);
uint32_t irstore_capacity(void);                    /* usable flash bytes, 0 unknown */
uint32_t irstore_rx(uint8_t *dst, uint32_t max);    /* raw bytes from the console */
uint32_t irstore_now_ms(void);
float irstore_gain(const float *ir);                /* the stock rule, 512 taps */
void irstore_pump(void);                            /* audio work between long steps */
/* The engine's IR staging buffer (IRSTORE_TAPS floats), NULL when in use;
 * release makes the engine reload the preset's cab. */
float *irstore_buf_get(void);
void irstore_buf_put(void);
#endif
