/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/codecs/flac_meta.c — FLAC metadata-block parser (see flac_meta.h).
 *
 * Cleanroom implementation from the public FLAC format spec. We do NOT lean
 * on dr_flac here: parsing the handful of metadata blocks ourselves is both
 * cheaper (no decoder state, no allocation) and cleaner. The format:
 *
 *   "fLaC"                                    (4-byte magic)
 *   repeat:
 *     block header (4 bytes):
 *       byte0 bit7      = last-block flag
 *       byte0 bits6..0  = block type (0 STREAMINFO, 4 VORBIS_COMMENT, ...)
 *       bytes1..3       = 24-bit BIG-endian block length
 *     block body (length bytes)
 *   until the last-block flag is set.
 *
 * STREAMINFO body (34 bytes), the fields we need live in a 64-bit big-endian
 * word at byte offset 10:
 *       bits 63..44 = sample_rate   (20 bits)
 *       bits 43..41 = channels - 1  (3 bits)
 *       bits 40..36 = bps - 1       (5 bits)
 *       bits 35..0  = total_samples (36 bits)
 *
 * VORBIS_COMMENT body is LITTLE-endian:
 *       u32 vendor_length, vendor[vendor_length],
 *       u32 comment_count,
 *       repeat comment_count: u32 length, "KEY=value"[length] (UTF-8).
 *
 * Everything is bounded: a capped block count, capped comment count, capped
 * per-comment length, and reads clamped to the declared block length, so a
 * malformed or hostile file can consume bytes but never run away.
 */

#include "flac_meta.h"

/* Bounds — generous enough for real files, tight enough to fence off junk. */
enum {
    FM_MAX_BLOCKS   = 64,     /* metadata blocks to walk before giving up   */
    FM_MAX_COMMENTS = 256,    /* VORBIS_COMMENT entries to parse            */
    FM_COMMENT_CAP  = 512,    /* bytes of any one comment we read+inspect   */
    FM_STREAMINFO_LEN = 34,
};

/* -------- source helpers ------------------------------------------------- */

/* Read exactly n bytes into buf. Returns 1 on success, 0 on short read. */
static int read_full(decoder_source_t *src, void *buf, uint32_t n)
{
    uint8_t *p = (uint8_t *)buf;
    uint32_t done = 0;
    while (done < n) {
        size_t got = src->read(src->userdata, p + done, n - done);
        if (got == 0) {
            return 0;
        }
        done += (uint32_t)got;
    }
    return 1;
}

/* Skip n bytes forward. Prefer a relative seek; fall back to read+discard for
 * sources that can't seek. Returns 1 on success. */
static int skip_bytes(decoder_source_t *src, uint32_t n)
{
    if (n == 0) {
        return 1;
    }
    if (src->seek && src->seek(src->userdata, (int)n, DECODER_SEEK_CUR)) {
        return 1;
    }
    uint8_t tmp[128];
    while (n > 0) {
        uint32_t chunk = (n < sizeof tmp) ? n : (uint32_t)sizeof tmp;
        if (!read_full(src, tmp, chunk)) {
            return 0;
        }
        n -= chunk;
    }
    return 1;
}

/* -------- string / number helpers --------------------------------------- */

/* Copy printable ASCII (0x20..0x7E) from src[0..len) into a bounded, NUL-
 * terminated dst of `cap` bytes (cap includes the NUL). Drops UTF-8 multibyte
 * and control bytes — matches the atlas font coverage (main.c copy_display_name). */
static void copy_printable(char *dst, uint32_t cap, const uint8_t *src, uint32_t len)
{
    uint32_t i = 0;
    for (uint32_t j = 0; j < len && i + 1 < cap; j++) {
        uint8_t c = src[j];
        if (c >= 0x20 && c <= 0x7E) {
            dst[i++] = (char)c;
        }
    }
    dst[i] = '\0';
}

/* Parse the first run of decimal digits in src[0..len) as a non-negative int
 * (bounded so it can't overflow wildly). Skips any leading non-digits, so
 * "2021-05-01" -> 2021 and "3/12" -> 3. Returns 0 if no digits. */
static int parse_leading_int(const uint8_t *src, uint32_t len)
{
    uint32_t j = 0;
    while (j < len && (src[j] < '0' || src[j] > '9')) {
        j++;
    }
    int v = 0, any = 0;
    while (j < len && src[j] >= '0' && src[j] <= '9') {
        if (v < 100000000) {               /* clamp — years/tracks are small */
            v = v * 10 + (src[j] - '0');
        }
        any = 1;
        j++;
    }
    return any ? v : 0;
}

