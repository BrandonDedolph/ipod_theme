/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/artcache.h — incremental album-art thumbnail cache.
 *
 * The album LIST wants a small cover chip next to every row, but loading a
 * cover is disk I/O and doing it synchronously during a scroll starved the
 * audio DMA (audible stutter). This cache decouples the two: the main loop
 * calls artcache_pump() once per pass, which loads AT MOST ONE thumbnail per
 * call, spreading the I/O across many passes so the decode/DMA loop is never
 * blocked.
 *
 * Storage splits in two, because the two halves have wildly different costs:
 *
 *   - A DIRECTORY entry per album (12 bytes: where its CoreArt sidecars live).
 *     Registered once by artcache_queue at library-load time, kept for the
 *     whole library — 3 KB for 256 albums.
 *   - ARTCACHE_WAYS PIXEL entries (~1.5 KB each), an associative set keyed by
 *     album index. Only ~6 chips are ever on screen, so holding decoded pixels
 *     for every album was ~200 KB of permanently resident RAM to serve six
 *     tiles — and, being a direct-mapped array indexed by album row, it left
 *     every album past the array end with no cover at all, ever.
 *
 * artcache_get is what drives residency: it is called for every visible row
 * on every frame, so the set of albums it touches IS the on-screen window.
 * A miss claims a way (evicting the least-recently-drawn album) and marks it
 * for loading; pump then fills the most recently drawn one first. Callers do
 * not have to re-queue on scroll — registering an album once is enough.
 *
 * Each way caches one album's cover pre-shrunk to ARTCACHE_DIM x ARTCACHE_DIM
 * RGB565. On pump, the pre-indexed "folder.thm" (preferred: baked at exactly
 * 28x28 = ARTCACHE_DIM by tools/coreart.py, so a 1:1 copy) or "folder.art"
 * (fallback, up to 120x120, box-averaged down) CoreArt sidecar is read into a
 * module-static scratch buffer, its "CART" header + dims validated, and the
 * pixels shrunk into the way. An album that can't be loaded (no art / bad
 * header / read error) is remembered as failed in a 32-byte bitmap — it costs
 * no way and is never retried.
 *
 * Freestanding: no libc/libm/malloc, no allocation, integer only. All storage
 * (directory + ways + scratch) is module-static and bounded.
 */
#ifndef CORE_UI_ARTCACHE_H
#define CORE_UI_ARTCACHE_H

#include <stdint.h>
#include "../fs/fat32.h"

#define ARTCACHE_DIM   28          /* list-chip cover is 28x28 RGB565 (taller
                                    * two-line rows give room for a bigger chip) */
#define ARTCACHE_SLOTS 1024         /* album indices the cache accepts, i.e. the
                                    * whole library (LIB_MAX_ALBUMS). This is a
                                    * KEY RANGE + a 12-byte directory entry —
                                    * NOT a decoded thumbnail each. */
#define ARTCACHE_WAYS  16          /* resident decoded covers: ~2.5 screens of
                                    * chips at 6 rows, ~25 KB */

/* CoreArt sidecar layout (see tools/coreart.py): "CART"(4) + u16 version +
 * u16 width + u16 height + u16 reserved, then width*height RGB565 pixels.
 * The scratch buffer is sized for the largest cover we accept (120x120). */
#define ARTCACHE_HDR_LEN     12
#define ARTCACHE_MAX_DIM     120
#define ARTCACHE_SCRATCH_SZ  (ARTCACHE_HDR_LEN + \
                              ARTCACHE_MAX_DIM * ARTCACHE_MAX_DIM * 2)

/* Drop the whole directory, every cached cover, and all pending work.
 * artcache_init is the name the boot path uses; both do the same thing (the
 * storage is static, so "init" is only ever a clear). */
void artcache_init(void);
void artcache_reset(void);

/*
 * Register where album `idx`'s cover lives: the CoreArt sidecar clusters/sizes
 * resolved at library-load time (folder.thm preferred, folder.art the fallback;
 * either cluster may be 0 for "absent", and a size larger than
 * ARTCACHE_SCRATCH_SZ is treated as absent). Indices outside
 * [0, ARTCACHE_SLOTS) are ignored.
 *
 * Idempotent and cheap: re-registering the same sidecars is a no-op, so a
 * caller may loop over every row of the view on every frame without ever
 * causing a re-decode. Registering DIFFERENT sidecars for an album drops its
 * cached pixels and any prior "no art" verdict, so it loads afresh.
 *
 * This only records the directory entry — it never decodes and never evicts.
 * Which albums actually hold pixels is decided by artcache_get.
 */
void artcache_queue(int idx, uint32_t thm_clus, uint32_t thm_size,
                    uint32_t art_clus, uint32_t art_size);

/*
 * Do at most one unit of loading work: take the album most recently asked for
 * by artcache_get that is still waiting, load + downscale its cover, and
 * return 1. If nothing is waiting, do nothing and return 0. Call once per
 * main-loop pass.
 */
int artcache_pump(fat32_t *fs);

/*
 * Return album `idx`'s ARTCACHE_DIM x ARTCACHE_DIM RGB565 pixels if they are
 * loaded, else NULL (not yet loaded / no art / out of range).
 *
 * This is also how the cache learns what is on screen — a call marks the album
 * as wanted, claiming a way for it if it has none. So call it for EVERY visible
 * row on every frame, hit or miss; a row you stop drawing is a row whose cover
 * becomes evictable.
 */
const uint16_t *artcache_get(int idx);

/*
 * Why `idx` has no pixels — a DIAGNOSTIC, so a blank chip on the device can be
 * told apart from the outside without a serial cable. Reading this has no side
 * effects (unlike artcache_get, which claims a way).
 */
enum {
    ARTCACHE_ST_LOADED   = 0,   /* pixels are resident                        */
    ARTCACHE_ST_NOCLUS   = 1,   /* no sidecar was ever registered for it       */
    ARTCACHE_ST_NOART    = 2,   /* tried and gave up: unreadable / not a CART  */
    ARTCACHE_ST_PENDING  = 3,   /* queued or not yet claimed — still coming    */
};
int artcache_state(int idx);

/*
 * The cache's sidecar staging buffer: ARTCACHE_SCRATCH_SZ bytes, valid for the
 * whole run. It is only live INSIDE artcache_pump(), so any other main-loop
 * code that needs a scratch area for the same job (reading a full-size
 * folder.art, say) can borrow this one instead of carrying a second ~28 KB
 * copy — as long as it is not itself called from a pump.
 */
uint8_t *artcache_scratch(void);

#endif /* CORE_UI_ARTCACHE_H */
