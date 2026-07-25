/*
 * core/player/player.c — background streaming playback engine.
 *
 * Moved verbatim out of core/kernel/main.c (see player.h). Streaming FLAC/MP3
 * decode into an SPSC PCM ring feeding the DMA-driven DAC, decoupled from the
 * UI so audio keeps running while the user navigates menus. dr_flac / dr_mp3
 * run freestanding on a static arena — no libc.
 *
 * CRITICAL device-proven invariants preserved here (regressing any = a hard
 * freeze or audio glitch on real hardware):
 *   - player_stop()/player_advance() do NOT close the decoder mid-decode.
 *   - decode_step decodes PLAY_STEP_FRAMES (menu responsiveness); decode_pump
 *     is the full-ring prime.
 *   - queue-based auto-advance; album art loaded once per queue.
 */

#include "player.h"

#include "hw/pp5022.h"
#include "hw/mmio.h"
#include "hw/audio.h"
#include "hw/ata.h"
#include "hal.h"
#include "../kernel/pcm_ring.h"
#include "../kernel/timer.h"
#include "../codecs/decoder.h"
#include "../codecs/arena.h"
#include "../codecs/readahead.h"
#include "../codecs/diskbuf.h"
#include "../codecs/flac_meta.h"
#include "../codecs/dr_flac/flac.h"
#include "../codecs/dr_mp3/mp3.h"

/*
 * Streaming playback. The drive can't sustain uncompressed PCM over PIO
 * (~172 KB/s needed, ~173 KB/s ceiling), so we stream the COMPRESSED file
 * (FLAC ~40 KB/s, MP3 less) and decode on the fly. An SPSC ring decouples the
 * producer (decode_pump, foreground) from the consumer (the DMA-completion
 * ISR). dr_flac / dr_mp3 run freestanding on a static arena — no libc.
 */
#define RING_FRAMES   (1u << 18)         /* 262144 frames = 1 MB ~ 5.94 s. Big
                                          * enough that the (blocking) HDD spin-up
                                          * on a refill after the drive has been
                                          * PARKED can't starve the DMA — decode is
                                          * frozen for the ~1-3 s spin-up, so the
                                          * ring must hold more than that. */
#define DECODE_FRAMES 4096u              /* frames per decode() call (prime)    */
#define PLAY_STEP_FRAMES 1024u           /* frames per decode step in the play   */
                                         /* loop: small so the loop returns to   */
                                         /* poll the wheel ~every 18ms (a 4096    */
                                         /* chunk blocked ~74ms, missing MENU).  */
#define ARENA_BYTES   (128u * 1024u)     /* MP3 arena high-water ~96 KB; FLAC   */
                                         /* ~40 KB. Sized for the larger.       */
#define RA_BYTES      (32u * 1024u)      /* read-ahead block buffer (see below) */

/*
 * Anti-skip disk buffer (see codecs/diskbuf.h). The drive can't idle with only
 * the 32 KB read-ahead: the decoder drains ~110 KB/s (FLAC) so a fresh ~32 KB
 * PIO read fires several times a second, the whole track — the "constant arm
 * movement" heard on hardware. This large watermarked buffer instead reads
 * MEGABYTES ahead in a burst, then lets the head sit idle for tens of seconds.
 *
 * Sizing (4 MB): at ~110 KB/s that is ~37 s of compressed audio. Filling from
 * DISK_LOW up to DISK_HIGH bursts ~3.5 MB, then the drive is idle until the
 * decoder drains back to DISK_LOW (~30 s) — so the head reads in bursts a few
 * times a MINUTE instead of a few times a SECOND. DISK_LOW (~512 KB ≈ 4.6 s)
 * is the safety margin: the decoder keeps feeding audio out of the buffer for
 * ~4.6 s while a refill burst is in flight, far longer than one bounded chunk
 * read. .bss cost is 4 MB; the low-32 MB image had <1 MB in use. */