/*
 * Parse a ReplayGain value — "-7.28 dB", "+3.5 dB", "0.00" — into 1/256 dB.
 * Sign, integer part, then up to 2 fraction digits; anything after (the " dB"
 * suffix, extra digits) is ignored. Returns 1 when a number was found.
 */
static int parse_gain_q8(const uint8_t *src, uint32_t len, int *out)
{
    uint32_t j = 0;
    int      neg = 0;

    while (j < len && (src[j] == ' ' || src[j] == '\t')) {
        j++;
    }
    if (j < len && (src[j] == '+' || src[j] == '-')) {
        neg = (src[j] == '-');
        j++;
    }
    int whole = 0, any = 0;
    while (j < len && src[j] >= '0' && src[j] <= '9') {
        if (whole < 1000) {                /* clamp — real gains are within ±60 dB */
            whole = whole * 10 + (src[j] - '0');
        }
        any = 1;
        j++;
    }
    int frac_hundredths = 0;
    if (j < len && src[j] == '.') {
        j++;
        for (int d = 0; d < 2; d++) {
            int digit = (j < len && src[j] >= '0' && src[j] <= '9') ? (src[j++] - '0') : 0;
            frac_hundredths = frac_hundredths * 10 + digit;
            any = 1;
        }
    }
    if (!any) {
        return 0;
    }
    /* q8 dB = (whole*100 + hundredths) * 256 / 100, rounded. */
    int total = whole * 100 + frac_hundredths;
    int q8    = (total * 256 + 50) / 100;
    *out = neg ? -q8 : q8;
    return 1;
}

/* Case-insensitive match of key[0..klen) against a NUL-terminated ASCII name. */
static int key_is(const uint8_t *key, uint32_t klen, const char *name)
{
    uint32_t i = 0;
    for (; i < klen && name[i]; i++) {
        uint8_t a = key[i];
        char    b = name[i];
        if (a >= 'A' && a <= 'Z') a = (uint8_t)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if ((char)a != b) {
            return 0;
        }
    }
    return i == klen && name[i] == '\0';
}

/* -------- VORBIS_COMMENT ------------------------------------------------- */

/* Interpret one "KEY=value" comment and route the value into *out. */
static void apply_comment(flac_meta_t *out, const uint8_t *c, uint32_t len)
{
    uint32_t eq = 0;
    while (eq < len && c[eq] != '=') {
        eq++;
    }
    if (eq >= len) {
        return;                            /* no '=' — not a tag */
    }
    const uint8_t *key = c;
    uint32_t       klen = eq;
    const uint8_t *val = c + eq + 1;
    uint32_t       vlen = len - eq - 1;

    if (key_is(key, klen, "TITLE")) {
        copy_printable(out->title, sizeof out->title, val, vlen);
    } else if (key_is(key, klen, "ARTIST")) {
        copy_printable(out->artist, sizeof out->artist, val, vlen);
    } else if (key_is(key, klen, "ALBUMARTIST") ||
               key_is(key, klen, "ALBUM ARTIST")) {
        /* Fallback source for artist — only fill if ARTIST hasn't already. */
        if (out->artist[0] == '\0') {
            copy_printable(out->artist, sizeof out->artist, val, vlen);
        }
    } else if (key_is(key, klen, "ALBUM")) {
        copy_printable(out->album, sizeof out->album, val, vlen);
    } else if (key_is(key, klen, "GENRE")) {
        copy_printable(out->genre, sizeof out->genre, val, vlen);
    } else if (key_is(key, klen, "TRACKNUMBER") ||
               key_is(key, klen, "TRACK")) {
        out->track = parse_leading_int(val, vlen);
    } else if (key_is(key, klen, "DATE") ||
               key_is(key, klen, "YEAR")) {
        int y = parse_leading_int(val, vlen);
        if (y > 0) {
            out->year = y;
        }
    } else if (key_is(key, klen, "REPLAYGAIN_TRACK_GAIN")) {
        if (parse_gain_q8(val, vlen, &out->rg_track_q8)) {
            out->have_rg |= FLAC_META_RG_TRACK;
        }
    } else if (key_is(key, klen, "REPLAYGAIN_ALBUM_GAIN")) {
        if (parse_gain_q8(val, vlen, &out->rg_album_q8)) {
            out->have_rg |= FLAC_META_RG_ALBUM;
        }
    }
}

/* Read a little-endian u32 from a 4-byte buffer. */
static uint32_t le32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/* Parse a VORBIS_COMMENT block body of `block_len` bytes, consuming exactly
 * that many bytes from the source. `remain` tracks bytes still owed to the
 * block; on any inconsistency we skip the rest and return. */
