/* The looper's flash side: see loopstore.h. Main loop code, cold (the
 * Makefile's COLD_SRC: XIP): it runs only while the flash is idle or its
 * erase is suspended (lsio.h). */
#include <string.h>
#include "loopstore/loopstore.h"
#include "loopstore/lsio.h"

static inline int bit(const uint8_t *b, unsigned i) { return (b[i >> 3] >> (i & 7u)) & 1; }

static inline void put(uint8_t *b, unsigned i, int v)
{
    if (v) b[i >> 3] |= (uint8_t)(1u << (i & 7u));
    else b[i >> 3] &= (uint8_t)~(1u << (i & 7u));
}

static inline uint32_t slot_off(const loopstore_t *ls, unsigned s)
{
    return ls->base + (uint32_t)s * LS_CHUNK;
}

static void publish_pool(loopstore_t *ls) { ls->io->pool = ls->pool; }

/* not in a map, not erased, not the writer's, not being erased */
static int garbage(const loopstore_t *ls, unsigned s)
{
    if (bit(ls->erased, s) || bit(ls->used, s)) return 0;
    if (ls->w_phase && s == ls->w_slot) return 0;
    if (ls->j_on && s == ls->j_slot) return 0;
    if (ls->er_on && s >= ls->er_slot && s < (unsigned)ls->er_slot + ls->er_n) return 0;
    return 1;
}

int ls_init(loopstore_t *ls, loopio_t *io, uint16_t *cur, uint16_t *alt, uint32_t base,
                 uint32_t end)
{
    memset(ls, 0, sizeof *ls);
    memset(io, 0, sizeof *io);
    ls->io = io;
    ls->cur = cur;
    ls->alt = alt;
    ls->base = base;
    uint32_t n = end > base ? (end - base) / LS_CHUNK : 0u;
    ls->nch = (uint16_t)(n > LS_MAX_CHUNKS ? LS_MAX_CHUNKS : n);
    for (unsigned i = 0; i < LS_MAX_CHUNKS; i++) cur[i] = alt[i] = LS_NONE;
    ls->dirty = 1;                   /* every slot: unknown content */
    if (ls->nch < 8u) {
        ls->nch = 0;
        return -1;
    }
    return 0;
}

void ls_arm(loopstore_t *ls) { ls->armed = 1; }

uint32_t ls_chunks(const loopstore_t *ls) { return ls->nch; }

/* ------------------------------------------------------------ erase ahead */

static void erase_step(loopstore_t *ls)
{
    if (!ls->er_on) {
        unsigned n = ls->nch, s = 0, found = 0;
        for (unsigned i = 0; i < n && !found; i++) {
            s = (ls->erase_at + i) % n;
            found = garbage(ls, s);
        }
        if (!found) {
            ls->dirty = 0;                  /* nothing to erase until a slot is freed */
            return;
        }
        unsigned k = 1;
        if (s % 8u == 0u && s + 8u <= n) {
            k = 8;
            for (unsigned j = 1; j < 8u && k == 8u; j++) if (!garbage(ls, s + j)) k = 1;
        }
        if (!lsio_erase_begin(slot_off(ls, s), k == 8u)) {
            ls->erase_at = (uint16_t)((s + 1u) % n);   /* a failing slot: try the next one later */
            return;
        }
        ls->er_on = 1;
        ls->er_slot = (uint16_t)s;
        ls->er_n = (uint8_t)k;
        ls->er_part = 0;
        ls->erase_at = (uint16_t)((s + k) % n);
    }
    int r = lsio_erase_run(LS_SLICE_MS);
    if (r == LSIO_DONE && ls->er_n == 1u && ls->er_part == 0u) {   /* the slot's second sector */
        ls->er_part = 1;
        r = lsio_erase_begin(slot_off(ls, ls->er_slot) + FLASH_SECTOR, 0) ?
            lsio_erase_run(LS_SLICE_MS) : LSIO_FAIL;          /* returns suspended or done */
    }
    if (r == LSIO_SUSPENDED) return;
    if (r == LSIO_DONE) {
        for (unsigned j = 0; j < ls->er_n; j++) put(ls->erased, ls->er_slot + j, 1);
        ls->pool += ls->er_n;
        publish_pool(ls);
    }
    ls->er_on = 0;
}

