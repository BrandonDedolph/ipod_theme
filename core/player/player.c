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
#include "hw/volume.h"
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
/* ...and a much higher bar when the drive is PARKED. That refill's first read
 * has to sit through the spin-up: ATA_SPINUP_US allows 4 s just for DRQ, so
 * kicking one off at the normal half-ring mark risks blocking decode for
 * longer than the 2.97 s of audio left. Only wake a parked platter from a
 * nearly-full ring (~5.2 s of headroom). */
#define RING_PARKED_GATE (RING_FRAMES - RING_FRAMES / 8u)

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
 * file through this. fat32_stream is forward-only, so a backward seek has to
 * restart the cluster walk and skip forward again.
 *
 * That used to mean "re-open at cluster 0 and walk the whole FAT chain", which
 * is O(file) in FAT sector reads — fine when the only seek was dr_flac sizing
 * the file at open, ruinous now that player_seek_to() exists and a backward
 * seek happens while the user is listening. So we keep a small ring of
 * CHECKPOINTS: a fat32_stream_t is only 16 bytes and fully describes a
 * position, so snapshotting it every ck_stride bytes lets a rewind resume from
 * the nearest earlier snapshot and walk at most one stride. 16 checkpoints
 * bound any backward seek to ~1/16 of the file's chain.
 */
#define FSRC_CKPT_MAX   16
#define FSRC_CKPT_MIN_STRIDE (256u * 1024u)

typedef struct {
    fat32_t       *fs;
    uint32_t       first_clus;
    uint32_t       fsize;
    fat32_stream_t st;
    uint32_t       phys;                 /* where the fat32 stream physically is */
    uint32_t       pos;                  /* logical position; a seek moves ONLY  */
                                         /* this — the physical walk is deferred */
    int            err;                  /* sticky: a disk read FAILED (not EOF)  */
    /* Cluster-chain checkpoints, ascending in ck_pos. */
    fat32_stream_t ck_st[FSRC_CKPT_MAX];
    uint32_t       ck_pos[FSRC_CKPT_MAX];
    uint32_t       ck_n;
    uint32_t       ck_stride;
    uint32_t       ck_next;              /* phys at which to take the next one */
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
    s->err = 0;
    s->ck_n = 0;
    s->ck_stride = sz / FSRC_CKPT_MAX;
    if (s->ck_stride < FSRC_CKPT_MIN_STRIDE) {
        s->ck_stride = FSRC_CKPT_MIN_STRIDE;
    }
    s->ck_next = s->ck_stride;
    fat32_stream_open(&s->st, fs, clus, sz);
}

/* 1 once a disk read has FAILED on this file (as opposed to hitting EOF).
 * Handed to the anti-skip buffer so it can tell the two apart. */
static int fat_src_error(void *ud)
{
    return ((fat_src_t *)ud)->err;
}

/* Snapshot the current cursor if we've walked a full stride since the last
 * one. Called after a successful read, where st describes exactly phys. */
static void fat_src_checkpoint(fat_src_t *s)
{
    if (s->ck_n >= FSRC_CKPT_MAX || s->phys < s->ck_next) {
        return;
    }
    s->ck_st[s->ck_n]  = s->st;
    s->ck_pos[s->ck_n] = s->phys;
    s->ck_n++;
    s->ck_next = s->phys + s->ck_stride;
}

/* Bring the physical stream up to the logical position — only when a read
 * actually needs data there. Forward is a cheap FAT-walk skip; backward
 * resumes from the newest checkpoint at or before the target (falling back to
 * cluster 0), then skips. This is what makes dr_flac's open-time seek-to-EOF
 * (to size the file) free: it never reads at EOF, so we never physically walk
 * there — and what keeps a mid-track backward seek from re-walking the whole
 * FAT chain. */