static void parse_vorbis_comment(decoder_source_t *src, flac_meta_t *out,
                                 uint32_t block_len)
{
    uint32_t remain = block_len;
    uint8_t  u32[4];

    /* vendor_length + vendor string (we don't keep the vendor). */
    if (remain < 4 || !read_full(src, u32, 4)) {
        goto drain;
    }
    remain -= 4;
    uint32_t vendor_len = le32(u32);
    if (vendor_len > remain) {
        goto drain;                        /* lies about its own size */
    }
    if (!skip_bytes(src, vendor_len)) {
        return;
    }
    remain -= vendor_len;

    /* comment_count. */
    if (remain < 4 || !read_full(src, u32, 4)) {
        goto drain;
    }
    remain -= 4;
    uint32_t count = le32(u32);
    if (count > FM_MAX_COMMENTS) {
        count = FM_MAX_COMMENTS;           /* cap — ignore the surplus */
    }

    for (uint32_t i = 0; i < count; i++) {
        if (remain < 4 || !read_full(src, u32, 4)) {
            goto drain;
        }
        remain -= 4;
        uint32_t clen = le32(u32);
        if (clen > remain) {
            goto drain;
        }
        remain -= clen;

        uint8_t  buf[FM_COMMENT_CAP];
        uint32_t take = (clen < FM_COMMENT_CAP) ? clen : FM_COMMENT_CAP;
        if (!read_full(src, buf, take)) {
            return;
        }
        if (clen > take && !skip_bytes(src, clen - take)) {
            return;                        /* tail of an oversized comment */
        }
        apply_comment(out, buf, take);
    }

drain:
    skip_bytes(src, remain);               /* leave the source block-aligned */
}

/* -------- STREAMINFO ----------------------------------------------------- */

static void parse_streaminfo(flac_meta_t *out, const uint8_t *b)
{
    /* 64-bit big-endian word at byte 10 (see header comment for the layout). */
    uint32_t sr = ((uint32_t)b[10] << 12) |
                  ((uint32_t)b[11] << 4)  |
                  ((uint32_t)b[12] >> 4);
    uint64_t total = ((uint64_t)(b[13] & 0x0F) << 32) |
                     ((uint64_t)b[14] << 24) |
                     ((uint64_t)b[15] << 16) |
                     ((uint64_t)b[16] << 8)  |
                     (uint64_t)b[17];

    out->sample_rate = sr;
    if (sr != 0 && total != 0) {
        /* Round to nearest whole second for display. */
        out->duration_s = (uint32_t)((total + (sr / 2)) / sr);
    }
}

/* -------- top-level parse ------------------------------------------------ */

int flac_meta_read(decoder_source_t *src, flac_meta_t *out)
{
    /* Zero out — the "absent tag" state is all-zero / empty strings. */
    uint8_t *z = (uint8_t *)out;
    for (uint32_t i = 0; i < sizeof *out; i++) {
        z[i] = 0;
    }

    if (!src || !src->read) {
        return -1;
    }

    uint8_t magic[4];
    if (!read_full(src, magic, 4)) {
        return -1;
    }
    if (magic[0] != 'f' || magic[1] != 'L' ||
        magic[2] != 'a' || magic[3] != 'C') {
        return -1;
    }

    for (int block = 0; block < FM_MAX_BLOCKS; block++) {
        uint8_t hdr[4];
        if (!read_full(src, hdr, 4)) {
            break;
        }
        int      last = (hdr[0] & 0x80) != 0;
        int      type = hdr[0] & 0x7F;
        uint32_t len  = ((uint32_t)hdr[1] << 16) |
                        ((uint32_t)hdr[2] << 8)  |
                        (uint32_t)hdr[3];

        if (type == 0) {                   /* STREAMINFO */
            uint8_t si[FM_STREAMINFO_LEN];
            if (len < FM_STREAMINFO_LEN || !read_full(src, si, FM_STREAMINFO_LEN)) {
                break;
            }
            parse_streaminfo(out, si);
            out->have = 1;
            if (!skip_bytes(src, len - FM_STREAMINFO_LEN)) {
                break;
            }
        } else if (type == 3) {            /* SEEKTABLE — count only, then skip */
            out->seek_points = len / 18u;  /* 18 bytes per seekpoint */
            if (!skip_bytes(src, len)) {
                break;
            }
        } else if (type == 4) {            /* VORBIS_COMMENT */
            parse_vorbis_comment(src, out, len);
        } else {                           /* anything else — skip the body */
            if (!skip_bytes(src, len)) {
                break;
            }
        }

        if (last) {
            break;
        }
    }

    return out->have ? 0 : -1;
}
