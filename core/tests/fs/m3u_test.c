/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/fs/m3u_test.c — host test for the M3U8 playlist reader.
 *
 * The parser's whole reason to exist is that it eats a file WE did not write,
 * off a disk the user plugged into some other computer, on a device with no
 * watchdog. So this test is mostly a corpus of ways an .m3u8 goes wrong —
 * CRLF, a BOM, backslashes, "..", control bytes, lines longer than the line
 * cap, more tracks than the caller's array, a file with no end — and for each
 * one it asserts the PARSED VALUES and the reported counters, not merely that
 * nothing crashed. "Didn't crash" is what a silently-empty result looks like
 * too.
 *
 * Most cases run against a memory byte source (m3u_parse), which is exactly
 * why the parser takes a source callback: a dozen malformed playlists are a
 * dozen string literals here instead of a dozen synthetic FAT32 volumes. The
 * last section closes the loop with a hand-built in-RAM FAT32 image and
 * m3u_parse_file, so the on-disk adapter is covered too.
 */

#include "m3u.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_fails;

static int check(const char *label, int cond)
{
    printf("[%s] %s\n", label, cond ? "PASS" : "FAIL");
    if (!cond) {
        g_fails++;
    }
    return cond;
}

/* ---- memory byte source ---------------------------------------------- */

/*
 * Serves a fixed buffer. `chunk` caps how much one call returns, so the same
 * playlist can be fed as one block or one byte at a time — the second shape
 * is what proves the BOM detector and the line assembler survive a split at
 * an arbitrary offset. `fail_at` makes the source report a read error once
 * that many bytes have been served, standing in for a disk that dies mid-file.
 */
typedef struct {
    const uint8_t *p;
    uint32_t       n;
    uint32_t       off;
    uint32_t       chunk;     /* 0 = unlimited */
    uint32_t       fail_at;   /* 0 = never fail */
    uint32_t       calls;
} mem_src_t;

static int32_t mem_src(void *ud, void *buf, uint32_t len)
{
    mem_src_t *s = (mem_src_t *)ud;
    uint32_t   n;

    s->calls++;
    if (s->fail_at && s->off >= s->fail_at) {
        return -1;
    }
    if (s->chunk && len > s->chunk) {
        len = s->chunk;
    }
    n = s->n - s->off;
    if (n > len) {
        n = len;
    }
    if (n) {
        memcpy(buf, s->p + s->off, n);
        s->off += n;
    }
    return (int32_t)n;
}

/*
 * An INFINITE source: emits "a.mp3\n" forever and never returns 0. Nothing in
 * the file tells the parser to stop, so if M3U_FILE_MAX were not enforced this
 * test would hang instead of failing — which is the point. On the device that
 * same shape (a directory entry claiming a 4 GB size) is a battery-pull.
 */
static int32_t endless_src(void *ud, void *buf, uint32_t len)
{
    static const char pat[] = "a.mp3\n";
    uint32_t *phase = (uint32_t *)ud;
    uint8_t  *b = (uint8_t *)buf;

    for (uint32_t i = 0; i < len; i++) {
        b[i] = (uint8_t)pat[*phase % 6u];
        (*phase)++;
    }
    return (int32_t)len;
}

/* ---- shared fixtures -------------------------------------------------- */

#define OUT_MAX 16
static m3u_entry_t   g_out[OUT_MAX];
static m3u_scratch_t g_scratch;
static m3u_result_t  g_res;

/* Parse `text` (len bytes) with the given base_dir into g_out/g_res. */
static int parse_mem(const char *text, uint32_t len, const char *base,
                     uint32_t out_max, uint32_t chunk)
{
    mem_src_t src = { (const uint8_t *)text, len, 0, chunk, 0, 0 };
    return m3u_parse(mem_src, &src, base, g_out, out_max, &g_scratch, &g_res);
}

/* Same, for a NUL-terminated literal. */
static int parse_str(const char *text, const char *base)
{
    return parse_mem(text, (uint32_t)strlen(text), base, OUT_MAX, 0);
}

/* entry i's path == want */
static int path_is(uint32_t i, const char *want)
{
    return i < g_res.count && strcmp(g_out[i].path, want) == 0;
}

static void dump(const char *why)
{
    printf("  (%s) count=%u total=%u trunc=%u long=%u esc=%u bad=%u\n",
           why, g_res.count, g_res.total, g_res.truncated,
           g_res.skipped_long, g_res.skipped_escape, g_res.skipped_bad);
    for (uint32_t i = 0; i < g_res.count; i++) {
        printf("    [%u] '%s' title='%s' dur=%d\n",
               i, g_out[i].path, g_out[i].title, g_out[i].duration_s);
    }
}

/* ---- in-RAM FAT32 image (same trick as fat32_test.c) ------------------ */