/* an erased slot for a new chunk version, or -1 */
static int alloc_slot(loopstore_t *ls)
{
    for (unsigned i = 0; i < ls->nch; i++) {
        unsigned s = (ls->alloc_at + i) % ls->nch;
        if (bit(ls->erased, s)) {
            put(ls->erased, s, 0);
            ls->alloc_at = (uint16_t)((s + 1u) % ls->nch);
            ls->pool--;
            publish_pool(ls);
            ls->io->alloc = ls->io->alloc + 1u;
            return (int)s;
        }
    }
    return -1;
}

/* ------------------------------------------------------------ writer */

/* bytes of logical chunk lc in the loop (a whole chunk while recording) */
static uint32_t chunk_bytes(const loopstore_t *ls, unsigned lc)
{
    uint32_t nfr = ls->io->nfr, first = (uint32_t)lc * LS_FPC;
    if (nfr == 0u || nfr >= first + LS_FPC) return LS_FPC * LC_BYTES;
    return nfr > first ? (nfr - first) * LC_BYTES : 0u;
}

static void program_page(loopstore_t *ls)
{
    uint32_t pb = (ls->w_pos - 1u) & ~(FLASH_PAGE - 1u);   /* the page just filled (or ended) */
    (void)lsio_program(slot_off(ls, ls->w_slot) + pb, ls->page);   /* errors: g_lsio */
    memset(ls->page, 0xFF, sizeof ls->page);
}

/* n bytes (from src, or from the old version when src is NULL: silence if
 * none) at w_pos, at most up to the page end */
static void w_bytes(loopstore_t *ls, const uint8_t *src, uint32_t n)
{
    uint32_t o = ls->w_pos % FLASH_PAGE;
    if (n > FLASH_PAGE - o) n = FLASH_PAGE - o;
    uint8_t *dst = (uint8_t *)ls->page + o;
    if (src) memcpy(dst, src, n);
    else if (ls->w_old == LS_NONE || !lsio_read(slot_off(ls, ls->w_old) + ls->w_pos, dst, n))
        memset(dst, 0, n);
    ls->w_pos += n;
    if (ls->w_pos % FLASH_PAGE == 0u) program_page(ls);
}

static void w_finish(loopstore_t *ls)
{
    uint32_t end = ls->w_fresh ? ls->w_pos : chunk_bytes(ls, ls->w_lc);
    ls->w_end = end > ls->w_pos ? end : ls->w_pos;
    ls->w_phase = 2;
}

/* install a complete version of chunk lc (slot s) */
static void install(loopstore_t *ls, unsigned lc, unsigned s)
{
    unsigned old = ls->cur[lc];
    ls->cur[lc] = (uint16_t)s;
    put(ls->used, s, 1);
    if (old != LS_NONE && old != s && ls->alt[lc] != old) {
        put(ls->used, old, 0);
        ls->dirty = 1;
    }
}

/* one page of the head job: the old version's first pages (or silence),
 * copied after the frames so that a dub never waits for them */
static void job_page(loopstore_t *ls)
{
    if (ls->j_pos < ls->j_head) {
        uint32_t buf[FLASH_PAGE / 4u];   /* ls->page holds the open chunk's page */
        if (ls->j_old == LS_NONE ||
            !lsio_read(slot_off(ls, ls->j_old) + ls->j_pos, buf, FLASH_PAGE))
            memset(buf, 0, sizeof buf);
        (void)lsio_program(slot_off(ls, ls->j_slot) + ls->j_pos, buf);
        ls->j_pos += FLASH_PAGE;
    }
    if (ls->j_pos >= ls->j_head) {
        install(ls, ls->j_lc, ls->j_slot);
        ls->j_on = 0;
        ls->io->w_busy = ls->w_phase != 0u;
    }
}

/* the tail is in: the last partial page; the head becomes the job (after
 * the job before it) and the writer takes the next chunk */
