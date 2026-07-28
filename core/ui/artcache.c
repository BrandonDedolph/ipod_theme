/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/artcache.c — incremental album-art thumbnail cache.
 *
 * Two tiers (see artcache.h for the why): a 12-byte DIRECTORY entry per album
 * that never moves, and ARTCACHE_WAYS decoded-pixel entries that come and go.
 * A way is EMPTY (key < 0) → QUEUED → LOADED; a load that fails frees the way
 * and records the album in a "no art" bitmap so it is never retried.
 * artcache_pump() advances exactly ONE QUEUED way per call, so the disk reads
 * a cover load needs are amortised over many main-loop passes and never stall
 * the audio decode/DMA loop the way a synchronous load during list scroll did.
 *
 * Eviction is least-recently-drawn. artcache_get() is called for every visible
 * row on every frame, so stamping a way on each call makes the newest stamps
 * exactly the on-screen window — a visible row can never be the victim, which
 * a "distance from a focus index" rule cannot promise once the window is
 * bigger than the gaps between resident albums. Same stamp orders pump: the
 * row you are looking at resolves before the row you scrolled past.
 *
 * The load path reads a pre-indexed CoreArt sidecar into a bounded module-
 * static scratch buffer, validates the "CART" magic + dimensions, then hands
 * the pixels to thumb.c to shrink into the way's ARTCACHE_DIM box. folder.thm
 * (baked at exactly 28x28) is preferred over folder.art (up to 120x120) when
 * both exist — either works, but the smaller source is both a cheaper read and
 * a 1:1 copy instead of a resample.
 *
 * Freestanding: no libc/libm/malloc, no allocation, integer only.
 */

#include "artcache.h"
#include "thumb.h"

enum {
    WAY_EMPTY = 0,
    WAY_QUEUED,
    WAY_LOADED,
};

/* Where one album's cover lives on disk. Sizes are u16 because the largest
 * sidecar we will read is ARTCACHE_SCRATCH_SZ (28,812) — queue() rejects
 * anything bigger, so nothing is silently truncated. */
typedef struct {
    uint32_t thm_clus, art_clus;
    uint16_t thm_size, art_size;
} album_t;

typedef struct {
    uint32_t stamp;                            /* last artcache_get, 0 if never */
    int16_t  key;                              /* album index, or -1 if unused  */
    uint8_t  state;
    uint16_t px[ARTCACHE_DIM * ARTCACHE_DIM];  /* 28x28 RGB565 (valid iff LOADED) */
} way_t;

/* ~28.3 KB resident, versus ~198.5 KB for the old album-indexed pixel array:
 *   directory  256 * 12                    =  3,072 B
 *   no-art bitmap                          =     32 B
 *   ways       16 * (28*28*2 + 8)          = 25,216 B
 * Scratch adds ARTCACHE_SCRATCH_SZ (28,812 B) on top, shareable via
 * artcache_scratch() so the rest of the UI need not carry a second copy. */
static album_t  g_album[ARTCACHE_SLOTS];
static uint8_t  g_noart[(ARTCACHE_SLOTS + 7) / 8];
/* One bit per album: "a disk read for this cover already failed once". Lets a
 * transient read error be retried exactly once before we give up on it, without
 * letting a permanently unreadable file re-issue a read on every pump. */
static uint8_t  g_retried[(ARTCACHE_SLOTS + 7) / 8];
static way_t    g_way[ARTCACHE_WAYS];
static uint8_t  g_scratch[ARTCACHE_SCRATCH_SZ];

/* Monotonic "draw order" counter. At ~6 stamps per frame and 30 fps a u32 lasts
 * ~270 days of continuous scrolling; a wrap costs at worst one bad eviction. */
static uint32_t g_clock;

static int retried(int idx)
{
    return (g_retried[idx >> 3] >> (idx & 7)) & 1;
}

static void set_retried(int idx, int on)
{
    uint8_t bit = (uint8_t)(1u << (idx & 7));
    if (on) {
        g_retried[idx >> 3] |= bit;
    } else {
        g_retried[idx >> 3] = (uint8_t)(g_retried[idx >> 3] & ~bit);
    }
}

static int noart(int idx)
{
    return (g_noart[idx >> 3] >> (idx & 7)) & 1;
}

static void set_noart(int idx, int on)
{
    uint8_t bit = (uint8_t)(1u << (idx & 7));
    if (on) {
        g_noart[idx >> 3] |= bit;
    } else {
        g_noart[idx >> 3] = (uint8_t)(g_noart[idx >> 3] & ~bit);
    }
}

/* Find the way holding `idx`, or -1. Linear over 16 — a handful of calls per
 * frame, so a scan beats carrying a second index. */