#define DISK_BUF_BYTES (8u * 1024u * 1024u)  /* ~73 s ahead -> longer idle gaps  */
#define DISK_LOW       (1024u * 1024u)   /* refill starts below this bytes-ahead */
#define DISK_HIGH      (DISK_BUF_BYTES - 128u * 1024u)  /* ...and tops out here  */
#define DISK_CHUNK     (128u * 1024u)    /* bytes per pump: bigger => FEWER, more */
                                         /* SEQUENTIAL reads (quieter head). A    */
                                         /* 128 KB PIO read (~0.28 s) still fits   */
                                         /* well under the ring-gate headroom.    */
/* Only spend time on a (blocking) disk chunk when the PCM ring has healthy
 * headroom, so a refill burst can never drain the ring: when the ring dips
 * below half, the pump backs off and lets decode catch up first. Half the ring
 * (~0.75 s) dwarfs one DISK_CHUNK read (~0.1 s), so audio can't underrun. */
#define RING_DISK_GATE (RING_FRAMES / 2u)

static int16_t    ring_storage[RING_FRAMES * 2];
static int16_t    decode_buf[DECODE_FRAMES * 2];        /* decoder output stage */
static uint8_t    arena_buf[ARENA_BYTES] __attribute__((aligned(8)));
static uint8_t    ra_buf[RA_BYTES]       __attribute__((aligned(8)));
static uint8_t    disk_buf[DISK_BUF_BYTES] __attribute__((aligned(8)));

static pcm_ring_t g_ring;
static decoder_t  g_dec;
static int        g_eos;                 /* decoder hit end of stream           */

/*
 * fat32-backed decoder_source_t: the decoder pulls its compressed input from a
 * file through this. fat32_stream is forward-only, so a backward seek re-opens
 * from the first cluster and skips forward — fine because the codecs only seek
 * during open() (to size the file / skip metadata), never in the decode loop.
 */
typedef struct {
    fat32_t       *fs;
    uint32_t       first_clus;
    uint32_t       fsize;
    fat32_stream_t st;
    uint32_t       phys;                 /* where the fat32 stream physically is */
    uint32_t       pos;                  /* logical position; a seek moves ONLY  */
                                         /* this — the physical walk is deferred */
} fat_src_t;

static fat_src_t g_fsrc;

/* Long-lived source structs: the decoder borrows a POINTER to the source it is
 * opened on (dr_flac/dr_mp3 stash it for their whole life), so these must
 * outlive the decode loop — hence file statics, not play_file locals. The
 * source chain (each layer wraps the one before, decoder reads through the last):
 *   g_file_src  raw fat_src bytes off the disk
 *   g_dbuf      MB-scale anti-skip buffer (bursty read-ahead; g_disk_src)
 *   g_ra        32 KB read-ahead shim collapsing the codec's tiny reads (g_dec_src)
 * The anti-skip buffer sits UNDER the read-ahead shim so the shim (and its
 * proven behaviour + host test) is untouched — only its backing source changed
 * from the raw disk to the RAM buffer. */
static decoder_source_t g_file_src;      /* raw fat_src bytes                    */
static diskbuf_t        g_dbuf;          /* MB-scale anti-skip disk buffer        */
static decoder_source_t g_disk_src;      /* g_dbuf as a source (ra's backing)     */
static readahead_t      g_ra;            /* block-buffering shim                 */
static decoder_source_t g_dec_src;       /* buffered source handed to the codec  */
static decoder_arena_t  g_arena;
static int              g_drive_parked;  /* 1 while the HDD is spun down (idle burst) */

static void fat_src_open(fat_src_t *s, fat32_t *fs, uint32_t clus, uint32_t sz)
{
    s->fs = fs;
    s->first_clus = clus;
    s->fsize = sz;
    s->phys = 0;
    s->pos = 0;
    fat32_stream_open(&s->st, fs, clus, sz);
}

/* Bring the physical stream up to the logical position — only when a read
 * actually needs data there. Forward is a cheap FAT-walk skip; backward
 * re-opens from the start then skips. This is what makes dr_flac's open-time
 * seek-to-EOF (to size the file) free: it never reads at EOF, so we never
 * physically walk there. */