static void w_tail_done(loopstore_t *ls)
{
    if (ls->w_pos % FLASH_PAGE) {
        program_page(ls);
        ls->w_pos = (ls->w_pos + FLASH_PAGE - 1u) & ~(FLASH_PAGE - 1u);
        return;
    }
    if (ls->j_on) {
        job_page(ls);
        return;
    }
    ls->j_on = 1;
    ls->j_lc = ls->w_lc;
    ls->j_slot = ls->w_slot;
    ls->j_old = ls->w_old;
    ls->j_pos = 0;
    ls->j_head = ls->w_head;
    ls->w_phase = 0;
    ls->io->w_busy = ls->j_on;
    job_page(ls);   /* a head of 0 pages installs at once */
}

/* a new version of chunk lc, its first frame at byte off */
static int w_open(loopstore_t *ls, unsigned lc, uint32_t off, int fresh)
{
    if (ls->j_on && ls->j_lc == lc) return 0;   /* (a one-chunk wrap) not before its install */
    int s = alloc_slot(ls);
    if (s < 0) return 0;
    ls->w_slot = (uint16_t)s;
    ls->w_lc = (uint16_t)lc;
    ls->w_old = fresh ? LS_NONE : ls->cur[lc];
    ls->w_fresh = (uint8_t)(fresh != 0);
    ls->w_head = off & ~(FLASH_PAGE - 1u);
    ls->w_pos = ls->w_head;
    memset(ls->page, 0xFF, sizeof ls->page);
    ls->w_phase = 1;
    ls->io->w_busy = 1;
    return 1;
}

static void write_step(loopstore_t *ls)
{
    loopio_t *io = ls->io;
    uint32_t pages0 = g_lsio.pages;
    while (g_lsio.pages - pages0 < LS_PAGES_PER_TASK) {
        if (ls->w_phase == 2u) {
            if (ls->w_pos < ls->w_end) w_bytes(ls, NULL, ls->w_end - ls->w_pos);
            else w_tail_done(ls);
            continue;
        }
        uint32_t t = io->wr_tail;
        if (t == io->wr_head) {
            if (!ls->j_on) break;
            job_page(ls);                                  /* the ring is empty: the head job */
            continue;
        }
        LIO_BARRIER();
        uint32_t meta = io->wr_meta[t % LIO_RING];
        if (meta & (LIO_END | LIO_SNAP)) {
            if (ls->w_phase == 1u) {                       /* the open chunk ends first */
                w_finish(ls);
                continue;
            }
            if (meta & LIO_SNAP) {
                if (ls->j_on) {                            /* the chunk before goes in first */
                    job_page(ls);
                    continue;
                }
                ls_dub_begin(ls);                          /* the undo point, in order */
            }
            io->wr_tail = t + 1u;
            continue;
        }
        uint32_t f = meta & LIO_FRAME, lc = f / LS_FPC, off = (f % LS_FPC) * LC_BYTES;
        if (ls->w_phase == 1u && (lc != ls->w_lc || off < ls->w_pos)) {
            w_finish(ls);                                  /* it left the chunk */
            continue;
        }
        if (ls->w_phase == 0u && (lc >= ls->nch || !w_open(ls, lc, off, (meta & LIO_FRESH) != 0u))) {
            ls->drops++;                                   /* no room: the audio side stops first */
            io->wr_tail = t + 1u;
            continue;
        }
        if (ls->w_pos < off) {                             /* a gap: the old frames (or silence) */
            w_bytes(ls, NULL, off - ls->w_pos);
            continue;
        }
        const uint8_t *src = io->wr[t % LIO_RING];
        uint32_t n = 0;
        while (n < LC_BYTES) {                             /* at most one page boundary */
            uint32_t before = ls->w_pos;
            w_bytes(ls, src + n, LC_BYTES - n);
            n += ls->w_pos - before;
        }
        LIO_BARRIER();
        io->wr_tail = t + 1u;
    }
}

/* ------------------------------------------------------------ reader */