static int find(int idx)
{
    for (int i = 0; i < ARTCACHE_WAYS; i++) {
        if (g_way[i].key == (int16_t)idx) {
            return i;
        }
    }
    return -1;
}

void artcache_reset(void)
{
    for (int i = 0; i < ARTCACHE_SLOTS; i++) {
        g_album[i].thm_clus = 0;
        g_album[i].art_clus = 0;
        g_album[i].thm_size = 0;
        g_album[i].art_size = 0;
    }
    for (unsigned i = 0; i < sizeof g_noart; i++) {
        g_noart[i] = 0;
    }
    for (unsigned i = 0; i < sizeof g_retried; i++) {
        g_retried[i] = 0;
    }
    for (int i = 0; i < ARTCACHE_WAYS; i++) {
        g_way[i].key   = -1;
        g_way[i].state = WAY_EMPTY;
        g_way[i].stamp = 0;
    }
    g_clock = 0;
}

void artcache_init(void)
{
    artcache_reset();
}

uint8_t *artcache_scratch(void)
{
    return g_scratch;
}

void artcache_queue(int idx, uint32_t thm_clus, uint32_t thm_size,
                    uint32_t art_clus, uint32_t art_size)
{
    if (idx < 0 || idx >= ARTCACHE_SLOTS) {
        return;
    }
    /* A sidecar too big for the staging buffer could never be read, so record
     * it as absent rather than storing a size that would not fit a u16. */
    if (thm_size < ARTCACHE_HDR_LEN || thm_size > ARTCACHE_SCRATCH_SZ) {
        thm_clus = 0;
        thm_size = 0;
    }
    if (art_size < ARTCACHE_HDR_LEN || art_size > ARTCACHE_SCRATCH_SZ) {
        art_clus = 0;
        art_size = 0;
    }

    album_t *al = &g_album[idx];
    if (al->thm_clus == thm_clus && al->art_clus == art_clus &&
        al->thm_size == (uint16_t)thm_size && al->art_size == (uint16_t)art_size) {
        return;                                /* unchanged: a re-queue is free */
    }

    al->thm_clus = thm_clus;
    al->thm_size = (uint16_t)thm_size;
    al->art_clus = art_clus;
    al->art_size = (uint16_t)art_size;

    /* Different art for this album: forget the old verdict and the old pixels
     * so the next artcache_get re-arms it. */
    set_noart(idx, 0);
    set_retried(idx, 0);
    int w = find(idx);
    if (w >= 0) {
        g_way[w].key   = -1;
        g_way[w].state = WAY_EMPTY;
        g_way[w].stamp = 0;
    }
}

int artcache_state(int idx)
{
    if (idx < 0 || idx >= ARTCACHE_SLOTS) {
        return ARTCACHE_ST_NOCLUS;
    }
    for (int i = 0; i < ARTCACHE_WAYS; i++) {
        if (g_way[i].key == (int16_t)idx && g_way[i].state == WAY_LOADED) {
            return ARTCACHE_ST_LOADED;
        }
    }
    if (noart(idx)) {
        return ARTCACHE_ST_NOART;
    }
    if (g_album[idx].thm_clus == 0 && g_album[idx].art_clus == 0) {
        return ARTCACHE_ST_NOCLUS;
    }
    return ARTCACHE_ST_PENDING;
}

const uint16_t *artcache_peek(int idx)
{
    if (idx < 0 || idx >= ARTCACHE_SLOTS) {
        return 0;
    }
    int w = find(idx);
    return (w >= 0 && g_way[w].state == WAY_LOADED) ? g_way[w].px : 0;
}

const uint16_t *artcache_get(int idx)
{
    if (idx < 0 || idx >= ARTCACHE_SLOTS) {
        return 0;
    }

    int w = find(idx);
    if (w >= 0) {
        g_way[w].stamp = ++g_clock;            /* still on screen */
        return g_way[w].state == WAY_LOADED ? g_way[w].px : 0;
    }

    /* Not resident. Nothing to claim a way for if we already know this album
     * has no usable art, or if no sidecar was ever registered for it. */
    if (noart(idx) || (g_album[idx].thm_clus == 0 && g_album[idx].art_clus == 0)) {
        return 0;
    }

    /* Claim a way: a free one, else the least recently drawn. Because every
     * visible row stamps its way on this same call each frame, the victim is
     * always something off screen. */
    int victim = 0;
    for (int i = 0; i < ARTCACHE_WAYS; i++) {
        if (g_way[i].key < 0) {
            victim = i;
            break;
        }
        if (g_way[i].stamp < g_way[victim].stamp) {
            victim = i;
        }
    }
    g_way[victim].key   = (int16_t)idx;
    g_way[victim].state = WAY_QUEUED;
    g_way[victim].stamp = ++g_clock;
    return 0;                                  /* pump will fill it */
}