#define MEM_BPS  512u
#define MEM_SECS 8u
static uint8_t g_mem[MEM_SECS * MEM_BPS];

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void put_dirent(uint8_t *e, const char *raw, uint8_t attr,
                       uint32_t clus, uint32_t size)
{
    memset(e, 0, 32);
    memcpy(e, raw, 11);
    e[11] = attr;
    put16(&e[20], (uint16_t)(clus >> 16));
    put16(&e[26], (uint16_t)(clus & 0xFFFF));
    put32(&e[28], size);
}

/* Root holds ROCK.M3U (cluster 3) whose bytes are `text`. BytesPerSec 512,
 * one sector per cluster, so cluster N is FS-sector N. */
static void build_playlist_image(const char *text, uint32_t len)
{
    memset(g_mem, 0, sizeof g_mem);

    uint8_t *bs = g_mem;
    bs[0] = 0xEB; bs[1] = 0x58; bs[2] = 0x90;
    memcpy(&bs[3], "MSDOS5.0", 8);
    put16(&bs[11], MEM_BPS);
    bs[13] = 1;                  /* SecPerClus */
    put16(&bs[14], 1);           /* RsvdSecCnt */
    bs[16] = 1;                  /* NumFATs    */
    bs[21] = 0xF8;
    put32(&bs[36], 1);           /* FATSz32    */
    put32(&bs[44], 2);           /* RootClus   */
    bs[510] = 0x55; bs[511] = 0xAA;

    uint8_t *fat = &g_mem[1 * MEM_BPS];
    put32(&fat[0 * 4], 0x0FFFFFF8u);
    put32(&fat[1 * 4], 0x0FFFFFFFu);
    put32(&fat[2 * 4], 0x0FFFFFFFu);   /* root */
    put32(&fat[3 * 4], 0x0FFFFFFFu);   /* ROCK.M3U */

    uint8_t *root = &g_mem[2 * MEM_BPS];
    put_dirent(&root[0], "ROCK    M3U", 0x20, 3, len);

    if (len > MEM_BPS) {
        len = MEM_BPS;
    }
    memcpy(&g_mem[3 * MEM_BPS], text, len);
}

static int mem_disk_read(void *ud, uint32_t lba, uint32_t count, void *buf)
{
    (void)ud;
    if ((lba + count) * 512u > sizeof g_mem) {
        return -1;
    }
    memcpy(buf, &g_mem[lba * 512u], count * 512u);
    return 0;
}

/* ---------------------------------------------------------------------- */

