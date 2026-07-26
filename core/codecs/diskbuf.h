/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/codecs/diskbuf.h — a large watermarked "anti-skip" buffer over a
 * decoder_source_t.
 *
 * Why this exists: the decoder streams compressed audio (~110 KB/s for FLAC)
 * and drains it in tiny reads. With only the small read-ahead shim
 * (codecs/readahead.h) between it and the disk, every ~32 KB consumed triggers
 * a fresh ~32 KB PIO read — so the drive head reads almost CONTINUOUSLY, a few
 * times a second, for the whole track. On real hardware that is audible: the
 * arm is in constant motion and never parks. Apple's firmware (and Rockbox)
 * avoid this by reading MEGABYTES ahead in a burst, then letting the drive sit
 * idle for tens of seconds before the next burst.
 *
 * This layer is that anti-skip buffer. It holds a multi-MB sliding window of
 * the compressed file in RAM and:
 *
 *   - SATISFIES the decoder's reads from RAM (no disk hit), through the SAME
 *     decoder_source_t interface it wraps — it drops in transparently under the
 *     read-ahead shim:
 *
 *         diskbuf_init(&db, &file_src, big_buf, sizeof big_buf, LOW, HIGH);
 *         decoder_source_t disk_src;
 *         diskbuf_as_source(&db, &disk_src);
 *         readahead_init(&ra, &disk_src, ra_buf, sizeof ra_buf);   // ra over db
 *
 *   - REFILLS itself only from a separate "pump" the caller drives from its
 *     main loop (diskbuf_pump), in BOUNDED chunks interleaved with decode/DMA
 *     feeding — so a refill never blocks long enough to starve the PCM ring.
 *
 *   - Uses HIGH/LOW watermark HYSTERESIS to make the I/O BURSTY: it fills up to
 *     `high` bytes-ahead, then goes idle (the pump does nothing, the drive head
 *     parks) until the decoder has drained it below `low`, then bursts back up
 *     to `high`. That idle gap between bursts is the whole point.
 *
 * Watermarks are measured in bytes buffered AHEAD of the decoder's cursor
 * (wr - rd), i.e. how much compressed audio is queued. Pick `low` to comfortably
 * cover a whole refill burst's worth of drain (several seconds) and `high` a
 * little under the buffer capacity.
 *
 * Seeks are handled like the read-ahead shim's: a seek inside the buffered
 * window just moves the cursor; one outside resets the window and refills at the
 * new position. Codecs only seek at open() (size the file / skip metadata), so
 * a reset is rare and always pre-audio. SEEK_END is delegated to the backing
 * source, which knows the length.
 *
 * Freestanding-clean: no libc beyond memcpy (routed through lib/mem.h on the
 * bare-metal build), no allocation — the caller owns the buffer. Host-testable
 * with an in-memory decoder_source_t (see tests/codecs/diskbuf_test.c).
 */
#ifndef CORE_CODECS_DISKBUF_H
#define CORE_CODECS_DISKBUF_H

#include <stddef.h>
#include <stdint.h>
#include "decoder.h"

typedef struct {
    decoder_source_t *inner;    /* backing byte source (owned by caller)         */
    uint8_t          *buf;      /* caller-provided ring buffer                    */
    uint32_t          cap;      /* ring capacity in bytes                         */
    /* Absolute file offsets. Bytes [win_lo, wr) live in the ring at
     * buf[offset % cap]; the decoder reads next at rd, with win_lo<=rd<=wr and
     * wr-win_lo <= cap. */
    int64_t           win_lo;   /* oldest buffered byte                           */
    int64_t           wr;       /* one past newest buffered byte (fill head)      */
    int64_t           rd;       /* decoder cursor (what tell() reports)           */
    int64_t           inner_pos;/* where `inner` is physically positioned, or -1  */
    uint32_t          low;      /* refill starts when (wr-rd) drops below this    */
    uint32_t          high;     /* refill stops when (wr-rd) reaches this         */
    int               filling;  /* hysteresis state: 1 = bursting, 0 = drive idle */
    int               eos;      /* backing source reached end of file             */
    /* Backing-error plumbing. A backing read that returns 0 bytes is
     * ambiguous — end of file, or an unreadable sector. Collapsing both to
     * "end of stream" made one bad sector three minutes into a track look
     * exactly like the track ending, so the player advanced and the user saw
     * a short song. `err_fn` (optional) reports the backing source's sticky
     * error flag; when it fires we retry the burst for a few pump passes (a
     * spun-down drive's first read legitimately fails) and only then latch
     * `err`, which the player reads instead of silently advancing. */
    int             (*err_fn)(void *ud);
    void             *err_ud;
    int               err;      /* latched: the backing source can't be read     */
    int               err_streak; /* consecutive failed fetches, reset on success */
} diskbuf_t;

/*
 * Initialise an anti-skip buffer over `inner`, using `buf` (`cap` bytes) as the
 * ring. `low`/`high` are the watermarks in bytes-ahead (see header); require
 * 0 < low < high <= cap. The logical cursor starts at 0 and the buffer starts
 * in the "filling" state so the first pump begins pre-loading immediately.
 * `buf` must stay valid for the life of the buffer.
 */
void diskbuf_init(diskbuf_t *db, decoder_source_t *inner,
                  uint8_t *buf, uint32_t cap, uint32_t low, uint32_t high);

/*
 * Fill `*out` with a decoder_source_t whose read/seek/tell route through `db`.
 * Hand `out` (not the backing source) to the next layer (the read-ahead shim or
 * the codec directly).
 */
void diskbuf_as_source(diskbuf_t *db, decoder_source_t *out);

/*
 * Advance the refill state machine by AT MOST one `chunk`-byte backing read.
 * Call it once per main-loop pass (ideally only when the PCM ring has healthy
 * headroom, so the bounded blocking read can't starve audio). Does nothing when
 * the buffer is above `high` (drive idle) or at end of file. `chunk` bounds the
 * per-call blocking read; keep it small (e.g. 32-64 KB) so the loop stays
 * responsive. Returns the bytes actually read this call (0 = idle or EOF).
 */
uint32_t diskbuf_pump(diskbuf_t *db, uint32_t chunk);

/*
 * Register the backing source's sticky-read-error probe. `fn(ud)` returns
 * nonzero once the source has failed a read (as opposed to reaching EOF).
 * Optional — without it a 0-byte backing read is taken as end of stream, the
 * historical behaviour. Call after diskbuf_init.
 */
void diskbuf_set_error_hook(diskbuf_t *db, int (*fn)(void *ud), void *ud);

/* Nonzero once the backing source has failed unrecoverably (see err_fn). The
 * player uses this to tell "disk read error" from "end of track". */
int diskbuf_error(const diskbuf_t *db);

/* Bytes currently buffered ahead of the decoder (wr - rd). */
uint32_t diskbuf_fill_ahead(const diskbuf_t *db);

#endif /* CORE_CODECS_DISKBUF_H */