static void fat_src_sync(fat_src_t *s)
{
    if (s->pos < s->phys) {              /* rewind: re-open at 0                 */
        fat32_stream_open(&s->st, s->fs, s->first_clus, s->fsize);
        s->phys = 0;
    }
    while (s->phys < s->pos) {           /* skip forward via the FAT chain       */
        uint32_t got = fat32_stream_skip(&s->st, s->pos - s->phys);
        if (got == 0) {
            break;
        }
        s->phys += got;
    }
}

static size_t fat_src_read(void *ud, void *buf, size_t bytes)
{
    fat_src_t *s = (fat_src_t *)ud;
    fat_src_sync(s);
    int32_t got = fat32_stream_read(&s->st, buf, (uint32_t)bytes);
    if (got <= 0) {
        return 0;
    }
    s->phys += (uint32_t)got;
    s->pos  += (uint32_t)got;
    return (size_t)got;
}
static int fat_src_seek(void *ud, int offset, int origin)
{
    fat_src_t *s = (fat_src_t *)ud;
    long target = (origin == DECODER_SEEK_SET) ? (long)offset
                : (origin == DECODER_SEEK_END) ? (long)s->fsize + offset
                                               : (long)s->pos + offset;
    if (target < 0 || (uint32_t)target > s->fsize) {
        return 0;
    }
    s->pos = (uint32_t)target;           /* lazy: physical move deferred to read */
    return 1;
}
static int64_t fat_src_tell(void *ud)
{
    return (int64_t)((fat_src_t *)ud)->pos;
}

/* hal_audio source (runs in the DMA ISR): drain the ring. A short return on
 * underrun makes the HAL zero-pad the rest — a glitch, never a stall. */
static int ring_source(void *ud, int16_t *buf, int frames)
{
    (void)ud;
    return (int)pcm_ring_read(&g_ring, buf, (uint32_t)frames);
}

/* Producer: decode (FLAC or MP3) into the ring until it's full or end-of-stream.
 * Codec-agnostic — it only calls g_dec.ops->decode, so the same loop drives
 * whichever decoder open() installed. Foreground only. */
/* How much to decode BEFORE audio starts. The ring is large (RING_FRAMES ~6s)
 * so a drive spin-up mid-playback can't underrun it — but priming the WHOLE ring
 * up front made every song take seconds to start (decode + read ~6s of audio
 * before the first sample). Prime just ~1.5s (the old, proven-safe amount);
 * player_pump tops the ring up to full in the background once playback runs. */
#define PRIME_FRAMES (1u << 16)          /* 65536 frames ~ 1.49 s */

static void decode_pump(void)
{
    while (!g_eos && pcm_ring_fill(&g_ring) < PRIME_FRAMES &&
           pcm_ring_free(&g_ring) >= DECODE_FRAMES) {
        int got = g_dec.ops->decode(&g_dec, decode_buf, (int)DECODE_FRAMES);
        if (got <= 0) {
            g_eos = 1;
            break;
        }
        pcm_ring_write(&g_ring, decode_buf, (uint32_t)got);
    }
}

/* Decode at most ONE chunk into the ring, then return. Used inside the play
 * loop so decoding never monopolizes the loop: however slow the codec is (a
 * soft-float MP3 frame can take many ms), the loop still gets back to polling
 * the wheel and repainting each pass, so the UI never appears frozen. Returns
 * the frames decoded this step (0 at EOS or when the ring is already full). */
static int decode_step(void)
{
    if (g_eos || pcm_ring_free(&g_ring) < PLAY_STEP_FRAMES) {
        return 0;
    }
    int got = g_dec.ops->decode(&g_dec, decode_buf, (int)PLAY_STEP_FRAMES);
    if (got <= 0) {
        g_eos = 1;
        return 0;
    }
    pcm_ring_write(&g_ring, decode_buf, (uint32_t)got);
    return got;
}

/* FAT32 block callback: read absolute 512-byte LBAs off the disk.
 *
 * Retries a few times with a short wait: the drive spins down during a browse
 * idle, and the first PIO read after spin-down can error/time out while the
 * platter comes back up — which showed up as an intermittent "OPEN FAILED
 * FFFFFFFF" when starting a song after sitting in the list. A wait + retry
 * rides over the spin-up; a genuinely bad read still fails after all tries. */