int main(void)
{
    /* ---- the sizes callers budget against --------------------------- */
    printf("sizeof(m3u_entry_t)=%u sizeof(m3u_scratch_t)=%u\n",
           (unsigned)sizeof(m3u_entry_t), (unsigned)sizeof(m3u_scratch_t));
    check("m3u_entry_t is 264 bytes (documented)",
          sizeof(m3u_entry_t) == 264);
    check("m3u_scratch_t is 1488 bytes (documented)",
          sizeof(m3u_scratch_t) == 1488);

    /* ---- 1. plain path-per-line, no directives ---------------------- */
    parse_str("01 Intro.flac\n02 Verse.mp3\n03 Outro.flac\n", "Music/Rock");
    check("plain: 3 entries", g_res.count == 3 && g_res.total == 3);
    check("plain: relative to base_dir",
          path_is(0, "Music/Rock/01 Intro.flac") &&
          path_is(1, "Music/Rock/02 Verse.mp3") &&
          path_is(2, "Music/Rock/03 Outro.flac"));
    check("plain: no title, duration unknown",
          g_res.count == 3 && g_out[0].title[0] == '\0' &&
          g_out[0].duration_s == -1);
    check("plain: nothing skipped, not truncated",
          g_res.skipped_long == 0 && g_res.skipped_escape == 0 &&
          g_res.skipped_bad == 0 && g_res.truncated == 0);
    check("plain: no #EXTM3U seen", g_res.had_extm3u == 0);
    check("plain: bytes counted", g_res.bytes == 41);
    if (g_fails) { dump("plain"); }

    /* ---- 2. #EXTM3U + #EXTINF: duration and title ------------------- */
    parse_str("#EXTM3U\n"
              "#EXTINF:213,Radiohead - Airbag\n"
              "airbag.flac\n"
              "#EXTINF:-1,Unknown Length Stream\n"
              "stream.mp3\n"
              "#EXTINF:212.386,Fractional Seconds\n"
              "frac.mp3\n"
              "#EXTINF:99\n"                      /* duration, no comma      */
              "notitle.mp3\n"
              "plain.mp3\n",                      /* no EXTINF at all        */
              "");
    check("extinf: header flagged", g_res.had_extm3u == 1);
    check("extinf: 5 entries", g_res.count == 5);
    check("extinf: duration + title captured",
          g_res.count == 5 &&
          g_out[0].duration_s == 213 &&
          strcmp(g_out[0].title, "Radiohead - Airbag") == 0 &&
          strcmp(g_out[0].path, "airbag.flac") == 0);
    check("extinf: -1 stays unknown, title still captured",
          g_res.count == 5 && g_out[1].duration_s == -1 &&
          strcmp(g_out[1].title, "Unknown Length Stream") == 0);
    check("extinf: fractional seconds truncated to whole",
          g_res.count == 5 && g_out[2].duration_s == 212 &&
          strcmp(g_out[2].title, "Fractional Seconds") == 0);
    check("extinf: no comma => duration only, empty title",
          g_res.count == 5 && g_out[3].duration_s == 99 &&
          g_out[3].title[0] == '\0');
    check("extinf: tag applies to ONE path only",
          g_res.count == 5 && g_out[4].duration_s == -1 &&
          g_out[4].title[0] == '\0' &&
          strcmp(g_out[4].path, "plain.mp3") == 0);
    if (g_fails) { dump("extinf"); }

    /* An EXTINF followed by a blank line and a comment still tags the path:
     * writers do that, and dropping the title over cosmetics loses data. */
    parse_str("#EXTINF:120,Tagged\n\n# a comment\n#EXTGRP:Rock\nx.mp3\n", "");
    check("extinf: survives blank/comment lines before its path",
          g_res.count == 1 && g_out[0].duration_s == 120 &&
          strcmp(g_out[0].title, "Tagged") == 0);

    /* Two EXTINFs in a row: the second wins, the first is simply lost (there
     * is no path for it to describe). */
    parse_str("#EXTINF:1,First\n#EXTINF:2,Second\nx.mp3\n", "");
    check("extinf: a second tag replaces an unconsumed first",
          g_res.count == 1 && g_out[0].duration_s == 2 &&
          strcmp(g_out[0].title, "Second") == 0);

    /* ---- 3. CRLF ---------------------------------------------------- */
    parse_str("#EXTM3U\r\n#EXTINF:10,Windows Title\r\none.mp3\r\ntwo.mp3\r\n",
              "");
    check("crlf: 2 entries, no stray CR in the path",
          g_res.count == 2 && path_is(0, "one.mp3") && path_is(1, "two.mp3"));
    check("crlf: no stray CR in the title",
          g_res.count == 2 && strcmp(g_out[0].title, "Windows Title") == 0);
    check("crlf: header still recognised", g_res.had_extm3u == 1);

    /* Lone CR (classic Mac) terminates a line too. */
    parse_str("one.mp3\rtwo.mp3\r", "");
    check("lone CR terminates lines",
          g_res.count == 2 && path_is(0, "one.mp3") && path_is(1, "two.mp3"));

    /* ---- 4. UTF-8 BOM ----------------------------------------------- */
    parse_str("\xEF\xBB\xBF" "#EXTM3U\n\xC3\x89t\xC3\xA9/song.mp3\n", "");
    check("bom: reported", g_res.had_bom == 1);
    check("bom: stripped, not glued to the first line",
          g_res.had_extm3u == 1 && g_res.count == 1 &&
          path_is(0, "\xC3\x89t\xC3\xA9/song.mp3"));

    /* Split the BOM across source reads: the detector must hold bytes back
     * rather than assume it sees all three at once. */
    parse_mem("\xEF\xBB\xBF" "a.mp3\n", 9, "", OUT_MAX, 1);
    check("bom: survives a one-byte-at-a-time source",
          g_res.had_bom == 1 && g_res.count == 1 && path_is(0, "a.mp3"));

    /* A file that merely STARTS with 0xEF but is not a BOM keeps its bytes. */
    parse_str("\xEF\xBB" "x.mp3\n", "");
    check("bom: a partial match is replayed, not eaten",
          g_res.had_bom == 0 && g_res.count == 1 &&
          path_is(0, "\xEF\xBB" "x.mp3"));

    /* A file that is nothing but a BOM. */
    parse_mem("\xEF\xBB\xBF", 3, "", OUT_MAX, 0);
    check("bom: BOM-only file yields nothing, cleanly",
          g_res.had_bom == 1 && g_res.count == 0 && g_res.total == 0);

    /* ---- 5. blank lines and stray whitespace ------------------------ */
    parse_str("\n\n   \t \n  one.mp3  \n\t\ttwo.mp3\t\n\n", "Music");
    check("whitespace: blanks ignored, edges trimmed",
          g_res.count == 2 && path_is(0, "Music/one.mp3") &&
          path_is(1, "Music/two.mp3"));
    check("whitespace: nothing counted as skipped",
          g_res.skipped_bad == 0 && g_res.skipped_long == 0);

    /* Internal spaces are part of the name and must survive. */
    parse_str("  02 - The Song Name.mp3  \n", "");
    check("whitespace: internal spaces preserved",
          g_res.count == 1 && path_is(0, "02 - The Song Name.mp3"));

    /* ---- 6. comments ------------------------------------------------ */
    parse_str("# just a comment\n"
              "#EXTM3U\n"
              "#PLAYLIST:My Mix\n"
              "#EXTVLCOPT:no-video\n"
              "   # indented comment\n"
              "real.mp3\n",
              "");
    check("comments: only the real path becomes an entry",
          g_res.count == 1 && path_is(0, "real.mp3"));
    check("comments: unknown directives are not counted as damage",
          g_res.skipped_bad == 0 && g_res.skipped_long == 0);

    /* ---- 7. backslash paths (and mixed separators) ------------------ */
    parse_str("Rock\\Album\\01.mp3\n"
              "Jazz/Album\\02.mp3\n"
              "\\Absolute\\Path\\03.mp3\n",
              "Music");
    check("backslash: converted to '/' and joined to the base",
          g_res.count == 3 &&
          path_is(0, "Music/Rock/Album/01.mp3") &&
          path_is(1, "Music/Jazz/Album/02.mp3"));
    check("backslash: a leading '\\' means volume root, not base_dir",
          path_is(2, "Absolute/Path/03.mp3"));

    /* Windows drive letter: the iPod was D: on someone's desktop. */
    parse_str("D:\\Music\\Rock\\01.mp3\nE:/Music/02.mp3\n", "Podcasts");
    check("drive letter stripped, path taken as volume-root-absolute",
          g_res.count == 2 && path_is(0, "Music/Rock/01.mp3") &&
          path_is(1, "Music/02.mp3"));

    /* Duplicate separators collapse. */
    parse_str("a//b\\\\c.mp3\n", "");
    check("duplicate separators collapse", path_is(0, "a/b/c.mp3"));

    /* ---- 8. '.' and '..' -------------------------------------------- */
    parse_str("./here.mp3\n"
              "../sibling/there.mp3\n"
              "sub/../flat.mp3\n"
              "a/b/c/../../d.mp3\n",
              "Music/Rock");
    check("dot: '.' is a no-op", path_is(0, "Music/Rock/here.mp3"));
    check("dot: '..' pops one level", path_is(1, "Music/sibling/there.mp3"));
    check("dot: '..' mid-path pops correctly",
          path_is(2, "Music/Rock/flat.mp3") &&
          path_is(3, "Music/Rock/a/d.mp3"));
    check("dot: legitimate '..' is not counted as an escape",
          g_res.skipped_escape == 0 && g_res.count == 4);
    if (g_fails) { dump("dots"); }

    /* Escapes: every one of these walks off the volume root and must be
     * REJECTED (not clamped to the root, which would silently play some
     * other file). base_dir is one level deep, so two '..' is already out. */
    parse_str("../../etc/passwd\n"
              "../../../../../../../../a.mp3\n"
              "/../root.mp3\n"
              "..\\..\\..\\windows\\x.mp3\n"
              "ok.mp3\n",
              "Music");
    check("escape: all four traversals rejected",
          g_res.skipped_escape == 4);
    check("escape: nothing clamped into the output",
          g_res.count == 1 && path_is(0, "Music/ok.mp3"));
    check("escape: total counts only resolvable lines", g_res.total == 1);
    if (g_fails) { dump("escape"); }

    /* A line that resolves to the volume root itself names no file at all,
     * so it is dropped as bad rather than as an escape — the distinction the
     * counters draw is "walked outside the volume" vs "resolved to nothing". */
    parse_str("/\n\\\\\n..\n", "Music");
    check("a line resolving to the volume root is dropped as bad",
          g_res.count == 0 && g_res.total == 0 && g_res.skipped_bad == 3 &&
          g_res.skipped_escape == 0);

    /* We never touch the directory tree, so a line naming a directory that
     * really exists is NOT detectable here: "." with a base resolves to the
     * base itself and is handed back. Asserted so the behaviour is a
     * deliberate division of labour (the later cluster resolver discovers it
     * is not a file) rather than an accident. */
    parse_str(".\n", "Music");
    check("'.' resolves to base_dir; directory detection is the resolver's job",
          g_res.count == 1 && path_is(0, "Music"));

    /* URLs are rejected by the one-colon-at-index-1 rule, with no scheme
     * list to keep up to date. */
    parse_str("http://stream.example/live.mp3\n"
              "https://a/b.mp3\n"
              "file:///Music/x.mp3\n"
              "weird:name.mp3\n"
              "local.mp3\n",
              "");
    check("urls and stray colons rejected",
          g_res.count == 1 && path_is(0, "local.mp3") &&
          g_res.skipped_bad == 4);

    /* ---- 9. a path longer than the cap ------------------------------ */
    {
        /* One 300-char segment: fits the LINE buffer, blows M3U_PATH_MAX. */
        static char big[400];
        uint32_t k = 0;
        for (; k < 300; k++) {
            big[k] = 'x';
        }
        big[k++] = '.'; big[k++] = 'm'; big[k++] = 'p'; big[k++] = '3';
        big[k++] = '\n';
        memcpy(&big[k], "short.mp3\n", 10);
        k += 10;
        parse_mem(big, k, "", OUT_MAX, 0);
        check("long path: over M3U_PATH_MAX is dropped and counted",
              g_res.skipped_long == 1 && g_res.count == 1 &&
              path_is(0, "short.mp3"));
    }
    {
        /* A path that is exactly M3U_PATH_MAX must still be accepted — the
         * boundary is the part that gets off-by-one'd. */
        static char exact[M3U_PATH_MAX + 8];
        uint32_t k = 0;
        for (; k < M3U_PATH_MAX; k++) {
            exact[k] = 'y';
        }
        exact[k++] = '\n';
        parse_mem(exact, k, "", OUT_MAX, 0);
        check("long path: exactly M3U_PATH_MAX is accepted",
              g_res.count == 1 && strlen(g_out[0].path) == M3U_PATH_MAX &&
              g_res.skipped_long == 0);
    }
    {
        /* A LINE longer than M3U_LINE_MAX: the tail never reached us, so the
         * line is discarded whole rather than truncated into a valid-looking
         * path pointing at the wrong file. The parser must resynchronise on
         * the next newline. */
        static char huge[M3U_LINE_MAX * 2 + 32];
        uint32_t k = 0;
        for (; k < M3U_LINE_MAX + 100u; k++) {
            huge[k] = 'z';
        }
        huge[k++] = '\n';
        memcpy(&huge[k], "after.mp3\n", 10);
        k += 10;
        parse_mem(huge, k, "", OUT_MAX, 0);
        check("long line: over M3U_LINE_MAX dropped whole and counted",
              g_res.skipped_long == 1 && g_res.count == 1);
        check("long line: parser resynchronises on the next newline",
              path_is(0, "after.mp3"));
    }
    {
        /* An over-long COMMENT is still a comment: ignored, not counted as a
         * lost track. */
        static char huge[M3U_LINE_MAX * 2 + 32];
        uint32_t k = 0;
        huge[k++] = '#';
        for (; k < M3U_LINE_MAX + 100u; k++) {
            huge[k] = 'c';
        }
        huge[k++] = '\n';
        memcpy(&huge[k], "after.mp3\n", 10);
        k += 10;
        parse_mem(huge, k, "", OUT_MAX, 0);
        check("long line: an over-long comment is not counted as damage",
              g_res.skipped_long == 0 && g_res.count == 1 &&
              path_is(0, "after.mp3"));
    }

    /* ---- 10. more entries than the caller's array ------------------- */
    {
        static char many[512];
        uint32_t k = 0;
        for (int i = 0; i < 25; i++) {
            many[k++] = (char)('a' + (i % 26));
            memcpy(&many[k], ".mp3\n", 5);
            k += 5;
        }
        parse_mem(many, k, "", OUT_MAX, 0);
        check("truncation: fills the array exactly",
              g_res.count == OUT_MAX);
        check("truncation: SIGNALLED, not silent", g_res.truncated == 1);
        check("truncation: total reports the real count", g_res.total == 25);
        check("truncation: the entries kept are the first ones",
              path_is(0, "a.mp3") && path_is(OUT_MAX - 1, "p.mp3"));

        /* out_max 0 with a NULL array is a legal counting pass. */
        {
            mem_src_t src = { (const uint8_t *)many, k, 0, 0, 0, 0 };
            int rc = m3u_parse(mem_src, &src, "", NULL, 0, &g_scratch, &g_res);
            check("counting pass (out_max=0, out=NULL) is legal",
                  rc == M3U_OK && g_res.count == 0 && g_res.total == 25 &&
                  g_res.truncated == 1);
        }
    }

    /* ---- 11. an empty file ------------------------------------------ */
    parse_mem("", 0, "Music", OUT_MAX, 0);
    check("empty file: OK, zero entries, nothing flagged",
          g_res.count == 0 && g_res.total == 0 && g_res.bytes == 0 &&
          g_res.truncated == 0 && g_res.file_truncated == 0 &&
          g_res.skipped_bad == 0);

    /* ---- 12. a file of only comments -------------------------------- */
    parse_str("#EXTM3U\n# nothing here\n#EXTINF:100,Orphan Title\n", "");
    check("comments only: header seen, no entries, nothing skipped",
          g_res.had_extm3u == 1 && g_res.count == 0 && g_res.total == 0 &&
          g_res.skipped_bad == 0 && g_res.skipped_long == 0);

    /* ---- 13. binary garbage ----------------------------------------- */
    {
        /* A JPEG-ish blob renamed .m3u8: high bytes, NULs, control codes, no
         * newline structure to speak of. Nothing must be emitted, nothing
         * must hang, and the damage must be counted. */
        static uint8_t junk[1024];
        for (uint32_t i = 0; i < sizeof junk; i++) {
            junk[i] = (uint8_t)((i * 37u) ^ 0xA5u);
        }
        junk[100] = '\n';
        junk[300] = '\n';
        junk[700] = '\n';
        parse_mem((const char *)junk, (uint32_t)sizeof junk, "", OUT_MAX, 0);
        check("binary: emits no entries", g_res.count == 0 && g_res.total == 0);
        check("binary: the damage is counted, not silent",
              g_res.skipped_bad + g_res.skipped_long + g_res.skipped_escape > 0);
        check("binary: consumed exactly the bytes offered",
              g_res.bytes == sizeof junk);

        /* All-NUL is the other degenerate binary shape. */
        memset(junk, 0, sizeof junk);
        parse_mem((const char *)junk, 64, "", OUT_MAX, 0);
        check("all-NUL input emits nothing", g_res.count == 0);
    }

    /* A control byte in the middle of an otherwise fine path rejects it —
     * FAT32 cannot name such a file, so the line is corruption. */
    parse_str("good.mp3\nbad\x01name.mp3\nalso_good.mp3\n", "");
    check("control byte inside a path rejects that line",
          g_res.count == 2 && g_res.skipped_bad == 1 &&
          path_is(0, "good.mp3") && path_is(1, "also_good.mp3"));

    /* ---- 14. no trailing newline ------------------------------------ */
    parse_str("#EXTM3U\n#EXTINF:42,Last One\nfinal.mp3", "Music");
    check("no trailing newline: the last line is still parsed",
          g_res.count == 1 && path_is(0, "Music/final.mp3") &&
          g_out[0].duration_s == 42 &&
          strcmp(g_out[0].title, "Last One") == 0);

    /* ---- UTF-8 titles ----------------------------------------------- */
    parse_str("#EXTINF:5,Sigur R\xC3\xB3s \xE2\x80\x93 Hopp\xC3\xADpolla\n"
              "s.flac\n", "");
    check("utf-8 title passes through byte-for-byte",
          g_res.count == 1 &&
          strcmp(g_out[0].title,
                 "Sigur R\xC3\xB3s \xE2\x80\x93 Hopp\xC3\xADpolla") == 0);
    {
        /* A title longer than M3U_TITLE_MAX must be cut on a CHARACTER
         * boundary: 32 x the 2-byte "é" is exactly 64 bytes, so adding one
         * more must drop the whole character, never half of it. */
        static char t[256];
        uint32_t k = 0;
        memcpy(&t[k], "#EXTINF:1,", 10); k += 10;
        for (int i = 0; i < 40; i++) {
            t[k++] = (char)0xC3; t[k++] = (char)0xA9;   /* U+00E9 */
        }
        t[k++] = '\n';
        memcpy(&t[k], "x.mp3\n", 6); k += 6;
        parse_mem(t, k, "", OUT_MAX, 0);
        size_t tl = strlen(g_out[0].title);
        check("over-long title truncated on a UTF-8 boundary",
              g_res.count == 1 && tl == 64 && (tl % 2) == 0);
        int clean = 1;
        for (size_t i = 0; i < tl; i += 2) {
            if ((uint8_t)g_out[0].title[i] != 0xC3u ||
                (uint8_t)g_out[0].title[i + 1] != 0xA9u) {
                clean = 0;
            }
        }
        check("truncated title contains no split sequence", clean);
    }

    /* ---- base_dir normalisation ------------------------------------- */
    parse_str("x.mp3\n", "\\Music\\Rock\\");
    check("base_dir: backslashes and edge separators normalised",
          path_is(0, "Music/Rock/x.mp3"));
    parse_str("x.mp3\n", "Music//Rock/./Sub/..");
    check("base_dir: '.', '..' and doubled separators applied",
          path_is(0, "Music/Rock/x.mp3"));
    parse_str("x.mp3\n", NULL);
    check("base_dir: NULL means the volume root", path_is(0, "x.mp3"));
    parse_str("x.mp3\n", "../../oops");
    check("base_dir: an escaping base degrades to the root, parse continues",
          g_res.count == 1 && path_is(0, "x.mp3"));

    /* ---- bounded reads: an endless source --------------------------- */
    {
        uint32_t phase = 0;
        int rc = m3u_parse(endless_src, &phase, "", g_out, OUT_MAX,
                           &g_scratch, &g_res);
        check("endless source: terminates at the byte cap",
              rc == M3U_OK && g_res.bytes == M3U_FILE_MAX);
        check("endless source: file_truncated reported",
              g_res.file_truncated == 1);
        check("endless source: still parsed what it read",
              g_res.count == OUT_MAX && g_res.truncated == 1);
    }

    /* ---- a source that fails mid-file ------------------------------- */
    {
        static const char text[] =
            "one.mp3\ntwo.mp3\nthree.mp3\nfour.mp3\nfive.mp3\n";
        mem_src_t src = { (const uint8_t *)text,
                          (uint32_t)(sizeof text - 1), 0, 8, 24, 0 };
        int rc = m3u_parse(mem_src, &src, "", g_out, OUT_MAX,
                           &g_scratch, &g_res);
        check("read error reported as M3U_EIO", rc == M3U_EIO);
        /* 24 bytes arrived: "one.mp3\ntwo.mp3\nthree.mp". The two COMPLETE
         * lines are kept; the half-line the error cut off is NOT flushed,
         * because "three.mp" is a perfectly plausible-looking path to a file
         * that is not the one the user asked for. */
        check("read error keeps the complete lines that arrived",
              g_res.count == 2 && path_is(0, "one.mp3") &&
              path_is(1, "two.mp3"));
    }

    /* ---- argument validation ---------------------------------------- */
    {
        mem_src_t src = { (const uint8_t *)"a.mp3\n", 6, 0, 0, 0, 0 };
        check("NULL result pointer rejected",
              m3u_parse(mem_src, &src, "", g_out, OUT_MAX, &g_scratch, NULL)
                  == M3U_EINVAL);
        check("NULL source rejected",
              m3u_parse(NULL, &src, "", g_out, OUT_MAX, &g_scratch, &g_res)
                  == M3U_EINVAL);
        check("NULL scratch rejected",
              m3u_parse(mem_src, &src, "", g_out, OUT_MAX, NULL, &g_res)
                  == M3U_EINVAL);
        check("NULL out with out_max>0 rejected",
              m3u_parse(mem_src, &src, "", NULL, OUT_MAX, &g_scratch, &g_res)
                  == M3U_EINVAL);
        check("a rejected call still zeroes the result",
              g_res.count == 0 && g_res.total == 0);
    }

    /* ---- reuse: one scratch, back-to-back parses -------------------- */
    parse_str("#EXTINF:7,Sticky\nfirst.mp3\n", "A");
    parse_str("second.mp3\n", "B");
    check("scratch carries no state between parses",
          g_res.count == 1 && path_is(0, "B/second.mp3") &&
          g_out[0].title[0] == '\0' && g_out[0].duration_s == -1 &&
          g_res.had_extm3u == 0);

    /* ---- deterministic fuzz sweep ----------------------------------- */
    /*
     * The hand-written cases above each encode a way a playlist goes wrong
     * that someone thought of. This one covers the ones nobody thought of:
     * 4000 pseudo-random blobs, drawn from an alphabet weighted towards the
     * bytes that actually steer the parser ('/', '\\', '.', ':', '#', ',',
     * NUL, newlines, high UTF-8 bytes), fed through sources that hand them
     * over in awkward chunk sizes.
     *
     * The assertions are deliberately structural rather than value-based —
     * there is no expected output for random bytes. What must hold for EVERY
     * input is: the call returns (a hang shows up as a test that never
     * finishes, which is a failure a CI timeout catches), it never writes
     * past the caller's array, and every string it produces is properly
     * terminated and within its documented cap. Run this file under
     * ASan/UBSan and those last two become memory-safety assertions rather
     * than length checks; the seed is fixed so a finding is reproducible.
     */
    {
        uint32_t seed = 0x1BADB002u;
        int structural_ok = 1;
        static uint8_t blob[900];
        static const char alpha[] =
            "/\\..::##,,\n\n\r\r\t  abcMP3\xC3\xA9\xEF\xBB\xBF\x01\x7F\xFF";

        for (int iter = 0; iter < 4000 && structural_ok; iter++) {
            uint32_t len, chunk;

            seed = seed * 1664525u + 1013904223u;
            len = seed % (uint32_t)sizeof blob;
            for (uint32_t i = 0; i < len; i++) {
                seed = seed * 1664525u + 1013904223u;
                /* 3 in 4 bytes come from the steering alphabet; the rest are
                 * uniform, so pure-binary shapes are covered too. */
                blob[i] = ((seed >> 16) & 3u)
                            ? (uint8_t)alpha[(seed >> 8) % (sizeof alpha - 1u)]
                            : (uint8_t)(seed >> 24);
            }
            seed = seed * 1664525u + 1013904223u;
            chunk = 1u + (seed % 300u);

            /* Alternate between the volume root and a deep base_dir, since
             * base depth is what decides whether a '..' run escapes. */
            parse_mem((const char *)blob, len,
                      (iter & 1) ? "Music/Rock/Deep" : "",
                      OUT_MAX, chunk);

            if (g_res.count > OUT_MAX || g_res.count > g_res.total ||
                g_res.bytes != len) {
                printf("  fuzz iter %d: count=%u total=%u bytes=%u len=%u\n",
                       iter, g_res.count, g_res.total, g_res.bytes, len);
                structural_ok = 0;
                break;
            }
            for (uint32_t i = 0; i < g_res.count; i++) {
                size_t pl = strlen(g_out[i].path);
                size_t tl = strlen(g_out[i].title);
                /* Non-empty, within cap, no separator at either edge, and no
                 * "." / ".." segment left anywhere in the result. */
                if (pl == 0 || pl > M3U_PATH_MAX || tl > M3U_TITLE_MAX ||
                    g_out[i].path[0] == '/' || g_out[i].path[pl - 1] == '/' ||
                    strstr(g_out[i].path, "//") != NULL ||
                    strstr(g_out[i].path, "/./") != NULL ||
                    strstr(g_out[i].path, "/../") != NULL ||
                    strcmp(g_out[i].path, ".") == 0 ||
                    strcmp(g_out[i].path, "..") == 0) {
                    printf("  fuzz iter %d entry %u: bad path '%s'\n",
                           iter, i, g_out[i].path);
                    structural_ok = 0;
                    break;
                }
            }
        }
        check("fuzz: 4000 random blobs all terminate with sane output",
              structural_ok);
    }

    /* ---- 15. the on-disk adapter, end to end ------------------------ */
    {
        static const char pl[] =
            "\xEF\xBB\xBF" "#EXTM3U\r\n"
            "#EXTINF:301,Album Opener\r\n"
            "..\\Rock\\Album\\01 Opener.flac\r\n"
            "02 Second.mp3\r\n"
            "../../../escape.mp3\r\n";   /* base is 2 deep: this leaves root */
        build_playlist_image(pl, (uint32_t)(sizeof pl - 1));

        fat32_t fs;
        check("m3u_parse_file: image mounts",
              fat32_mount(&fs, mem_disk_read, NULL, 0) == 0);

        uint32_t clus = 0, size = 0;
        check("m3u_parse_file: ROCK.M3U found",
              fat32_open(&fs, "ROCK.M3U", &clus, &size) == 0 &&
              size == (uint32_t)(sizeof pl - 1));

        int rc = m3u_parse_file(&fs, clus, size, "Music/Pop",
                                g_out, OUT_MAX, &g_scratch, &g_res);
        check("m3u_parse_file: returns OK", rc == M3U_OK);
        check("m3u_parse_file: BOM + CRLF + EXTINF all handled off disk",
              g_res.had_bom == 1 && g_res.had_extm3u == 1 &&
              g_res.count == 2);
        check("m3u_parse_file: paths resolved against base_dir",
              path_is(0, "Music/Rock/Album/01 Opener.flac") &&
              path_is(1, "Music/Pop/02 Second.mp3"));
        check("m3u_parse_file: title/duration survive the disk path",
              g_res.count == 2 && g_out[0].duration_s == 301 &&
              strcmp(g_out[0].title, "Album Opener") == 0 &&
              g_out[1].duration_s == -1);
        check("m3u_parse_file: the escaping line is still rejected",
              g_res.skipped_escape == 1);
        if (g_fails) { dump("fat32"); }

        /* A directory entry that lies about the size must not be believed
         * into reading past the file: the stream stops at `size`. */
        rc = m3u_parse_file(&fs, clus, 20, "", g_out, OUT_MAX,
                            &g_scratch, &g_res);
        check("m3u_parse_file: honours the size bound",
              rc == M3U_OK && g_res.bytes == 20);

        /* An empty file on disk. */
        rc = m3u_parse_file(&fs, clus, 0, "", g_out, OUT_MAX,
                            &g_scratch, &g_res);
        check("m3u_parse_file: zero-size file is empty, not an error",
              rc == M3U_OK && g_res.count == 0 && g_res.bytes == 0);

        check("m3u_parse_file: NULL fs rejected",
              m3u_parse_file(NULL, clus, size, "", g_out, OUT_MAX,
                             &g_scratch, &g_res) == M3U_EINVAL);
    }

    if (g_fails == 0) {
        printf("ALL PASS\n");
    } else {
        printf("FAIL: %d check%s failed\n", g_fails, g_fails == 1 ? "" : "s");
    }
    return g_fails == 0 ? 0 : 1;
}
