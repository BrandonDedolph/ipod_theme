/*
 * core/player/player.h — background streaming playback engine.
 *
 * Extracted verbatim from the old core/kernel/main.c god-file: the streaming
 * FLAC/MP3 decode path, the SPSC PCM ring feeding the DMA-driven DAC, the
 * fat32-backed byte source + read-ahead glue, the per-folder album art, and the
 * queue-based auto-advancing player. The UI (main.c) drives it through this
 * narrow API and never touches the decoder/ring statics directly.
 *
 * Behaviour is load-bearing and device-proven — do NOT change the decode /
 * stop / advance logic. In particular player_stop()/player_advance() must NOT
 * close the decoder mid-decode (that hard-freezes the core); the next open
 * reclaims its memory via the arena reset.
 */
#ifndef CORE_PLAYER_PLAYER_H
#define CORE_PLAYER_PLAYER_H

#include <stdint.h>
#include "../fs/fat32.h"
#include "../codecs/flac_meta.h"

/* Max entries in a browse listing. Bounds the UI's g_browse[] (one album's
 * worth of tracks). */
#define BROWSE_MAX 128
/* Max entries in the PLAYBACK queue — larger than a browse listing so
 * "Shuffle Songs" can hold the whole library, not just one album. */
#define QUEUE_MAX  1200
#define NAME_MAX   64                    /* stored display name (Nunito, ASCII)  */

/* One list row: a subdirectory or a playable file. Shared between the browser
 * (which fills it) and the player (which copies a folder's worth as its queue). */
typedef struct {
    char     name[NAME_MAX + 1];         /* display name (uppercased, font-safe) */
    uint32_t clus;
    uint32_t size;
    uint32_t art_clus;                   /* this track's cover (folder.art); 0 =  */
    uint32_t art_size;                   /*   use the queue-level art (album play) */
    uint8_t  fmt;                        /* 0 = FLAC, 1 = MP3 (only when !is_dir) */
    uint8_t  is_dir;                     /* 1 = subdirectory                     */
} browse_entry_t;

/* FAT32 block callback: read absolute 512-byte LBAs off the disk, with a
 * spin-up retry (rides over the drive spinning down during a browse idle).
 * Lives with the player because it's the disk read path the streaming decoder
 * rides on; main.c also passes it to fat32_mount. */
int player_disk_read(void *ud, uint32_t lba, uint32_t count, void *buf);

/* Bind the player to the mounted volume; call once before play. */
void player_init(fat32_t *fs);

/* Launch playback: copy `entries` as the queue, load the folder's album art
 * (folder.art at art_clus/art_size) once, and start at `start_idx`. Replaces
 * any current playback. */
void player_play_queue(const browse_entry_t *entries, int n, int start_idx,
                       uint32_t art_clus, uint32_t art_size);

/* Build a large queue incrementally (used by Shuffle Songs to enqueue the whole
 * library without a huge caller-side staging array): begin (stops playback and
 * clears), add each entry, then commit to start at `start`. Each entry's
 * art_clus is used for its cover (per-track). */
void player_queue_begin(void);
void player_queue_add(const browse_entry_t *e);
void player_queue_commit(int start);

/* Decode one bounded chunk and auto-advance at end of track. Call every
 * main-loop pass so audio runs while the UI is elsewhere. */
void player_pump(void);

/* Stop playback (does NOT close the decoder — see header note). */
void player_stop(void);

/* Pause / resume the current track (suspends the DAC, holds decoder+position);
 * toggle picks the opposite. No-ops when nothing is loaded. */
void player_pause(void);
void player_resume(void);
void player_toggle_pause(void);
int  player_paused(void);              /* 1 while paused */

/* Playback order/looping (driven by Settings). shuffle: pick the next track at
 * random. repeat: 0 = off (stop at queue end), 1 = all (loop the queue), 2 = one
 * (replay the current track). */
void player_set_shuffle(int on);
void player_set_repeat(int mode);

/* 1 while a track is loaded (playing OR paused). */
int  player_active(void);

/* 1 while playback is paused (a track is loaded but the DAC is stopped). Lets
 * callers distinguish "actively playing" (needs the disk for anti-skip refills)
 * from "paused" (drive can safely spin down). */
int  player_is_paused(void);

/* Tags + duration of the current track (parsed at open; fields empty/0 when a
 * tag is absent — fall back to the filename). */
const flac_meta_t *player_meta(void);

/* Probe an arbitrary file's tags without touching playback (library scan). */
int player_probe_meta(uint32_t clus, uint32_t size, flac_meta_t *out);

/* Now-playing readouts (valid while active; elapsed/buf return 0 otherwise). */
const char *player_track_name(void);
uint32_t    player_elapsed_s(void);
uint32_t    player_total_s(void);
uint32_t    player_buf_pct(void);

/* Reset the ring low-water mark; the now-playing renderer calls this once per
 * present so the buffer-health readout tracks the last frame's low point. */
void player_note_presented(void);

/* The playback queue (the folder a track was launched from), for the queue view.
 * player_jump switches to entry `i` and plays it (no-op for folders/out-of-range). */
int         player_queue_len(void);
int         player_queue_current(void);
const char *player_queue_name(int i);
int         player_queue_is_dir(int i);
void        player_jump(int i);

/* Manual track skip (Prev/Next buttons). player_prev restarts the current track
 * if >~3s in, else goes to the previous; both wrap and ignore Repeat-One. */
void        player_next(void);
void        player_prev(void);

/* Album art accessors for the now-playing renderer (the PLAYING folder's art,
 * held across browsing elsewhere). player_art_pixels() is RGB565, w*h. */
int             player_art_ok(void);
int             player_art_w(void);
int             player_art_h(void);
const uint16_t *player_art_pixels(void);

#endif /* CORE_PLAYER_PLAYER_H */
