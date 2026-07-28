/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/fs/m3u.c — M3U8 (UTF-8) playlist reader (see m3u.h for the API, the
 * caps and the memory budget).
 *
 * Portable: no hardware access, no libc, no allocation, no statics. The one
 * disk-facing entry point (m3u_parse_file) is a four-line adapter that turns
 * fat32_stream_read into the generic byte source everything else runs on.
 *
 * The rules this file enforces everywhere, because it is parsing a file some
 * other machine wrote and this one has no watchdog and no MMU:
 *   - EVERY loop has a bound that does not depend on the input's contents.
 *     The byte loop is bounded by M3U_FILE_MAX, the line loop by
 *     M3U_LINE_MAX, the segment loop by the line it walks, and there is no
 *     recursion anywhere.
 *   - EVERY write into a fixed buffer is bounds-checked at the write, not
 *     argued to be safe from somewhere else in the file.
 *   - EVERY dropped input is COUNTED. See the m3u_result_t skipped_* fields;
 *     nothing here fails silently, because a playlist that quietly lost a
 *     third of its tracks looks exactly like a parser bug.
 */

#include "m3u.h"

/* memcpy: lib/mem.c's word-optimised one on bare metal, libc's in the host
 * tests. Same declaration either way (as in fat32.c). */
#include "../lib/mem.h"

/* ---- tiny freestanding helpers --------------------------------------- */

/*
 * strlen, bounded. There is no libc here, and an unbounded strlen over a
 * caller-supplied base_dir is exactly the kind of "it'll be NUL-terminated"
 * assumption that turns a bad argument into a wild read. `cap` is the most we
 * will ever look at; a longer string reports `cap` and is then rejected by
 * the length checks downstream.
 */