int player_disk_read(void *ud, uint32_t lba, uint32_t count, void *buf)
{
    (void)ud;
    for (int attempt = 0; attempt < 6; attempt++) {
        if (ata_read_sectors(lba, count, buf) == 0) {
            return 0;
        }
        /* Immediate retries clear a transient PIO glitch with no audible stall
         * (a sleep here would freeze the decode mid-playback). Only escalate to
         * a wait after a few fast retries fail — that's the spun-down-drive case
         * at open time, where the ring isn't yet feeding audio. */
        if (attempt >= 2) {
            sleep_ms(60);
        }
    }
    return -1;
}

/* ---------------------------------------------------------------------------
 * Album art
 *
 * Each album folder carries a pre-scaled "folder.art" sidecar (host tools/
 * coreart.py): a CoreArt RGB565 bitmap the device blits straight onto the
 * now-playing screen — no on-device JPEG decode or scaling. The clus/size are
 * captured by the browser while enumerating the folder and handed to
 * player_play_queue; load_folder_art() reads + validates it once per queue so
 * the now-playing art stays the PLAYING folder's even as the user browses
 * elsewhere.
 * ------------------------------------------------------------------------- */
#define ART_MAX_DIM  120
#define ART_HDR_LEN  12                  /* "CART" + u16 ver/w/h/reserved        */
static uint8_t  g_art_raw[ART_HDR_LEN + ART_MAX_DIM * ART_MAX_DIM * 2];
static int      g_art_ok, g_art_w, g_art_h;

static void load_folder_art(fat32_t *fs, uint32_t clus, uint32_t size)
{
    g_art_ok = 0;
    if (clus == 0 || size < ART_HDR_LEN || size > sizeof g_art_raw) {
        return;
    }
    int32_t n = fat32_read_file(fs, clus, g_art_raw, size);
    if (n < (int32_t)ART_HDR_LEN) {
        return;
    }
    if (g_art_raw[0] != 'C' || g_art_raw[1] != 'A' ||
        g_art_raw[2] != 'R' || g_art_raw[3] != 'T') {
        return;
    }
    int w = g_art_raw[6] | (g_art_raw[7] << 8);
    int h = g_art_raw[8] | (g_art_raw[9] << 8);
    if (w <= 0 || h <= 0 || w > ART_MAX_DIM || h > ART_MAX_DIM) {
        return;
    }
    if ((int32_t)(ART_HDR_LEN + w * h * 2) > n) {
        return;
    }
    g_art_w = w;
    g_art_h = h;
    g_art_ok = 1;
}

int             player_art_ok(void)     { return g_art_ok; }
int             player_art_w(void)      { return g_art_w; }
int             player_art_h(void)      { return g_art_h; }
const uint16_t *player_art_pixels(void) { return (const uint16_t *)(g_art_raw + ART_HDR_LEN); }

/* ---------------------------------------------------------------------------
 * Background player engine
 *
 * Playback is decoupled from the UI: player_pump() decodes one bounded chunk
 * per main-loop pass and auto-advances at end of track, so audio keeps running
 * while the user navigates menus. A "queue" is the set of files in the folder a
 * track was launched from; the player owns its own copy so browsing elsewhere
 * doesn't disturb the currently-playing album (or its art).
 * ------------------------------------------------------------------------- */
static fat32_t       *g_pl_fs;
static browse_entry_t g_queue[QUEUE_MAX];
static int            g_queue_n;
static int            g_queue_idx;
static int            g_pl_active;        /* a track is loaded (playing OR paused) */
static int            g_pl_paused;         /* DMA suspended, position held          */
static uint32_t       g_pl_start_us;      /* USEC_TIMER at current track start    */
static uint32_t       g_pl_pause_us;       /* USEC_TIMER when paused (freezes clock) */
static uint32_t       g_pl_total_s;       /* current track length, seconds        */
static uint32_t       g_pl_low_fill;      /* ring low-water since last NP repaint  */
static int            g_shuffle;          /* pick the next track at random         */
static int            g_repeat;           /* 0 off, 1 all (loop queue), 2 one       */
static flac_meta_t    g_cur_meta;         /* tags/duration of the current track     */
static uint32_t       g_rng = 0x2545F491u;/* LCG state for shuffle (varies w/ USEC) */