/*
 * Read + validate a CoreArt sidecar (clus/size) into g_scratch and shrink it
 * into `dst` (ARTCACHE_DIM square).
 *
 * Returns 1 on success, 0 when this album DEFINITIVELY has no usable art here
 * (nothing registered, or the file is not a valid sidecar), and -1 when the
 * DISK READ ITSELF failed. The caller must not confuse the last two: a read
 * error is transient and deserves a retry, whereas latching it as "no art"
 * blanks the album for the rest of the session.
 */
static int load_one(fat32_t *fs, uint32_t clus, uint32_t size, uint16_t *dst)
{
    if (clus == 0 || size < ARTCACHE_HDR_LEN || size > sizeof g_scratch) {
        return 0;                              /* nothing registered */
    }
    int32_t n = fat32_read_file(fs, clus, g_scratch, size);
    if (n < 0) {
        return -1;                             /* disk read failed — retryable */
    }
    if (n < (int32_t)ARTCACHE_HDR_LEN) {
        return 0;
    }
    if (g_scratch[0] != 'C' || g_scratch[1] != 'A' ||
        g_scratch[2] != 'R' || g_scratch[3] != 'T') {
        return 0;
    }
    int w = g_scratch[6] | (g_scratch[7] << 8);
    int h = g_scratch[8] | (g_scratch[9] << 8);
    if (w <= 0 || h <= 0 || w > ARTCACHE_MAX_DIM || h > ARTCACHE_MAX_DIM) {
        return 0;
    }
    if ((int32_t)(ARTCACHE_HDR_LEN + w * h * 2) > n) {
        return 0;
    }
    /* Box-average: a chip is a big shrink (120 -> 28) and nearest-neighbour
     * there shimmers as the list scrolls. This runs once per album, off the
     * audio path, so the averaging is free at this cadence. A 28x28 folder.thm
     * hits thumb_box_rgb565's 1:1 fast path and is copied verbatim. */
    thumb_box_rgb565((const uint16_t *)(g_scratch + ARTCACHE_HDR_LEN), w, h,
                     dst, ARTCACHE_DIM, ARTCACHE_DIM);
    return 1;
}

int artcache_pump(fat32_t *fs)
{
    /*
     * OLDEST outstanding request first.
     *
     * This used to take the most recently stamped way, on the theory that it is
     * the one the user is looking at. But every visible row is re-stamped on
     * every pass, in row order, so the TOP row always ended up with the lowest
     * stamp and was therefore always last in line — permanently, since the
     * ordering is re-established each pass. Any pass that ran out of budget
     * starved it, which is why the first album's cover stayed blank until you
     * scrolled it out of the window and back (re-claiming it at a fresh stamp).
     *
     * All the candidates here are on screen anyway, so FIFO is both fair and
     * starvation-free: a request that has waited longest cannot keep losing.
     */
    int pick = -1;
    for (int i = 0; i < ARTCACHE_WAYS; i++) {
        if (g_way[i].state != WAY_QUEUED) {
            continue;
        }
        if (pick < 0 || g_way[i].stamp < g_way[pick].stamp) {
            pick = i;
        }
    }
    if (pick < 0 || fs == 0) {
        return 0;
    }

    way_t *s = &g_way[pick];
    const album_t *al = &g_album[s->key];
    /* Clusters were pre-resolved at library-load, so this is a direct file read
     * — no directory scan. Prefer folder.thm: it is pre-baked at exactly
     * ARTCACHE_DIM (28x28) by tools/coreart.py, so the load is a ~1.5KB read +
     * a 1:1 copy — no big read, no resample. folder.art (120x120) is only the
     * fallback for an album that somehow lacks a thm. */
    int r = load_one(fs, al->thm_clus, al->thm_size, s->px);
    if (r == 0) {
        r = load_one(fs, al->art_clus, al->art_size, s->px);
    }

    if (r == 1) {
        s->state = WAY_LOADED;
        return 1;
    }

    int key = s->key;
    s->key   = -1;
    s->state = WAY_EMPTY;
    s->stamp = 0;

    if (r < 0 && !retried(key)) {
        /*
         * The READ failed rather than the art being absent. Hand the way back
         * WITHOUT latching a verdict, so the next paint re-queues it and we
         * try once more. This matters most for the first album in the list:
         * its cover is the first read issued after the library load, i.e. the
         * most likely one to hit a drive still spinning up — and latching that
         * left exactly one album permanently blank while every other cover
         * appeared. Bounded to a single retry so a genuinely unreadable file
         * cannot re-issue a disk read on every pump forever.
         */
        set_retried(key, 1);
        return 1;
    }

    /* No usable art, or a read that failed twice: remember it in one bit so a
     * cover-less album costs no pixels and is not retried. */
    set_noart(key, 1);
    return 1;
}