static uint32_t slen_n(const char *s, uint32_t cap)
{
    uint32_t i = 0;
    while (i < cap && s[i] != '\0') {
        i++;
    }
    return i;
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Case-insensitive ASCII prefix test. Directives are conventionally upper
 * case ("#EXTINF:") but plenty of writers emit "#extinf:"; the tag is ASCII
 * either way, so folding case here costs nothing and accepts both. */
static int starts_with_ci(const char *s, uint32_t n, const char *pre)
{
    uint32_t i = 0;
    while (pre[i] != '\0') {
        if (i >= n || lower(s[i]) != lower(pre[i])) {
            return 0;
        }
        i++;
    }
    return 1;
}

/* Space or tab. Deliberately NOT a general isspace(): CR and LF are line
 * terminators handled by the byte loop, and every other control byte is
 * treated as corruption rather than as whitespace to be politely trimmed. */
static int is_blank(char c)
{
    return c == ' ' || c == '\t';
}

static int is_sep(char c)
{
    return c == '/' || c == '\\';
}

/*
 * Length of the UTF-8 sequence starting at s[i], or 0 if s[i] does not begin
 * a well-formed sequence that fits entirely within n.
 *
 * This exists for ONE reason: truncating a title must never leave a partial
 * sequence at the end of the string. A dangling lead byte is not just an ugly
 * glyph — it invites every downstream decoder to read past it looking for the
 * continuation bytes that are not there. So the title copy advances only over
 * sequences this function has already confirmed are complete and in range.
 * Overlong forms and surrogates are NOT rejected; we are a byte pipe for the
 * name, not a UTF-8 validator, and the font layer already handles unknown
 * codepoints. What we guarantee is framing, not validity.
 */
static uint32_t utf8_seq_len(const char *s, uint32_t i, uint32_t n)
{
    uint8_t b = (uint8_t)s[i];
    uint32_t need;

    if (b < 0x80u) {
        return 1;                       /* ASCII */
    }
    if (b >= 0xC2u && b <= 0xDFu) {
        need = 2;
    } else if (b >= 0xE0u && b <= 0xEFu) {
        need = 3;
    } else if (b >= 0xF0u && b <= 0xF4u) {
        need = 4;
    } else {
        return 0;                       /* continuation or C0/C1: not a lead */
    }
    if (i + need > n) {
        return 0;                       /* runs off the end of the span */
    }
    for (uint32_t k = 1; k < need; k++) {
        if (((uint8_t)s[i + k] & 0xC0u) != 0x80u) {
            return 0;                   /* missing continuation byte */
        }
    }
    return need;
}

/*
 * Copy up to `cap` BYTES of `src` (length n) into dst, cutting only on a UTF-8
 * character boundary, and NUL-terminate. Any byte that does not begin a
 * complete sequence ends the copy — so a title whose tail was chopped by
 * M3U_LINE_MAX comes out short rather than malformed. dst must hold cap + 1.
 */
static void copy_utf8(char *dst, uint32_t cap, const char *src, uint32_t n)
{
    uint32_t i = 0;

    while (i < n) {
        uint32_t len = utf8_seq_len(src, i, n);
        if (len == 0 || i + len > cap) {
            break;
        }
        i += len;
    }
    if (i > 0) {
        memcpy(dst, src, i);
    }
    dst[i] = '\0';
}

/* ---- path resolution -------------------------------------------------- */

/*
 * Errors from path_build(). Negative so a caller can test `< 0`; the exact
 * value picks which m3u_result_t counter gets bumped, which is the whole
 * point — the user is told WHY a track vanished.
 */
#define PB_TOOLONG (-1)
#define PB_ESCAPE  (-2)
#define PB_BAD     (-3)

/*
 * Resolve one playlist line into a canonical, volume-root-relative path.
 *
 * This is the security-relevant function in the module, so the rules are
 * spelled out rather than implied:
 *
 *  ABSOLUTE vs RELATIVE. A leading '/' or '\' means "from the volume root" —
 *    there is exactly one volume, so there is nothing else it could mean. A
 *    leading drive letter ("D:\Music\x.mp3") is ALSO the volume root: that is
 *    what a Windows player writes when the iPod is mounted as D:, and the
 *    drive letter it recorded has no meaning on the device. Stripping it is
 *    the only interpretation that plays the user's music. Everything else is
 *    relative to `base`, the directory the playlist file itself lives in.
 *
 *  SEPARATORS. '/' and '\' are both accepted, on every line, mixed freely,
 *    because real playlists contain both — the same file gets edited on a
 *    desktop and appended to by a player. FAT32 allows neither character in a
 *    name, so treating both as separators cannot corrupt a legal name.
 *
 *  COLONS. At most one ':' is allowed and only at index 1, i.e. only as a
 *    drive letter. That single rule rejects "http://host/x.mp3" and every
 *    other URL scheme (their colon is at index >= 2) without needing a list
 *    of schemes, and it also rejects a colon buried in a name — which FAT32
 *    cannot store anyway, so such a line can only be a mistake or an attack.
 *
 *  DOT SEGMENTS. "." is dropped. ".." pops the last segment already built.
 *    A ".." with nothing left to pop would name something OUTSIDE the volume
 *    root; we REJECT the entry (PB_ESCAPE) rather than clamp it to the root.
 *    Clamping is the traditional choice and it is wrong here: it silently
 *    turns "../../etc/passwd" into "etc/passwd" and plays whatever that is.
 *    Rejecting means the user sees a skipped track, which is honest.
 *
 *  CONTROL BYTES. Any byte below 0x20 rejects the line (PB_BAD). FAT32 forbids
 *    them in names, so their presence means the "line" is really a chunk of a
 *    binary file that happened to lie between two 0x0A bytes.
 *
 * On success returns the length written to dst (always > 0, NUL-terminated,
 * '/'-separated, no leading or trailing separator). `base` must already be in
 * that same canonical form ("" for the root).
 */
static int32_t path_build(char *dst, uint32_t cap,
                          const char *base, const char *rel, uint32_t rel_len)
{
    uint32_t dlen = 0;
    uint32_t i    = 0;
    int      absolute = 0;

    /* Reject control bytes up front: one pass, before any interpretation, so
     * nothing downstream has to wonder whether a NUL is hiding in the span. */
    for (uint32_t k = 0; k < rel_len; k++) {
        if ((uint8_t)rel[k] < 0x20u) {
            return PB_BAD;
        }
        /* Colon: legal only as the drive-letter separator at index 1. */
        if (rel[k] == ':' && k != 1) {
            return PB_BAD;
        }
    }

    if (rel_len >= 2 && rel[1] == ':') {
        char d = lower(rel[0]);
        if (d < 'a' || d > 'z') {
            return PB_BAD;              /* ":" or "1:" — not a drive letter */
        }
        i = 2;
        absolute = 1;
    }
    if (i < rel_len && is_sep(rel[i])) {
        absolute = 1;                   /* "/x", "\x", or "D:\x" */
    }
    while (i < rel_len && is_sep(rel[i])) {
        i++;                            /* eat the leading separator run */
    }

    if (!absolute) {
        /* Relative: start from the playlist's own directory. base is already
         * canonical and already known to fit, but bounds-check anyway — this
         * is the one place a caller-supplied string enters the buffer. */
        uint32_t blen = slen_n(base, cap + 1u);
        if (blen > cap) {
            return PB_TOOLONG;
        }
        if (blen > 0) {
            memcpy(dst, base, blen);
        }
        dlen = blen;
    }

    /* Segment loop. Bounded by rel_len: every iteration consumes at least one
     * byte of `rel` (the separator run below always advances past the segment
     * it just read), so this cannot spin. */
    while (i < rel_len) {
        uint32_t start = i;
        uint32_t seg_len;

        while (i < rel_len && !is_sep(rel[i])) {
            i++;
        }
        seg_len = i - start;
        while (i < rel_len && is_sep(rel[i])) {
            i++;                        /* collapse "a//b" to "a/b" */
        }

        if (seg_len == 0) {
            continue;                   /* leading/duplicate separator */
        }
        if (seg_len == 1 && rel[start] == '.') {
            continue;                   /* "." is a no-op */
        }
        if (seg_len == 2 && rel[start] == '.' && rel[start + 1] == '.') {
            if (dlen == 0) {
                return PB_ESCAPE;       /* already at the root: no parent */
            }
            while (dlen > 0 && dst[dlen - 1] != '/') {
                dlen--;
            }
            if (dlen > 0) {
                dlen--;                 /* drop the '/' too */
            }
            continue;
        }

        /* A real segment. Space for the separator + the segment + the NUL. */
        {
            uint32_t need = seg_len + (dlen > 0 ? 1u : 0u);
            if (dlen + need > cap) {
                return PB_TOOLONG;
            }
            if (dlen > 0) {
                dst[dlen++] = '/';
            }
            memcpy(&dst[dlen], &rel[start], seg_len);
            dlen += seg_len;
        }
    }

    dst[dlen] = '\0';
    if (dlen == 0) {
        /* The line named the volume root itself ("/", ".", "\\") — a
         * directory, not a track. Nothing to play. */
        return PB_BAD;
    }
    return (int32_t)dlen;
}

/* ---- #EXTINF ---------------------------------------------------------- */

/*
 * Parse "#EXTINF:<seconds>,<title>" into the pending slot in `sc`.
 *
 * The real-world shape is looser than the name suggests. Seconds may be
 * negative ("-1" = unknown, used for streams), may be fractional
 * ("212.386"), and may be followed by key="value" attributes before the
 * comma (the IPTV dialect). All of that is handled by the same two rules:
 * take the leading signed integer, and take the title as everything after the
 * FIRST comma. A fractional part is discarded rather than rounded — the
 * display shows whole seconds and half a second of drift is invisible.
 *
 * Digits are capped at 9 so the accumulator cannot overflow int32 no matter
 * what the file says; a longer run is treated as "unknown" rather than
 * wrapped into some arbitrary small number.
 */
static void parse_extinf(m3u_scratch_t *sc, const char *s, uint32_t n)
{
    uint32_t i   = 8;                   /* past "#EXTINF:" */
    int      neg = 0;
    int32_t  val = 0;
    uint32_t digits = 0;
    uint32_t comma;

    while (i < n && is_blank(s[i])) {
        i++;
    }
    if (i < n && (s[i] == '-' || s[i] == '+')) {
        neg = (s[i] == '-');
        i++;
    }
    while (i < n && s[i] >= '0' && s[i] <= '9' && digits < 9u) {
        val = val * 10 + (s[i] - '0');
        digits++;
        i++;
    }
    if (digits == 0 || (i < n && s[i] >= '0' && s[i] <= '9')) {
        val = -1;                       /* absent, or an implausible run */
    } else if (neg) {
        val = -val;
    }
    sc->dur = (val < 0) ? -1 : val;

    /* Title: after the first comma anywhere in the line. */
    sc->title[0] = '\0';
    for (comma = 8; comma < n; comma++) {
        if (s[comma] == ',') {
            break;
        }
    }
    if (comma < n) {
        uint32_t ts = comma + 1u;
        uint32_t te = n;
        while (ts < te && is_blank(s[ts])) {
            ts++;
        }
        while (te > ts && is_blank(s[te - 1])) {
            te--;
        }
        /* Cut at the first control byte: a title is display text, and a stray
         * NUL or ESC in it is corruption, not content. */
        for (uint32_t k = ts; k < te; k++) {
            if ((uint8_t)s[k] < 0x20u) {
                te = k;
                break;
            }
        }
        copy_utf8(sc->title, M3U_TITLE_MAX, &s[ts], te - ts);
    }
    sc->have_ext = 1;
}

/* ---- the line handler ------------------------------------------------- */

typedef struct {
    m3u_entry_t  *out;
    uint32_t      out_max;
    m3u_scratch_t *sc;
    m3u_result_t *res;
} m3u_ctx_t;

/*
 * Consume one complete logical line (already stripped of its terminator).
 * `over` is set when the line blew M3U_LINE_MAX and what we hold is only its
 * first M3U_LINE_MAX bytes.
 */
static void handle_line(m3u_ctx_t *cx, char *s, uint32_t n, int over)
{
    m3u_scratch_t *sc  = cx->sc;
    m3u_result_t  *res = cx->res;
    uint32_t start = 0;
    int32_t  plen;

    while (start < n && is_blank(s[start])) {
        start++;
    }
    while (n > start && is_blank(s[n - 1])) {
        n--;
    }
    s += start;
    n -= start;

    if (n == 0) {
        /* Blank line. Note it does NOT clear a pending #EXTINF: writers do
         * put a blank line between the tag and its path, and dropping the
         * title there would lose data for a purely cosmetic reason. */
        return;
    }

    if (s[0] == '#') {
        if (over) {
            return;                     /* an over-long comment is still a
                                         * comment; nothing was lost that we
                                         * were going to use */
        }
        if (starts_with_ci(s, n, "#EXTM3U")) {
            res->had_extm3u = 1;
        } else if (starts_with_ci(s, n, "#EXTINF:")) {
            parse_extinf(sc, s, n);
        }
        /* Every other directive (#PLAYLIST, #EXTGRP, #EXTALB, a plain
         * comment) is ignored on purpose, and ignored WITHOUT clearing the
         * pending #EXTINF — grouping tags legitimately sit between an EXTINF
         * and its path. */
        return;
    }

    /* A track line. Whatever happens to it, it consumes the pending #EXTINF:
     * a tag describes the next path and only the next path. Cleared here, at
     * the top, so that every early return below still clears it. */
    {
        int      have = sc->have_ext;
        int32_t  dur  = sc->dur;
        sc->have_ext = 0;

        if (over) {
            /* Half a path is a path to the WRONG file. Drop it and say so. */
            res->skipped_long++;
            return;
        }

        plen = path_build(sc->work, M3U_PATH_MAX, sc->base, s, n);
        if (plen < 0) {
            if (plen == PB_TOOLONG) {
                res->skipped_long++;
            } else if (plen == PB_ESCAPE) {
                res->skipped_escape++;
            } else {
                res->skipped_bad++;
            }
            return;
        }

        res->total++;
        if (res->count >= cx->out_max) {
            res->truncated = 1;         /* keep counting: res->total tells the
                                         * caller how many it did not get */
            return;
        }

        {
            m3u_entry_t *e = &cx->out[res->count];
            memcpy(e->path, sc->work, (uint32_t)plen + 1u);
            if (have) {
                uint32_t tl = slen_n(sc->title, M3U_TITLE_MAX);
                memcpy(e->title, sc->title, tl);
                e->title[tl] = '\0';
                e->duration_s = dur;
            } else {
                e->title[0]   = '\0';
                e->duration_s = -1;
            }
            res->count++;
        }
    }
}

/* ---- the parse -------------------------------------------------------- */

/* UTF-8 byte-order mark. Notepad and a good many exporters emit one; it is
 * not part of the first path and must not become part of the first name. */
static const uint8_t k_bom[3] = { 0xEFu, 0xBBu, 0xBFu };

/* Append one byte to the line being assembled. Silently dropping the overflow
 * is safe ONLY because line_over is latched and handle_line refuses to emit
 * an entry for an over-long line. */
static void line_push(m3u_scratch_t *sc, char c)
{
    if (sc->line_over) {
        return;
    }
    if (sc->line_len >= M3U_LINE_MAX) {
        sc->line_over = 1;
        return;
    }
    sc->line[sc->line_len++] = c;
}

static void line_flush(m3u_ctx_t *cx)
{
    m3u_scratch_t *sc = cx->sc;

    sc->line[sc->line_len] = '\0';
    handle_line(cx, sc->line, sc->line_len, sc->line_over);
    sc->line_len  = 0;
    sc->line_over = 0;
}

int m3u_parse(m3u_src_fn src, void *ud, const char *base_dir,
              m3u_entry_t *out, uint32_t out_max,
              m3u_scratch_t *scratch, m3u_result_t *res)
{
    m3u_ctx_t cx;
    uint32_t  bom_n = 0;                /* candidate BOM bytes held back */
    int       bom_done = 0;
    int       rc = M3U_OK;

    if (!res) {
        return M3U_EINVAL;              /* nowhere to report anything */
    }
    memset(res, 0, sizeof *res);
    if (!src || !scratch || (out_max > 0 && !out)) {
        return M3U_EINVAL;
    }

    memset(scratch, 0, sizeof *scratch);
    scratch->dur = -1;

    /*
     * Normalise base_dir once. A caller that hands us "\Music\Rock\" or
     * "Music//Rock" gets the same answer as one that hands us "Music/Rock".
     * A base that is unusable (too long, or escaping) degrades to the volume
     * root rather than failing the whole parse: the paths in the file are
     * still worth reading, and a wrong base yields a track that does not
     * resolve later, which is visible, whereas refusing to open the playlist
     * at all is not diagnosable by the user.
     */
    scratch->base[0] = '\0';
    if (base_dir && base_dir[0] != '\0') {
        uint32_t bl = slen_n(base_dir, M3U_PATH_MAX + 1u);
        if (bl <= M3U_PATH_MAX) {
            if (path_build(scratch->base, M3U_PATH_MAX, "", base_dir, bl) < 0) {
                scratch->base[0] = '\0';
            }
        }
    }

    cx.out     = out;
    cx.out_max = out_max;
    cx.sc      = scratch;
    cx.res     = res;

    /*
     * Byte loop. Two nested loops, both bounded without reference to the
     * file's contents: the outer one stops when the source is exhausted or
     * when M3U_FILE_MAX bytes have been consumed (and `bytes` strictly
     * increases every iteration, since a zero-length read breaks out), the
     * inner one runs over a fixed-size window.
     */
    while (res->bytes < M3U_FILE_MAX) {
        uint32_t want = M3U_FILE_MAX - res->bytes;
        int32_t  got;

        if (want > M3U_IO_CHUNK) {
            want = M3U_IO_CHUNK;
        }
        got = src(ud, scratch->io, want);
        if (got < 0) {
            rc = M3U_EIO;
            break;                      /* entries parsed so far stay valid */
        }
        if (got == 0) {
            break;                      /* end of input */
        }
        if ((uint32_t)got > want) {
            got = (int32_t)want;        /* a misbehaving source cannot make us
                                         * read past our own buffer */
        }
        res->bytes += (uint32_t)got;

        for (uint32_t i = 0; i < (uint32_t)got; i++) {
            char c = (char)scratch->io[i];

            /*
             * BOM: only at offset 0, and only as all three bytes. Held back
             * byte by byte so it works even if the source hands us one byte
             * at a time; if the run breaks, the held bytes are replayed into
             * the line so a file that genuinely starts with 0xEF is intact.
             */
            if (!bom_done) {
                if ((uint8_t)c == k_bom[bom_n]) {
                    bom_n++;
                    if (bom_n == 3u) {
                        bom_done      = 1;
                        res->had_bom  = 1;
                    }
                    continue;
                }
                bom_done = 1;
                for (uint32_t k = 0; k < bom_n; k++) {
                    line_push(scratch, (char)k_bom[k]);
                }
                bom_n = 0;
            }

            /* Both CR and LF end a line. That covers LF, CRLF and lone-CR
             * files with no state at all: CRLF produces one line plus one
             * empty line, and empty lines are ignored anyway. */
            if (c == '\n' || c == '\r') {
                line_flush(&cx);
            } else {
                line_push(scratch, c);
            }
        }
    }

    if (res->bytes >= M3U_FILE_MAX) {
        /*
         * We stopped because of our own cap, not because the file ended. The
         * partial line in hand is NOT flushed: it may be the first half of a
         * path, and half a path names the wrong file.
         */
        res->file_truncated = 1;
        scratch->line_len   = 0;
        scratch->line_over  = 0;
    } else if (rc == M3U_OK && scratch->line_len > 0) {
        /* No trailing newline: the last line is still a line. */
        line_flush(&cx);
    }

    /* A bare BOM with nothing after it: nothing to replay, nothing to do. */
    return rc;
}

/* ---- the on-disk adapter ---------------------------------------------- */

typedef struct {
    fat32_stream_t st;
} m3u_fat_src_t;

static int32_t fat_src(void *ud, void *buf, uint32_t len)
{
    m3u_fat_src_t *s = (m3u_fat_src_t *)ud;
    return fat32_stream_read(&s->st, buf, len);   /* negative => M3U_EIO */
}

int m3u_parse_file(fat32_t *fs, uint32_t first_clus, uint32_t size,
                   const char *base_dir,
                   m3u_entry_t *out, uint32_t out_max,
                   m3u_scratch_t *scratch, m3u_result_t *res)
{
    m3u_fat_src_t s;

    if (!res) {
        return M3U_EINVAL;
    }
    if (!fs) {
        memset(res, 0, sizeof *res);
        return M3U_EINVAL;
    }
    /* `size` is the directory entry's claim and is not trusted: m3u_parse
     * bounds the read at M3U_FILE_MAX regardless, and the stream itself
     * validates every cluster it follows. */
    fat32_stream_open(&s.st, fs, first_clus, size);
    return m3u_parse(fat_src, &s, base_dir, out, out_max, scratch, res);
}