void player_set_shuffle(int on)   { g_shuffle = on ? 1 : 0; }
void player_set_repeat(int mode)  { g_repeat  = mode; }

/* Count playable (non-dir) entries in the queue. */
static int queue_playable_count(void)
{
    int c = 0;
    for (int i = 0; i < g_queue_n; i++) {
        if (!g_queue[i].is_dir) c++;
    }
    return c;
}

/* Pick a random playable index, preferring one != `avoid` when possible. */
static int queue_random_playable(int avoid)
{
    int n = queue_playable_count();
    if (n <= 0) return -1;
    g_rng = g_rng * 1103515245u + 12345u + mmio_read32(USEC_TIMER_ADDR);
    int target = (int)((g_rng >> 8) % (uint32_t)n);      /* 0..n-1 among playable  */
    int pick = -1, seen = 0;
    for (int i = 0; i < g_queue_n; i++) {
        if (g_queue[i].is_dir) continue;
        if (seen == target) { pick = i; break; }
        seen++;
    }
    if (n > 1 && pick == avoid) {                         /* avoid an immediate repeat */
        for (int i = 1; i < g_queue_n; i++) {
            int j = (pick + i) % g_queue_n;
            if (!g_queue[j].is_dir) { pick = j; break; }
        }
    }
    return pick;
}

void player_init(fat32_t *fs)
{
    g_pl_fs = fs;
}

/* Open g_queue[g_queue_idx] and start the DAC. Returns 0, or -1 on any failure
 * (bad open / unsupported rate / audio init) — caller decides whether to skip. */
static int player_open_current(void)
{
    const browse_entry_t *b = &g_queue[g_queue_idx];
    if (b->art_clus) {                   /* per-track cover (mixed/shuffle queue) */
        load_folder_art(g_pl_fs, b->art_clus, b->art_size);
    }
    fat_src_open(&g_fsrc, g_pl_fs, b->clus, b->size);
    g_file_src.read = fat_src_read;
    g_file_src.seek = fat_src_seek;
    g_file_src.tell = fat_src_tell;
    g_file_src.userdata = &g_fsrc;
    /* Read tags + duration up front. Buffer the header through a read-ahead so
     * the parser's many tiny reads (4-byte block headers, per-comment lengths)
     * collapse into ~one backing disk read instead of ~30 full-FS-sector PIO
     * reads — a direct cut to time-to-first-sound. ra_buf is free here (the
     * decode path re-inits it just below). Harmless on non-FLAC (have=0).
     * diskbuf_init below starts fresh (inner_pos=-1) and re-seeks the source to 0
     * on its first read, undoing the position this advanced. */
    {
        readahead_t      meta_ra;
        decoder_source_t meta_src;
        readahead_init(&meta_ra, &g_file_src, ra_buf, sizeof ra_buf);
        readahead_as_source(&meta_ra, &meta_src);
        flac_meta_read(&meta_src, &g_cur_meta);
    }
    /* Anti-skip buffer over the raw disk, then the read-ahead shim over that. */
    diskbuf_init(&g_dbuf, &g_file_src, disk_buf, DISK_BUF_BYTES,
                 DISK_LOW, DISK_HIGH);
    diskbuf_as_source(&g_dbuf, &g_disk_src);
    readahead_init(&g_ra, &g_disk_src, ra_buf, sizeof ra_buf);
    readahead_as_source(&g_ra, &g_dec_src);

    decoder_arena_init(&g_arena, arena_buf, sizeof arena_buf);
    decoder_alloc_t alloc = decoder_arena_allocator(&g_arena);
    int oc = (b->fmt == 1) ? mp3_open_stream(&g_dec, &g_dec_src, &alloc)
                           : flac_open_stream(&g_dec, &g_dec_src, &alloc);
    if (oc != 0) {
        return -1;
    }
    if (g_dec.sample_rate != 44100u || g_dec.channels != 2u) {
        g_dec.ops->close(&g_dec);
        return -1;
    }

    g_pl_total_s = (g_dec.total_frames > 0)
                 ? (uint32_t)(g_dec.total_frames / 44100u) : 0;

    pcm_ring_init(&g_ring, ring_storage, RING_FRAMES);
    g_eos = 0;
    decode_pump();                       /* prime */

    /* Full audio bring-up per track (proven by the old sequential player):
     * player_stop() has already cleanly stopped any previous track, so re-init
     * here is over a quiescent HAL. */
    if (hal_audio_init(44100u, 2u) != 0) {
        g_dec.ops->close(&g_dec);
        return -1;
    }
    hal_audio_set_source(ring_source, 0);
    hal_audio_start();
    g_pl_start_us = mmio_read32(USEC_TIMER_ADDR);
    g_pl_low_fill = RING_FRAMES;
    g_pl_active   = 1;
    g_pl_paused   = 0;
    return 0;
}

