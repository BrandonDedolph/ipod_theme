/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/fs/m3u.h — M3U8 (UTF-8) playlist READER.
 *
 * The readable half of "Playlists": turn a .m3u8 file on the FAT32 volume
 * into a bounded list of track references the player can later be handed.
 * Read-only. No UI, no menu wiring, no writing — a later change resolves each
 * reference to a cluster and hands the result to player_play_queue().
 *
 * WHY M3U8 AND NOT A BINARY FORMAT
 * Playlists are USER data, not derived index. The library index is ours to
 * rebuild at will; a playlist the user made must survive a firmware switch and
 * be editable from a desktop. M3U8 is what Rockbox and iTunes both speak, and
 * it is the format the project's playlist design memo specifies. The cost is
 * that we now parse a text file we did not write — see ROBUSTNESS below.
 *
 * FORMAT, as actually found in the wild (there is no RFC; the de-facto rules):
 *   - Line-oriented UTF-8. LF or CRLF. An optional UTF-8 BOM (EF BB BF).
 *   - A line whose first non-blank byte is '#' is a comment or a directive:
 *       #EXTM3U                       header, should be line 1
 *       #EXTINF:<seconds>,<title>     duration + display title, and it
 *                                     describes the NEXT non-comment line
 *     Everything else beginning with '#' is ignored.
 *   - Any other non-blank line is a path to a track.
 *
 * WHAT THIS MODULE DELIBERATELY DOES NOT DO
 *   - It does NOT touch the directory tree. Resolving a path to a FAT32
 *     cluster is the next change's job; this one produces the normalised
 *     path string that makes that lookup a simple segment walk (see
 *     m3u_entry_t.path). Splitting it this way keeps the parser host-testable
 *     against raw bytes with no disk image at all.
 *   - It does NOT write, sort, de-duplicate, or check that a track exists.
 *   - It does NOT follow nested playlists (a .m3u listed inside a .m3u is
 *     just another entry; nothing recurses, so nothing can recurse forever).
 *
 * ROBUSTNESS — THIS PARSES AN UNTRUSTED FILE
 * A playlist arrives over USB from a desktop we know nothing about, or from a
 * previous firmware, or from a half-finished transfer. On this device a wild
 * index or a non-terminating loop is a brick until the battery is pulled:
 * there is no watchdog, no debugger and no MMU. So, as in fat32.c:
 *   - EVERY loop is bounded. The whole parse is bounded by M3U_FILE_MAX bytes
 *     of input regardless of what the directory entry claims the size is.
 *   - Nothing is allocated, and the module holds NO statics: all working
 *     memory is the caller's (m3u_scratch_t) and all output is the caller's
 *     array. The parser is therefore reentrant.
 *   - Every cap has a REPORTED overflow. A file with more tracks than the
 *     caller's array does not silently lose the tail: m3u_result_t.truncated
 *     says so and .total says how many there really were. Same for an
 *     over-long line, a path that will not fit, and a file bigger than the
 *     byte cap. Silent loss in a playlist is indistinguishable from a bug in
 *     the parser, which is why none of these are silent.
 *
 * MEMORY COST (all caller-owned; this module's .bss is zero)
 *   sizeof(m3u_scratch_t)  1488 bytes, one instance, reusable across parses
 *   sizeof(m3u_entry_t)    264 bytes x the caller's out_max
 *                          (so a 128-track array is 33 KB)
 *   stack                  a few hundred bytes; no recursion anywhere
 * Both sizes are asserted in tests/fs/m3u_test.c so they cannot drift
 * silently past a caller that budgeted for them.
 */
#ifndef CORE_FS_M3U_H
#define CORE_FS_M3U_H

#include <stdint.h>

#include "fat32.h"

/*
 * Caps. Each is a deliberate number, not a round one:
 *
 * M3U_PATH_MAX — the normalised, volume-root-relative path we hand back.
 *   FAT32's own limit on a full path is 260 characters including the drive
 *   ("C:\"), so 191 leaves room for every path a Windows tool could have
 *   written into the file in the first place, while keeping m3u_entry_t under
 *   a quarter-KB so a 128-entry array is 33 KB rather than 100 KB.
 *
 * M3U_TITLE_MAX — 64, deliberately the same as player.h's NAME_MAX, because
 *   the display title ends up in the same list rows the browser fills. A
 *   longer title is truncated on a UTF-8 CHARACTER boundary (never mid
 *   sequence — a split sequence renders as a replacement glyph or, worse,
 *   walks a decoder off the end of the string).
 *
 * M3U_LINE_MAX — the longest logical line we will assemble. A path that fits
 *   M3U_PATH_MAX can still arrive as a longer line (".././a/./b/"), and an
 *   #EXTINF title can be long, so this is comfortably above both. A line
 *   longer than this is DISCARDED, not truncated: half a path is a path to
 *   the wrong file, and quietly playing the wrong file is worse than skipping.
 *
 * M3U_FILE_MAX — the hard ceiling on bytes pulled from the stream. At the
 *   realistic ~80 bytes per line that is ~13000 lines, far beyond any playlist
 *   a 320x240 screen can browse, and it means a corrupt directory entry
 *   claiming a 4 GB size costs a bounded read instead of a hang.
 */
#define M3U_PATH_MAX   191u
#define M3U_TITLE_MAX  64u
#define M3U_LINE_MAX   512u
#define M3U_FILE_MAX   (1024u * 1024u)

/* Size of the stream read window inside m3u_scratch_t. One 512-byte block:
 * fat32_stream_read already reads through the FAT/data sector caches, so a
 * bigger window buys nothing but scratch. */
#define M3U_IO_CHUNK   512u

/*
 * Return codes. Same numbering as fat32.h on purpose — a caller that chains
 * fat32_open() into m3u_parse_file() gets one consistent negative space
 * instead of two overlapping ones.
 */
#define M3U_OK         0
#define M3U_EIO        (-2)   /* the disk / source callback reported an error */
#define M3U_EINVAL     (-4)   /* null or absurd argument; nothing was parsed  */

/*
 * One parsed track reference.
 *
 * `path` is the ONLY thing a later resolver needs, and it is deliberately not
 * the raw text of the line. The raw text is ambiguous — it may be relative to
 * the playlist's own directory or to the volume root, may use '\' or '/', and
 * may contain '.' and '..' segments — and every consumer would have to redo
 * that reasoning, i.e. redo the security-relevant part, correctly, every time.
 * So it is done ONCE, here, and what comes out is canonical:
 *
 *   - relative to the VOLUME ROOT, with no leading separator
 *   - '/' separated, always
 *   - no '.' or '..' segments remain; they are applied, not carried
 *   - guaranteed never to escape the root (an entry whose '..' would have
 *     escaped is REJECTED, not clamped — see m3u_result_t.skipped_escape)
 *
 * So "Music/Rock/01 Song.mp3" — exactly the segment list a resolver feeds to
 * fat32_open_in() one directory at a time. It is NUL-terminated and never
 * empty for a returned entry.
 *
 * `title` and `duration_s` come from the #EXTINF that preceded this line, if
 * there was one: title is "" and duration_s is -1 when absent or unparseable
 * (-1 is also what a well-formed "#EXTINF:-1,Stream" means, i.e. "unknown",
 * so the two are correctly indistinguishable to a caller that just wants to
 * know whether it can display a length).
 */
typedef struct {
    char    path[M3U_PATH_MAX + 1];    /* canonical, root-relative, '/'-sep   */
    char    title[M3U_TITLE_MAX + 1];  /* #EXTINF display title, "" if absent */
    int32_t duration_s;                /* #EXTINF seconds, -1 if absent/unknown */
} m3u_entry_t;

/*
 * Caller-provided working memory. One instance can serve every parse in the
 * firmware; it holds no state between calls (m3u_parse* initialises it).
 * Passed by pointer rather than allocated here so that the ~1.1 KB is visible
 * at the call site and lands in the caller's chosen storage — this device has
 * no heap for it and the .bss budget is already 81% spent.
 */
typedef struct {
    uint8_t io[M3U_IO_CHUNK];        /* stream read window                    */
    char    line[M3U_LINE_MAX + 1];  /* the logical line being assembled      */
    char    title[M3U_TITLE_MAX + 1];/* pending #EXTINF title                 */
    char    base[M3U_PATH_MAX + 1];  /* base_dir, normalised once per parse   */
    char    work[M3U_PATH_MAX + 1];  /* path resolution scratch               */
    int32_t dur;                     /* pending #EXTINF duration              */
    uint32_t line_len;               /* bytes in line[]                       */
    uint8_t  line_over;              /* current line already blew M3U_LINE_MAX */
    uint8_t  have_ext;               /* a pending #EXTINF applies to next path */
} m3u_scratch_t;

/*
 * What the parse found. Everything here is a COUNT, not a flag-you-can-miss,
 * so a caller can show the user "42 tracks (3 skipped)" rather than pretending
 * a damaged playlist was fine.
 */
typedef struct {
    uint32_t count;          /* entries actually written to out[]             */
    uint32_t total;          /* track lines that RESOLVED (rejects are in the
                              * skipped_* counters, not here). total > count
                              * exactly when the output array ran out.        */
    uint32_t bytes;          /* bytes consumed from the source                */
    uint32_t skipped_long;   /* dropped: line or resolved path over its cap   */
    uint32_t skipped_escape; /* dropped: '..' walked off the volume root      */
    uint32_t skipped_bad;    /* dropped: NUL/control bytes, URL scheme, or a
                              * path that resolved to nothing at all          */
    uint8_t  truncated;      /* 1: out_max was reached, entries were dropped  */
    uint8_t  file_truncated; /* 1: input hit M3U_FILE_MAX, tail never read    */
    uint8_t  had_extm3u;     /* 1: the file opened with #EXTM3U               */
    uint8_t  had_bom;        /* 1: a UTF-8 BOM was stripped                   */
} m3u_result_t;

/*
 * Byte source. Fill up to `len` bytes into `buf`; return how many were
 * written (0 = end of input) or negative on an unrecoverable read error.
 * A short non-zero return is fine and does NOT mean end of input.
 *
 * Why an indirection instead of taking fat32_stream_t directly: it makes the
 * parser testable against raw bytes with no disk image in the loop, which is
 * the difference between "we have a dozen malformed-input tests" and "we have
 * a Python script that builds a dozen malformed FAT32 volumes". The on-disk
 * case is m3u_parse_file() below, which is a four-line adapter over it.
 */
typedef int32_t (*m3u_src_fn)(void *ud, void *buf, uint32_t len);

/*
 * Parse a playlist from an arbitrary byte source.
 *
 * base_dir  the directory the playlist file itself lives in, as a canonical
 *           root-relative '/'-separated path with no leading or trailing
 *           slash ("Music/Rock"), or "" / NULL for the volume root. This is
 *           what a relative entry is relative to. It is normalised on the way
 *           in, so a caller that passes "\Music\Rock\" is not punished.
 * out       caller's array, out_max entries. May be NULL only if out_max is 0
 *           (a counting pass: res->total still comes back correct).
 * scratch   caller's working memory; contents on entry are irrelevant.
 * res       filled in on every return path, including the error ones, so a
 *           caller can always report what happened. Must not be NULL.
 *
 * Returns M3U_OK even for a file that was empty, all comments, or entirely
 * garbage — "nothing playable in it" is a RESULT, not an error, and the
 * counters in *res say which it was. Only a null argument (M3U_EINVAL) or a
 * source that reported a read error (M3U_EIO) is a failure return, and even
 * then any entries parsed before the failure are valid and counted.
 */
int m3u_parse(m3u_src_fn src, void *ud, const char *base_dir,
              m3u_entry_t *out, uint32_t out_max,
              m3u_scratch_t *scratch, m3u_result_t *res);

/*
 * Parse a playlist that lives on the mounted volume: the same parse, with
 * fat32_stream_read as the source. Open the file with fat32_open/_open_in to
 * get (first_clus, size), pass the directory you found it in as base_dir.
 *
 * `size` is the directory entry's claim about the file and is NOT trusted:
 * the read is bounded by M3U_FILE_MAX as well, and res->file_truncated says
 * whether that bound was the one that stopped us.
 */
int m3u_parse_file(fat32_t *fs, uint32_t first_clus, uint32_t size,
                   const char *base_dir,
                   m3u_entry_t *out, uint32_t out_max,
                   m3u_scratch_t *scratch, m3u_result_t *res);

#endif /* CORE_FS_M3U_H */