static void fat_src_sync(fat_src_t *s)
{
    if (s->pos < s->phys) {              /* rewind                               */
        uint32_t i = s->ck_n;
        while (i > 0 && s->ck_pos[i - 1] > s->pos) {
            i--;
        }
        if (i > 0) {                     /* resume from the nearest checkpoint    */
            s->st   = s->ck_st[i - 1];
            s->phys = s->ck_pos[i - 1];
        } else {                         /* none early enough: back to cluster 0  */
            fat32_stream_open(&s->st, s->fs, s->first_clus, s->fsize);
            s->phys = 0;
        }
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
    if (got < 0) {
        /* A disk error, NOT end of file. Collapsing the two into "return 0"
         * is what made one unreadable sector three minutes into a track look
         * exactly like the track ending — the player advanced and the user
         * saw a short song. Latch it so the diskbuf/player can tell. */
        s->err = 1;
        return 0;
    }
    if (got == 0) {
        return 0;                        /* genuine end of file */
    }
    s->err   = 0;                        /* a good read clears a transient fault */
    s->phys += (uint32_t)got;
    s->pos  += (uint32_t)got;
    fat_src_checkpoint(s);
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

/* ---------------------------------------------------------------------------
 * Real-time margin instrumentation
 *
 * Nothing measured decode cost, so there was no way to know how close FLAC is
 * to the edge on this CPU. These counters are deliberately trivial — two
 * USEC_TIMER reads per decode step (one per ~1024 frames, i.e. ~23 ms of
 * audio) and one increment in the ISR — so they stay permanently enabled and
 * a debug screen can read them through player_stats().
 * ------------------------------------------------------------------------- */
static player_stats_t g_stats;
static uint32_t       g_dec_us_acc;      /* microseconds since the last rollup */
static uint32_t       g_dec_frames_acc;  /* frames decoded since the last one  */
#define STATS_ROLLUP_FRAMES 44100u       /* recompute the rate ~once a second  */

/* hal_audio source (runs in the DMA ISR): drain the ring. A short return on
 * underrun makes the HAL zero-pad the rest — a glitch, never a stall. */
static int ring_source(void *ud, int16_t *buf, int frames)
{
    (void)ud;
    uint32_t got = pcm_ring_read(&g_ring, buf, (uint32_t)frames);
    if (got < (uint32_t)frames) {
        g_stats.underruns++;             /* one add; ISR-safe */
    }
    return (int)got;
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

/* Frames ever written to the ring (monotonic, wraps like the ring's own
 * indices). `g_written - pcm_ring_fill()` is therefore how many frames the DMA
 * has actually PLAYED — which is how the prefetch below knows when the
 * previous track has finished being heard, not just finished decoding. */
static uint32_t g_written;

/* A mono file arrives as one sample per frame; the ring and the DAC are both
 * stereo. Expand in place, back to front, so no second buffer is needed. */
static void mono_to_stereo(int16_t *buf, int frames)
{
    for (int i = frames - 1; i >= 0; i--) {
        int16_t s = buf[i];
        buf[i * 2]     = s;
        buf[i * 2 + 1] = s;
    }
}

/* One decode call into decode_buf, normalised to stereo and timed. Returns
 * frames, 0 at end of stream. */
static int decode_chunk(uint32_t max_frames)
{
    uint32_t t0  = mmio_read32(USEC_TIMER_ADDR);
    int      got = g_dec.ops->decode(&g_dec, decode_buf, (int)max_frames);
    g_dec_us_acc += mmio_read32(USEC_TIMER_ADDR) - t0;
    if (got <= 0) {
        return 0;
    }
    if (g_dec.channels == 1u) {
        mono_to_stereo(decode_buf, got);
    }
    g_dec_frames_acc += (uint32_t)got;
    if (g_dec_frames_acc >= STATS_ROLLUP_FRAMES) {
        /* Microseconds of CPU spent decoding 1000 frames. At 44.1 kHz the
         * real-time budget is 22676 us/kframe — anything approaching that is
         * the codec running out of headroom. */
        g_stats.decode_us_per_kframe = (g_dec_us_acc * 1000u) / g_dec_frames_acc;
        g_dec_us_acc     = 0;
        g_dec_frames_acc = 0;
    }
    return got;
}

static void decode_pump(void)
{
    while (!g_eos && pcm_ring_fill(&g_ring) < PRIME_FRAMES &&
           pcm_ring_free(&g_ring) >= DECODE_FRAMES) {
        int got = decode_chunk(DECODE_FRAMES);
        if (got <= 0) {
            g_eos = 1;
            break;
        }
        g_written += pcm_ring_write(&g_ring, decode_buf, (uint32_t)got);
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
    int got = decode_chunk(PLAY_STEP_FRAMES);
    if (got <= 0) {
        g_eos = 1;
        return 0;
    }
    g_written += pcm_ring_write(&g_ring, decode_buf, (uint32_t)got);
    return got;
}

/* FAT32 block callback: read absolute 512-byte LBAs off the disk.
 *
 * Retries a few times with a short wait: the drive spins down during a browse
 * idle, and the first PIO read after spin-down can error/time out while the
 * platter comes back up — which showed up as an intermittent "OPEN FAILED
 * FFFFFFFF" when starting a song after sitting in the list. A wait + retry
 * rides over the spin-up; a genuinely bad read still fails after all tries. */
/* Set for exactly ONE read when the pump wakes a PARKED drive. That first read
 * has to wait out the spin-up, and ATA_SPINUP_US already allows 4 s for DRQ —
 * six of those plus the backoff is ~24 s of frozen decode against a 5.94 s
 * ring. One attempt (~4 s worst case) then back to the main loop: the diskbuf
 * keeps its filling state, so the next pump pass retries with decode and the
 * UI alive in between. */
static int g_spinup_probe;

int player_disk_read(void *ud, uint32_t lba, uint32_t count, void *buf)
{
    (void)ud;
    if (g_spinup_probe) {
        g_spinup_probe = 0;
        return ata_read_sectors(lba, count, buf) == 0 ? 0 : -1;
    }
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
/* Bumped every time the loaded art changes — including when it is CLEARED for
 * a track whose album has no cover. A downstream cache of the scaled hero can
 * key on this instead of the queue index, which doesn't change on repeat-one
 * or on a new queue that happens to start at the same position. */
static uint32_t g_art_seq;

static void load_folder_art(fat32_t *fs, uint32_t clus, uint32_t size)
{
    g_art_ok = 0;
    g_art_seq++;
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
uint32_t        player_art_seq(void)    { return g_art_seq; }
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
static int            g_last_err;         /* why the last open/skip failed          */

/* Format the DAC is currently clocked at. hal_audio_init is only re-issued
 * when this has to change, which is what lets a same-format track hand over
 * without touching (and therefore without RESETTING) the codec. */
static uint32_t       g_out_rate;

/* ---------------------------------------------------------------------------
 * Prefetch / hand-over
 *
 * The old auto-advance waited for the PCM ring to run completely dry, and only
 * THEN opened the next file: metadata parse, diskbuf window reset with
 * synchronous 32 KB fetches, possibly a drive spin-up (the pump parks the
 * platter once it's topped up) and a 65536-frame prime — 1.5 s of silence at
 * best, 3-5 s with a parked drive, every single track.
 *
 * The observation that makes this cheap to fix: once the decoder reports EOS,
 * the CURRENT track is completely decoded. Its decoder, its arena, its disk
 * buffer and its file cursor are all dead weight from that instant, while the
 * ring still holds up to 5.94 s of its audio waiting to be played. So at EOS
 * we immediately open the NEXT track over the same statics and keep decoding
 * into the same ring — its frames simply queue up behind the tail of the
 * previous one. No second arena, no second disk buffer, and when the two
 * tracks share a sample rate the DAC is never stopped at all: true gapless.
 *
 * What must NOT happen immediately is the PRESENTATION switch — the title,
 * elapsed clock and artwork belong to whatever is audible, and that is still
 * the old track for several seconds. g_written (frames ever queued) minus the
 * ring fill is the number of frames the DMA has actually played, so we record
 * the queue position of the handover in g_boundary and commit the UI-visible
 * state only once playback crosses it.
 *
 * Only one track is ever prefetched (a track shorter than the ring would
 * otherwise stack up handovers); a second EOS simply waits for the commit.
 * ------------------------------------------------------------------------- */
static int            g_pending;          /* a prefetched track is queued behind  */
static int            g_pending_idx;      /* its queue index                       */
static flac_meta_t    g_pending_meta;
static uint32_t       g_pending_total_s;
static int            g_pending_gapless;  /* same format: no DAC re-init needed    */
static uint32_t       g_boundary;         /* g_written at its first frame          */
static int            g_prefetch_tried;   /* prefetch attempted for this track     */

/* 1 when the queue's art is a single ALBUM cover shared by every entry (the
 * folder-play case), 0 when each entry carries its own art_clus (Shuffle
 * Songs / Songs / any mixed queue). It's the difference between "this entry
 * has no art of its own, keep the album's" and "this entry's album has no
 * cover, show nothing" — conflating them left the PREVIOUS track's cover on
 * screen for every coverless album in a mixed queue. */
static int            g_queue_art_shared;

/* Bumped once per successful open. A monotonic "the audible track changed"
 * trigger that a queue-index compare cannot provide: repeat-one, prev-restart
 * and a single-track shuffle all reopen the SAME index. */
static uint32_t       g_open_seq;

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
    /* Init ONCE, reset per track: high_water and oom then accumulate across the
     * whole session, which is the only way an arena OOM is ever observable
     * (see player_stats). */
    decoder_arena_init(&g_arena, arena_buf, sizeof arena_buf);
}

/* Load the art appropriate to queue entry `b`. For a mixed queue a zero
 * art_clus means "this album has no cover" and must CLEAR the art; for an
 * album queue the cover is queue-level and already loaded. */
static void load_track_art(const browse_entry_t *b)
{
    if (!g_queue_art_shared) {
        load_folder_art(g_pl_fs, b->art_clus, b->art_size);
    }
}

/*
 * Build the decode chain for queue entry `idx` — file source, anti-skip
 * buffer, read-ahead shim, codec — and parse its tags into *meta. Touches NO
 * audio state: no HAL, no PCM ring, no artwork. That separation is what lets
 * the prefetch path open the next track while the previous one is still
 * playing out of the ring.
 *
 * Returns 0, or -1 if the file doesn't parse as FLAC/MP3.
 */
static int track_open(int idx, flac_meta_t *meta)
{
    const browse_entry_t *b = &g_queue[idx];

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
        flac_meta_read(&meta_src, meta);
    }
    /* Anti-skip buffer over the raw disk, then the read-ahead shim over that. */
    diskbuf_init(&g_dbuf, &g_file_src, disk_buf, DISK_BUF_BYTES,
                 DISK_LOW, DISK_HIGH);
    diskbuf_set_error_hook(&g_dbuf, fat_src_error, &g_fsrc);
    diskbuf_as_source(&g_dbuf, &g_disk_src);
    readahead_init(&g_ra, &g_disk_src, ra_buf, sizeof ra_buf);
    readahead_as_source(&g_ra, &g_dec_src);

    decoder_arena_reset(&g_arena);
    decoder_alloc_t alloc = decoder_arena_allocator(&g_arena);
    int oc = (b->fmt == 1) ? mp3_open_stream(&g_dec, &g_dec_src, &alloc)
                           : flac_open_stream(&g_dec, &g_dec_src, &alloc);
    if (oc != 0) {
        return -1;
    }
    /* ReplayGain, only when the file actually carries the tag — an untagged
     * file must leave the decode path bit-identical. Track gain preferred;
     * album gain is the fallback so an album plays at a consistent level. */
    if (b->fmt != 1) {
        if (meta->have_rg & FLAC_META_RG_TRACK) {
            flac_set_gain_db_q8(&g_dec, meta->rg_track_q8);
        } else if (meta->have_rg & FLAC_META_RG_ALBUM) {
            flac_set_gain_db_q8(&g_dec, meta->rg_album_q8);
        }
    }
    g_eos = 0;
    return 0;
}

/* Seconds of audio in `d`, from its own rate (not a hardcoded 44100). */
static uint32_t track_total_s(const decoder_t *d)
{
    uint32_t rate = d->sample_rate ? d->sample_rate : 44100u;
    return (d->total_frames > 0) ? (uint32_t)(d->total_frames / rate) : 0;
}

/*
 * Bring the DAC up at `rate` and start pulling from the ring.
 *
 * hal_audio_init RESETS the WM8758 and writes 0 dB headphone gain, so the
 * user's volume and balance have to be re-applied — and re-applied BEFORE the
 * DMA is kicked, not after, or every track change emits ~30-80 ms at unity
 * gain. The bug this fixes was worse than that: the only re-apply anywhere was
 * a POINTER compare in main.c (`tn != last_tn` against &g_queue[idx].name), so
 * whenever Repeat-One or a Prev-restart reopened the same queue index the
 * pointer was identical, the branch never fired, and the codec stayed at unity
 * for the WHOLE track — full-scale into someone's headphones at a "20%"
 * setting. Doing it here covers every path into playback by construction.
 *
 * Returns 0, or -1 if the HAL can't clock this format.
 */
static int audio_bringup(uint32_t rate)
{
    if (hal_audio_init(rate, 2u) != 0) {
        return -1;
    }
    hal_volume_set(hal_volume_get());
    hal_balance_set(hal_balance_get());
    hal_audio_set_source(ring_source, 0);
    hal_audio_start();
    g_out_rate = rate;
    return 0;
}

/* Open g_queue[g_queue_idx] and start the DAC. Returns 0, or -1 on failure;
 * player_last_error() then says whether the file was unreadable or simply a
 * format the DAC can't clock, so the UI can eventually explain the skip. */
static int player_open_current(void)
{
    load_track_art(&g_queue[g_queue_idx]);

    g_pending        = 0;                /* any prefetch is superseded */
    g_prefetch_tried = 0;
    if (track_open(g_queue_idx, &g_cur_meta) != 0) {
        g_last_err = PLAYER_ERR_OPEN;
        return -1;
    }
    g_pl_total_s = track_total_s(&g_dec);

    pcm_ring_init(&g_ring, ring_storage, RING_FRAMES);
    g_written  = 0;
    g_boundary = 0;
    decode_pump();                       /* prime */

    /* Full audio bring-up per track (proven by the old sequential player):
     * player_stop() has already cleanly stopped any previous track, so re-init
     * here is over a quiescent HAL. The rate comes from the FILE now — the old
     * hard `!= 44100 || != 2` rejection meant a 48 kHz album or a mono podcast
     * scrolled past unplayed and unexplained. Mono is up-mixed to stereo in
     * the decode step, so the DAC only ever sees 2 channels and we depend on
     * the HAL for nothing but the rate. */
    if (audio_bringup(g_dec.sample_rate) != 0) {
        g_last_err = PLAYER_ERR_RATE;
        return -1;
    }
    g_pl_start_us = mmio_read32(USEC_TIMER_ADDR);
    g_pl_low_fill = RING_FRAMES;
    g_pl_active   = 1;
    g_pl_paused   = 0;
    g_last_err    = PLAYER_OK;
    g_open_seq++;
    return 0;
}

/*
 * Open the current entry, restoring a paused transport if we were paused.
 *
 * Every manual skip (Next / Prev / queue jump) funnels through here. Without
 * it, skipping while paused silently RESUMED playback: player_open_current
 * unconditionally clears g_pl_paused and calls hal_audio_start(). The codec
 * still comes up at the user's gain either way — audio_bringup re-latches
 * volume before the DMA is kicked, and the pause below only suspends the DMA
 * again, it doesn't touch the gain.
 */
static int open_current_keep_pause(int was_paused)
{
    if (player_open_current() != 0) {
        return -1;
    }
    if (was_paused) {
        player_pause();
    }
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
    g_pending   = 0;                     /* drop any queued hand-over */
    /* NOTE: do NOT close the decoder here. Closing it MID-DECODE (song switch)
     * hard-freezes the device (marker 9), while closing at end-of-track
     * (auto-advance) is fine — a decode-in-progress teardown hazard. The next
     * player_open_current() resets the whole arena, reclaiming this decoder's
     * memory anyway (there is no file handle to leak — the source is a custom
     * read callback), so skipping close here is safe. */
    g_pl_active = 0;
}

/* The next playable index AFTER `from` for a manual skip: forward, wrapping
 * only under Repeat All. -1 when there is none. Ignores Repeat-One (a
 * deliberate skip always moves). */
static int next_playable(int from)
{
    for (int j = from + 1; j < g_queue_n; j++) {
        if (!g_queue[j].is_dir) return j;
    }
    if (g_repeat == 1) {                  /* Repeat All: wrap to the first */
        for (int j = 0; j < g_queue_n; j++) {
            if (!g_queue[j].is_dir) return j;
        }
    }
    return -1;
}

/* The index AUTO-advance should play after `from` (Repeat-One / Shuffle /
 * Repeat-All aware). -1 when the queue is finished. */
static int auto_next_index(int from)
{
    if (g_repeat == 2) {                  /* Repeat One: the same track again */
        return from;
    }
    if (g_shuffle) {
        return queue_random_playable(from);
    }
    return next_playable(from);
}

/*
 * End of the current track's DECODE: open the next one now, behind the audio
 * still queued in the ring. See the prefetch note above the globals.
 *
 * Only ever called with the ring holding the tail of the finished track, so
 * the blocking work here (metadata parse, diskbuf priming, a possible drive
 * spin-up) is covered by up to 5.94 s of audio that is already decoded — where
 * before it happened in dead silence after the ring had drained.
 */
static void player_advance(void);        /* fwd: the commit path can skip too */

static void prefetch_next(void)
{
    int nxt = auto_next_index(g_queue_idx);

    for (int tries = 0; nxt >= 0 && tries <= g_queue_n; tries++) {
        /* The finished decoder is not closed — closing MID-DECODE hard-freezes
         * the device (marker 9) and track_open's arena reset reclaims its
         * memory anyway. It is at EOS, so nothing will call into it again. */
        if (track_open(nxt, &g_pending_meta) != 0) {
            g_last_err = PLAYER_ERR_OPEN;
            /* Step FORWARD past the broken entry rather than re-asking
             * auto_next_index, which under Repeat-One would hand back the same
             * unopenable file forever. */
            nxt = next_playable(nxt);
            continue;
        }
        g_pending          = 1;
        g_pending_idx      = nxt;
        g_pending_total_s  = track_total_s(&g_dec);
        g_pending_gapless  = (g_dec.sample_rate == g_out_rate);
        g_boundary         = g_written;   /* everything queued so far is the old track */
        /* No decode_pump() here: priming would block for ~1.5 s while the
         * previous track is audible. decode_step fills the ring at leisure —
         * and in the gapless case it is appending, so there is nothing to
         * prime for. */
        return;
    }
}

/*
 * The prefetched track has now reached the speakers: switch everything
 * user-visible over to it. When the format changed, the ring has just run dry
 * at exactly this point, so this is where the DAC is re-clocked (and the
 * volume re-latched, since that re-init resets the codec).
 */
static void pending_commit(void)
{
    g_queue_idx  = g_pending_idx;
    g_cur_meta   = g_pending_meta;
    g_pl_total_s = g_pending_total_s;
    load_track_art(&g_queue[g_queue_idx]);
    g_pending        = 0;
    g_prefetch_tried = 0;                /* the new track gets its own prefetch */

    if (!g_pending_gapless) {
        /* Different sample rate: the ring is empty here by construction (we
         * held decode back until the old track finished playing), so re-clock
         * the DAC and prime before letting it run again. */
        hal_audio_stop();
        decode_pump();
        if (audio_bringup(g_dec.sample_rate) != 0) {
            /* The DAC can't clock this file. Don't let one odd-rate track end
             * the album — skip it the way a corrupt file is skipped. */
            g_last_err  = PLAYER_ERR_RATE;
            g_pl_active = 0;
            player_advance();
            return;
        }
    }
    g_pl_start_us = mmio_read32(USEC_TIMER_ADDR);
    g_pl_low_fill = RING_FRAMES;
    g_last_err    = PLAYER_OK;
    g_open_seq++;
}

/* Advance to the next playable file in the queue (skipping folders and any that
 * fail to open). Marks the player idle when the queue is exhausted. Used for
 * the immediate (non-prefetched) paths: a broken first track, mainly. */
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
    /* Repeat One: replay the same track (single attempt — falling into the skip
     * loop below with Repeat-One set would retry a lone broken track forever). */
    if (g_repeat == 2 && player_open_current() == 0) {
        return;
    }
    for (int tries = 0; tries <= g_queue_n; tries++) {
        int nxt = g_shuffle ? queue_random_playable(g_queue_idx)
                            : next_playable(g_queue_idx);
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
    /* One album, one cover: every entry shares it, so per-track art loading is
     * suppressed for the life of this queue. */
    g_queue_art_shared = (art_clus != 0);
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
    /* Mixed queue: each entry carries its own art_clus, and a zero one means
     * "this album has no cover" — NOT "keep whatever is loaded". Without this
     * distinction every coverless album in a Shuffle Songs queue displayed the
     * previous track's artwork. */
    g_queue_art_shared = 0;
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

    uint32_t fill = pcm_ring_fill(&g_ring);

    /* Has the prefetched track reached the speakers yet? g_written - fill is
     * the frame the DMA is playing; g_boundary is where the handover sits in
     * that stream. Committing on the DECODE finishing instead would flip the
     * title and clock up to 6 s early. The compare is a SIGNED difference so
     * it stays correct across the 32-bit wrap the ring indices also ride. */
    if (g_pending && (int32_t)((g_written - fill) - g_boundary) >= 0) {
        pending_commit();
        if (!g_pl_active) {
            return;                      /* commit failed (unclockable rate) */
        }
        fill = pcm_ring_fill(&g_ring);
    }

    /* Decode, unless a format-changing handover is waiting: those frames must
     * not be queued behind the old track's tail, because the DAC is still
     * clocked at the old rate until the ring runs dry and pending_commit
     * re-inits it. */
    if (!(g_pending && !g_pending_gapless)) {
        decode_step();
    }

    /* Refill the anti-skip buffer in bounded bursts, but only while the PCM ring
     * has healthy headroom — so the (blocking) chunk read can't starve audio.
     * When the buffer is above its high watermark the pump does nothing and the
     * drive head sits idle; see codecs/diskbuf.h.
     *
     * A PARKED drive needs a much higher gate. The first read after a park has
     * to wait out the spin-up (ATA_SPINUP_US allows 4 s for DRQ), so starting
     * one at the normal half-ring mark could block decode for longer than the
     * remaining 2.97 s of audio. Wait until the ring is nearly full, and mark
     * the read as a single non-retrying probe (see g_spinup_probe) so one
     * failure costs one spin-up wait instead of six plus backoff. */
    uint32_t gate = RING_DISK_GATE;
    if (g_drive_parked) {
        gate = RING_PARKED_GATE;
        /* ...unless the compressed buffer is nearly dry. Holding out for a
         * fuller ring then just hands the same spin-up to diskbuf_read's
         * synchronous fallback, which runs with no headroom at all. */
        if (diskbuf_fill_ahead(&g_dbuf) < DISK_LOW / 4u) {
            gate = RING_DISK_GATE;
        }
    }
    if (pcm_ring_fill(&g_ring) >= gate) {
        if (g_drive_parked || g_dbuf.err_streak > 0) {
            /* Waking a parked platter, or retrying a read that already failed
             * once: one attempt, no backoff loop. */
            g_drive_parked = 0;
            g_spinup_probe = 1;
        }
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
    fill = pcm_ring_fill(&g_ring);
    if (fill < g_pl_low_fill) {
        g_pl_low_fill = fill;
    }
    g_stats.ring_low_frames = g_pl_low_fill;

    if (g_eos) {
        /* The track is fully DECODED. Distinguish "it ended" from "the disk
         * stopped answering": collapsing the two is what let one unreadable
         * sector three minutes in look like a short song. */
        if (diskbuf_error(&g_dbuf)) {
            /* Play out what is already decoded, then stop and SAY so rather
             * than advancing as if the track had simply finished. */
            if (fill == 0u) {
                hal_audio_stop();
                g_pl_active = 0;
                g_pending   = 0;
                g_last_err  = PLAYER_ERR_READ;
            }
            return;                      /* never prefetch onto a dead disk */
        }
        /* Open the next track NOW, behind the audio still in the ring — this
         * is where the 1.5-5 s inter-track silence used to be. Only one
         * handover is ever in flight. */
        if (!g_pending && !g_prefetch_tried) {
            g_prefetch_tried = 1;        /* once per track, not once per pass */
            prefetch_next();
        }
        if (!g_pending && fill == 0u) {
            player_advance();            /* nothing queued: idle out (or retry) */
        }
    }
}

int player_active(void) { return g_pl_active; }

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

/* Jump to queue entry `i` and play it (no-op for a folder / out-of-range).
 * Keeps a paused transport paused — selecting a track from the queue view
 * while paused used to silently start playback. */
void player_jump(int i)
{
    if (i < 0 || i >= g_queue_n || g_queue[i].is_dir) {
        return;
    }
    int was_paused = g_pl_paused;
    player_stop();
    g_queue_idx = i;
    if (open_current_keep_pause(was_paused) != 0) {
        player_advance();                /* skip a broken pick */
    }
}

/*
 * Manual skip to the next playable track. Ignores Repeat-One (a deliberate
 * skip always moves); wraps only under Repeat All, and on the LAST track with
 * Repeat off it is a no-op — which is what an iPod does.
 *
 * The successor is chosen BEFORE anything is torn down. Stopping first and
 * then discovering there is no next track left the player permanently
 * inactive, and since every transport handler in the UI is gated on
 * player_active(), Play / Prev / Next all went dead with no way back into the
 * queue short of re-entering it from a list.
 */
void player_next(void)
{
    if (g_queue_n == 0) {
        return;
    }
    int nxt = g_shuffle ? queue_random_playable(g_queue_idx)
                        : next_playable(g_queue_idx);
    if (nxt < 0) {
        return;                          /* past the last: leave playback alone */
    }
    int was_paused = g_pl_paused;
    hal_audio_stop();
    g_pl_active = 0;
    for (int tries = 0; tries <= g_queue_n && nxt >= 0; tries++) {
        g_queue_idx = nxt;
        if (open_current_keep_pause(was_paused) == 0) {
            return;
        }
        nxt = g_shuffle ? queue_random_playable(g_queue_idx)
                        : next_playable(g_queue_idx);
    }
}

/* The previous playable index before `from`, wrapping to the last entry.
 * -1 only when the queue holds no playable entry at all. */
static int prev_playable(int from)
{
    for (int j = from - 1; j >= 0; j--) {
        if (!g_queue[j].is_dir) return j;
    }
    for (int j = g_queue_n - 1; j >= 0; j--) {   /* wrap to last */
        if (!g_queue[j].is_dir) return j;
    }
    return -1;
}

/* Manual skip to the previous track — or restart the current one if we're more
 * than ~3s in (the familiar iPod behaviour). Wraps at the start. Keeps a
 * paused transport paused. */
void player_prev(void)
{
    if (g_queue_n == 0) {
        return;
    }
    if (!g_shuffle && player_elapsed_s() > 3u) {         /* restart current */
        player_jump(g_queue_idx);
        return;
    }
    int prv = g_shuffle ? queue_random_playable(g_queue_idx)
                        : prev_playable(g_queue_idx);
    if (prv < 0) {
        return;
    }
    int was_paused = g_pl_paused;
    hal_audio_stop();
    g_pl_active = 0;
    for (int tries = 0; tries <= g_queue_n && prv >= 0; tries++) {
        g_queue_idx = prv;
        if (open_current_keep_pause(was_paused) == 0) {
            return;
        }
        prv = g_shuffle ? queue_random_playable(g_queue_idx)
                        : prev_playable(g_queue_idx);
    }
}

/* ---- Seek ----------------------------------------------------------------
 *
 * Both wrappers have always implemented decoder_ops->seek and nothing called
 * it. The two things that make a naive seek slow are handled elsewhere:
 *   - a backward seek drives diskbuf_seek outside its window, which resets the
 *     window and rewinds fat_src — now served from the cluster-chain
 *     checkpoints in fat_src_t instead of re-walking the FAT from cluster 0;
 *   - FLAC lands on a SEEKTABLE seekpoint (dr_flac parses and binary-searches
 *     it at open), so it is O(log n) plus one frame. MP3 has no seek table and
 *     falls back to dr_mp3's brute-force scan — accurate but linear; see the
 *     note in mp3.c.
 *
 * The DAC is stopped across the seek so the ISR can't drain PCM belonging to
 * the old position, and hal_audio_init is deliberately NOT re-issued: the
 * format hasn't changed, and re-initialising would reset the codec's gain.
 */
int player_seek_to(uint32_t sec)
{
    if (!g_pl_active || !g_dec.ops || !g_dec.ops->seek) {
        return -1;
    }
    if (g_pending) {
        return -1;                       /* mid-handover: refuse rather than guess */
    }
    uint32_t rate   = g_dec.sample_rate ? g_dec.sample_rate : 44100u;
    uint64_t target = (uint64_t)sec * rate;
    if (g_dec.total_frames > 0 && target >= g_dec.total_frames) {
        target = g_dec.total_frames - 1;
        sec    = (uint32_t)(target / rate);
    }

    int was_paused = g_pl_paused;
    hal_audio_stop();
    if (g_dec.ops->seek(&g_dec, target) != DECODER_OK) {
        if (!was_paused) {
            hal_audio_start();
        }
        return -1;
    }
    pcm_ring_init(&g_ring, ring_storage, RING_FRAMES);
    g_written        = 0;
    g_boundary       = 0;
    g_eos            = 0;
    g_prefetch_tried = 0;
    decode_pump();                       /* re-prime before audio resumes */
    g_pl_start_us = mmio_read32(USEC_TIMER_ADDR) - sec * 1000000u;
    g_pl_low_fill = RING_FRAMES;
    if (!was_paused) {
        hal_audio_start();
    }
    return 0;
}

int player_seek_seconds(int delta)
{
    if (!g_pl_active) {
        return -1;
    }
    int64_t pos = (int64_t)player_elapsed_s() + delta;
    if (pos < 0) {
        pos = 0;
    }
    if (g_pl_total_s > 0 && pos > (int64_t)g_pl_total_s) {
        pos = (int64_t)g_pl_total_s;
    }
    return player_seek_to((uint32_t)pos);
}

/* ---- Status --------------------------------------------------------------- */

int player_last_error(void) { return g_last_err; }

uint32_t player_open_seq(void) { return g_open_seq; }

const player_stats_t *player_stats(void)
{
    g_stats.arena_high_water = (uint32_t)g_arena.high_water;
    g_stats.arena_oom        = g_arena.oom;
    return &g_stats;
}