/* Pause: suspend the DMA but keep the decoder, ring, and position — resume
 * re-primes the DAC from the still-full ring. Freezes the elapsed clock. */
void player_pause(void)
{
    if (!g_pl_active || g_pl_paused) {
        return;
    }
    hal_audio_stop();
    g_pl_pause_us = mmio_read32(USEC_TIMER_ADDR);
    g_pl_paused   = 1;
}

/* Resume from pause: shift the track start forward by the paused duration so the
 * elapsed clock is continuous, then restart the DAC (re-primes from the ring). */
void player_resume(void)
{
    if (!g_pl_active || !g_pl_paused) {
        return;
    }
    g_pl_start_us += mmio_read32(USEC_TIMER_ADDR) - g_pl_pause_us;
    g_pl_paused    = 0;
    hal_audio_start();
}

void player_toggle_pause(void)
{
    if (g_pl_paused) {
        player_resume();
    } else {
        player_pause();
    }
}

int player_paused(void) { return g_pl_paused; }

/* Stop playback and release the decoder. */
void player_stop(void)
{
    if (!g_pl_active) {
        return;
    }
    hal_audio_stop();
    g_pl_paused = 0;
    /* NOTE: do NOT close the decoder here. Closing it MID-DECODE (song switch)
     * hard-freezes the device (marker 9), while closing at end-of-track
     * (auto-advance) is fine — a decode-in-progress teardown hazard. The next
     * player_open_current() resets the whole arena, reclaiming this decoder's
     * memory anyway (there is no file handle to leak — the source is a custom
     * read callback), so skipping close here is safe. */
    g_pl_active = 0;
}

/* Advance to the next playable file in the queue (skipping folders and any that
 * fail to open). Marks the player idle when the queue is exhausted. */
static void player_advance(void)
{
    /* Close only a validly-open decoder. player_advance is also called after a
     * FAILED open (from player_play_queue), where g_dec was never opened — an
     * unconditional g_dec.ops->close() there dereferenced a NULL/stale ops and
     * hard-froze the device. g_pl_active is the "decoder open + running" flag. */
    if (g_pl_active) {
        hal_audio_stop();
        g_pl_active = 0;                  /* no close: next open resets the arena */
    }
    /* Repeat One: replay the same track (return early — no skip loop, so a lone
     * broken track can't spin forever). */
    if (g_repeat == 2) {
        if (player_open_current() == 0) {
            return;
        }
        /* fall through to normal selection if the replay somehow fails */
    }
    for (int tries = 0; tries <= g_queue_n; tries++) {
        int nxt = -1;
        if (g_shuffle) {
            nxt = queue_random_playable(g_queue_idx);
        } else {
            for (int j = g_queue_idx + 1; j < g_queue_n; j++) {
                if (!g_queue[j].is_dir) { nxt = j; break; }
            }
            if (nxt < 0 && g_repeat == 1) {   /* Repeat All: wrap to the first */
                for (int j = 0; j < g_queue_n; j++) {
                    if (!g_queue[j].is_dir) { nxt = j; break; }
                }
            }
        }
        if (nxt < 0) {
            return;                      /* queue done → idle */
        }
        g_queue_idx = nxt;
        if (player_open_current() == 0) {
            return;                      /* next track playing */
        }
        /* else: broken track, loop to skip it (bounded by `tries`) */
    }
}