static void read_step(loopstore_t *ls)
{
    loopio_t *io = ls->io;
    for (;;) {
        uint32_t tail = io->rd_tail, head = io->rd_head;
        if ((int32_t)(head - tail) < 0) head = tail;       /* the audio side skipped ahead */
        uint32_t room = LIO_RING - (head - tail);
        if (room == 0u) break;
        uint32_t nfr = io->nfr, f = ls->rd_base_f + (head - ls->rd_base_abs);
        if (nfr) f %= nfr;
        uint32_t lc = f / LS_FPC, in = f % LS_FPC;
        if (lc >= ls->nch || ls->cur[lc] == LS_NONE) {     /* not written yet */
            io->rd_head = head;
            break;
        }
        uint32_t run = LS_FPC - in;
        if (nfr && run > nfr - f) run = nfr - f;
        if (run > room) run = room;
        if (run > LIO_RING - head % LIO_RING) run = LIO_RING - head % LIO_RING;
        if (!lsio_read(slot_off(ls, ls->cur[lc]) + in * LC_BYTES, io->rd[head % LIO_RING],
                       run * LC_BYTES)) {
            io->rd_head = head;
            break;
        }
        LIO_BARRIER();
        io->rd_head = head + run;
    }
}

void ls_task(loopstore_t *ls)
{
    if (ls->nch == 0u) return;
    read_step(ls);
    write_step(ls);
    read_step(ls);
    if (ls->armed && ls->io->wr_head - ls->io->wr_tail < LIO_RING / 2u) erase_step(ls);
}

/* ------------------------------------------------------------ control */

static void abort_writer(loopstore_t *ls)
{
    ls->w_phase = 0;          /* its slot is garbage now */
    ls->j_on = 0;
    ls->io->w_busy = 0;
    ls->dirty = 1;
    ls->io->wr_tail = ls->io->wr_head;
}

static void forget(loopstore_t *ls)
{
    abort_writer(ls);
    for (unsigned i = 0; i < LS_MAX_CHUNKS; i++) ls->cur[i] = ls->alt[i] = LS_NONE;
    memset(ls->used, 0, sizeof ls->used);
    ls->io->nfr = 0;
}

/* the read ring is filled at once (the flash is not busy while the main
 * loop runs): the next frame is there when the audio side wants it */
void ls_play_from(loopstore_t *ls, uint32_t f)
{
    ls->io->rd_head = ls->io->rd_tail;
    ls->rd_base_abs = ls->io->rd_tail;
    ls->rd_base_f = f;
    if (ls->nch) read_step(ls);
}

void ls_rec_begin(loopstore_t *ls)
{
    forget(ls);
    ls_play_from(ls, 0);
}

void ls_clear(loopstore_t *ls)
{
    forget(ls);
    ls_play_from(ls, 0);
}

void ls_dub_begin(loopstore_t *ls)
{
    for (unsigned i = 0; i < ls->nch; i++) {
        unsigned a = ls->alt[i];
        if (a != LS_NONE && a != ls->cur[i]) {             /* the old undo/redo layer */
            put(ls->used, a, 0);
            ls->dirty = 1;
        }
        ls->alt[i] = ls->cur[i];
    }
}

void ls_swap(loopstore_t *ls, uint32_t next_f)
{
    uint16_t *t = ls->cur;
    ls->cur = ls->alt;
    ls->alt = t;
    ls_play_from(ls, next_f);
}

int ls_writer_idle(const loopstore_t *ls)
{
    return ls->io->wr_head == ls->io->wr_tail && ls->w_phase == 0u && !ls->j_on;
}

void ls_info(const loopstore_t *ls, ls_info_t *out)
{
    memset(out, 0, sizeof *out);
    out->chunks = ls->nch;
    out->base = ls->base;
    out->end = ls->base + (uint32_t)ls->nch * LS_CHUNK;
    out->drops = ls->drops;
    for (unsigned s = 0; s < ls->nch; s++) {
        if (bit(ls->used, s)) out->n_used++;
        else if (bit(ls->erased, s)) out->n_erased++;
        else if (garbage(ls, s)) out->n_garbage++;
    }
}