/* Launch playback: copy the folder's entries as the queue, load its album art
 * once, and start at `start`. Replaces any current playback. */
void player_play_queue(const browse_entry_t *src, int n, int start,
                       uint32_t art_clus, uint32_t art_size)
{
    player_stop();
    for (int i = 0; i < n && i < QUEUE_MAX; i++) {
        g_queue[i] = src[i];
    }
    g_queue_n   = (n < QUEUE_MAX) ? n : QUEUE_MAX;
    g_queue_idx = start;
    load_folder_art(g_pl_fs, art_clus, art_size);  /* queue-level art */
    if (player_open_current() != 0) {
        player_advance();                /* skip a broken first track */
    }
}

/* Incremental large-queue builder (Shuffle Songs enqueues the whole library). */
void player_queue_begin(void)
{
    player_stop();
    g_queue_n = 0;
}

void player_queue_add(const browse_entry_t *e)
{
    if (g_queue_n < QUEUE_MAX) {
        g_queue[g_queue_n++] = *e;
    }
}

void player_queue_commit(int start)
{
    if (g_queue_n == 0) {
        return;
    }
    g_queue_idx = (start >= 0 && start < g_queue_n) ? start : 0;
    load_folder_art(g_pl_fs, 0, 0);      /* mixed queue: per-track art loads on open */
    if (player_open_current() != 0) {
        player_advance();                /* skip a broken first track */
    }
}

/* Decode one chunk per call and auto-advance at end of track. Called every
 * main-loop pass, so audio runs in the background while the UI is elsewhere. */
void player_pump(void)
{
    if (!g_pl_active || g_pl_paused) {
        return;                          /* paused: hold the ring + position     */
    }
    decode_step();
    /* Refill the anti-skip buffer in bounded bursts, but only while the PCM ring
     * has healthy headroom — so the (blocking) chunk read can't starve audio.
     * When the buffer is above its high watermark the pump does nothing and the
     * drive head sits idle; see codecs/diskbuf.h. */
    if (pcm_ring_fill(&g_ring) >= RING_DISK_GATE) {
        diskbuf_pump(&g_dbuf, DISK_CHUNK);
    }
    /* Apple-quiet playback: physically PARK the drive between refill bursts.
     * The diskbuf already goes idle (filling==0) once it's topped up; when it
     * does, spin the platters DOWN so they're not just idle-spinning. The next
     * burst's read spins them back up transparently (ata_read_sectors' DRQ wait
     * is time-based to tolerate the ~1-3 s spin-up, which the enlarged PCM ring
     * covers). While a burst is filling, the head is up by definition. */
    if (g_dbuf.filling) {
        g_drive_parked = 0;                      /* a burst is reading -> head up */
    } else if (!g_drive_parked &&
               diskbuf_fill_ahead(&g_dbuf) > DISK_LOW) {
        ata_standby();                           /* topped up + idle -> park it */
        g_drive_parked = 1;
    }
    uint32_t fill = pcm_ring_fill(&g_ring);
    if (fill < g_pl_low_fill) {
        g_pl_low_fill = fill;
    }
    if (g_eos && fill == 0u) {
        player_advance();
    }
}

int player_active(void) { return g_pl_active; }
int player_is_paused(void) { return g_pl_paused; }

const char *player_track_name(void) { return g_queue[g_queue_idx].name; }

const flac_meta_t *player_meta(void) { return &g_cur_meta; }

/* Probe a file's tags/duration WITHOUT disturbing playback — a throwaway
 * fat-source on the stack (used by the library scan for Songs/Genres). Returns
 * 0 on success (out->have==1), -1 on not-a-FLAC. */
static uint8_t     probe_ra_buf[16 * 1024];      /* buffers the metadata header  */
static readahead_t probe_ra;

int player_probe_meta(uint32_t clus, uint32_t size, flac_meta_t *out)
{
    fat_src_t s;
    decoder_source_t raw, buf;
    fat_src_open(&s, g_pl_fs, clus, size);
    raw.read = fat_src_read;
    raw.seek = fat_src_seek;
    raw.tell = fat_src_tell;
    raw.userdata = &s;
    /* Read the header through a 16 KB read-ahead so the parser's many tiny reads
     * collapse into ~one backing disk read — the difference between a snappy and
     * a minutes-long library scan over slow PIO. */
    readahead_init(&probe_ra, &raw, probe_ra_buf, sizeof probe_ra_buf);
    readahead_as_source(&probe_ra, &buf);
    return flac_meta_read(&buf, out);
}

uint32_t player_elapsed_s(void)
{
    if (!g_pl_active) {
        return 0;
    }
    uint32_t nowu = g_pl_paused ? g_pl_pause_us : mmio_read32(USEC_TIMER_ADDR);
    return (nowu - g_pl_start_us) / 1000000u;
}

uint32_t player_total_s(void) { return g_pl_total_s; }

uint32_t player_buf_pct(void) { return (g_pl_low_fill * 100u) / RING_FRAMES; }

void player_note_presented(void) { g_pl_low_fill = pcm_ring_fill(&g_ring); }

/* ---- Queue inspection + jump (the "Now Playing" queue view) --------------- */
int player_queue_len(void)      { return g_queue_n; }
int player_queue_current(void)  { return g_queue_idx; }

const char *player_queue_name(int i)
{
    return (i >= 0 && i < g_queue_n) ? g_queue[i].name : "";
}

int player_queue_is_dir(int i)
{
    return (i >= 0 && i < g_queue_n) ? g_queue[i].is_dir : 0;
}

/* Jump to queue entry `i` and play it (no-op for a folder / out-of-range). */
void player_jump(int i)
{
    if (i < 0 || i >= g_queue_n || g_queue[i].is_dir) {
        return;
    }
    player_stop();
    g_queue_idx = i;
    if (player_open_current() != 0) {
        player_advance();                /* skip a broken pick */
    }
}

/* Manual skip to the next playable track. Ignores Repeat-One (a deliberate skip
 * always moves) and wraps at the end. Shuffle picks a random track. */
void player_next(void)
{
    if (g_queue_n == 0) {
        return;
    }
    hal_audio_stop();
    g_pl_active = 0;
    for (int tries = 0; tries <= g_queue_n; tries++) {
        int nxt = -1;
        if (g_shuffle) {
            nxt = queue_random_playable(g_queue_idx);
        } else {
            for (int j = g_queue_idx + 1; j < g_queue_n; j++) {
                if (!g_queue[j].is_dir) { nxt = j; break; }
            }
            if (nxt < 0 && g_repeat == 1) {              /* wrap only on Repeat All */
                for (int j = 0; j < g_queue_n; j++) {
                    if (!g_queue[j].is_dir) { nxt = j; break; }
                }
            }
        }
        if (nxt < 0) return;                             /* past the last → stop     */
        g_queue_idx = nxt;
        if (player_open_current() == 0) return;
    }
}

/* Manual skip to the previous track — or restart the current one if we're more
 * than ~3s in (the familiar iPod behaviour). Wraps at the start. */
void player_prev(void)
{
    if (g_queue_n == 0) {
        return;
    }
    if (!g_shuffle && player_elapsed_s() > 3u) {         /* restart current */
        player_jump(g_queue_idx);
        return;
    }
    hal_audio_stop();
    g_pl_active = 0;
    for (int tries = 0; tries <= g_queue_n; tries++) {
        int prv = -1;
        if (g_shuffle) {
            prv = queue_random_playable(g_queue_idx);
        } else {
            for (int j = g_queue_idx - 1; j >= 0; j--) {
                if (!g_queue[j].is_dir) { prv = j; break; }
            }
            if (prv < 0) {                               /* wrap to last */
                for (int j = g_queue_n - 1; j >= 0; j--) {
                    if (!g_queue[j].is_dir) { prv = j; break; }
                }
            }
        }
        if (prv < 0) return;
        g_queue_idx = prv;
        if (player_open_current() == 0) return;
    }
}
