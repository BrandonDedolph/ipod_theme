/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/kernel/resume_test.c — host tests for the resume LOCATOR RESOLUTION
 * in kernel/main.c: given a saved name hash and track length, which song in
 * the library (if any) may the boot path re-open?
 *
 * WHY THIS IS THE TEST THAT MATTERS. Everything else about resume degrades
 * gracefully — a missed capture just loses a position, a rejected record just
 * means "start from the menu". This one function is the only place where a
 * wrong answer is ACTED ON: it hands the boot path a song, and the boot path
 * opens it. Resuming nothing is a shrug; resuming somebody else's track
 * because two albums both contain "01 Intro.flac" is the bug.
 *
 * WHY A COPY: resume_find_song() is `static` inside kernel/main.c, which also
 * drags in the whole UI, the FAT reader, the player and the HAL — there is no
 * way to link the real one into a host test. So the function body below is a
 * VERBATIM COPY, diffed against main.c by tests/scripts/check_resume_parity.py
 * (run from `meson test` and from `make verify-hw`), exactly as
 * name_hash_ref.c does for the locator hash. Do not reformat, re-indent or
 * rename anything between the BEGIN/END markers; if you change main.c, paste
 * the new text in here.
 *
 * The library it reads is stubbed below with the same field NAMES main.c's
 * lib_song_t uses, so the copied body compiles unchanged.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "name_hash_ref.h"

static int g_fails;

static void check(const char *label, int cond)
{
    printf("[%s] %s\n", label, cond ? "PASS" : "FAIL");
    if (!cond) {
        g_fails++;
    }
}

/* ---- the environment the copied function reads -------------------------- */

/*
 * A stand-in for main.c's g_songs[]/g_songs_n, carrying only the three fields
 * resume_find_song() touches — under the same names, so the copy needs no
 * edits. main.c's lib_song_t has more, and none of it is consulted here.
 */
#define STUB_FILE_MAX 64
#define STUB_SONGS    16

static struct {
    char     file[STUB_FILE_MAX];    /* filename, extension trimmed */
    uint32_t file_clus;              /* 0 = indexed but not on disk */
    uint32_t duration_s;
} g_songs[STUB_SONGS];
static int g_songs_n;

/* main.c's name_hash() is static too; name_hash_ref.c holds the parity-checked
 * copy, so route the one call in the body at it. */
static uint32_t name_hash(const char *s)
{
    return name_hash_ref(s);
}

/* Slop between the library's indexed duration and the decoder's, in seconds.
 * Restated here rather than shared: this file is the independent statement of
 * what the rule is, and check_resume_parity.py pins the value against main.c. */
#define RESUME_DUR_SLOP    2u

/* ---- BEGIN VERBATIM COPY OF core/kernel/main.c ------------------------- */

static int resume_find_song(uint32_t hash, uint32_t total_s)
{
    int best = -1, n_named = 0;

    for (int i = 0; i < g_songs_n; i++) {
        if (name_hash(g_songs[i].file) != hash) {
            continue;
        }
        n_named++;
        if (!g_songs[i].file_clus) {
            continue;                  /* indexed but not on the disk any more */
        }
        uint32_t d = g_songs[i].duration_s;
        if (d != 0 && total_s != 0 &&
            d + RESUME_DUR_SLOP >= total_s && total_s + RESUME_DUR_SLOP >= d) {
            return i;                  /* name AND length agree — this is it */
        }
        if (best < 0) {
            best = i;
        }
    }
    return (n_named == 1) ? best : -1;
}

/* ---- END VERBATIM COPY -------------------------------------------------- */

/* ---- library fixture ---------------------------------------------------- */

static void lib_reset(void)
{
    memset(g_songs, 0, sizeof g_songs);
    g_songs_n = 0;
}

static void lib_add(const char *file, uint32_t clus, uint32_t dur)
{
    int i = g_songs_n++;
    size_t n = strlen(file);
    memcpy(g_songs[i].file, file, n < STUB_FILE_MAX ? n : STUB_FILE_MAX - 1);
    g_songs[i].file_clus  = clus;
    g_songs[i].duration_s = dur;
}

#define H(s) name_hash_ref(s)

/* ---- 1. the happy path -------------------------------------------------- */

static void test_resolves(void)
{
    lib_reset();
    lib_add("Blue Monday", 100, 442);
    lib_add("Ceremony",    101, 264);
    lib_add("Temptation",  102, 415);

    check("resolves a unique name with a matching length",
          resume_find_song(H("Blue Monday"), 442) == 0);
    check("resolves the other entries too",
          resume_find_song(H("Ceremony"), 264) == 1 &&
          resume_find_song(H("Temptation"), 415) == 2);

    /* The stored length comes from the decoder and the library's from the
     * tags; both truncate to whole seconds from different sources, so they
     * disagree by a second on perfectly good matches. */
    check("tolerates a 1 s duration disagreement",
          resume_find_song(H("Ceremony"), 265) == 1 &&
          resume_find_song(H("Ceremony"), 263) == 1);
    check("tolerates the full slop either way",
          resume_find_song(H("Ceremony"), 266) == 1 &&
          resume_find_song(H("Ceremony"), 262) == 1);

    /* The hash is case/quote-folded, which is the whole reason the library
     * binds by it: a re-import that re-cases a filename must not lose the
     * resume. */
    check("the match is case-folded",
          resume_find_song(H("BLUE MONDAY"), 442) == 0);
}

/* ---- 2. stale resume: every way it must decline ------------------------- */

static void test_stale(void)
{
    /* The track was deleted, or the library was rebuilt without it. */
    lib_reset();
    lib_add("Ceremony",   101, 264);
    lib_add("Temptation", 102, 415);
    check("declines a name that is no longer in the library",
          resume_find_song(H("Blue Monday"), 442) < 0);

    /* An empty library — first boot after an import that has not been indexed,
     * or a disk with no music on it at all. */
    lib_reset();
    check("declines against an empty library",
          resume_find_song(H("Blue Monday"), 442) < 0);

    /* Indexed but unresolved: CORELIB.IDX still lists the track, the file is
     * not on the disk. Opening it would fail; declining is cheaper and
     * quieter. */
    lib_reset();
    lib_add("Blue Monday", 0 /* unresolved */, 442);
    check("declines a song with no file cluster",
          resume_find_song(H("Blue Monday"), 442) < 0);

    /* A hash of 0 is main.c's "nothing saved" sentinel, and resume_restore
     * short-circuits on it — but if one ever reached here it must not match a
     * real song by accident. */
    lib_reset();
    lib_add("Blue Monday", 100, 442);
    check("declines the empty-locator sentinel",
          resume_find_song(0, 0) < 0);
}

/* ---- 3. ambiguity: the one that would play the WRONG track -------------- */

static void test_ambiguous(void)
{
    /*
     * "01 Intro.flac" in three albums, all different lengths. The saved length
     * is what picks one, and it must pick the RIGHT one — not the first, which
     * is what a name-only match would have returned.
     */
    lib_reset();
    lib_add("01 Intro", 200, 61);
    lib_add("01 Intro", 201, 95);
    lib_add("01 Intro", 202, 130);

    check("duplicate names: the length picks the right one",
          resume_find_song(H("01 Intro"), 95) == 1);
    check("duplicate names: and the third",
          resume_find_song(H("01 Intro"), 130) == 2);
    check("duplicate names: and the first",
          resume_find_song(H("01 Intro"), 61) == 0);

    /* No length agrees: three candidates, no way to choose. Declining is the
     * requirement — a coin flip is not a resume. */
    check("duplicate names with no length match declines",
          resume_find_song(H("01 Intro"), 300) < 0);

    /* Length unknown on the saved side (an old record, or a stream whose
     * duration the decoder never reported). Same answer: ambiguous. */
    check("duplicate names with no saved length declines",
          resume_find_song(H("01 Intro"), 0) < 0);

    /* Untagged library entries (duration 0) can't confirm anything either. */
    lib_reset();
    lib_add("01 Intro", 200, 0);
    lib_add("01 Intro", 201, 0);
    check("duplicate untagged names decline",
          resume_find_song(H("01 Intro"), 95) < 0);

    /*
     * A UNIQUE name with no length confirmation is still taken: this is the
     * deliberate asymmetry. One song can only be itself, so the worst case is
     * resuming a track whose tags changed — not a different track.
     */
    lib_reset();
    lib_add("01 Intro", 200, 0);
    check("a unique untagged name still resolves",
          resume_find_song(H("01 Intro"), 95) == 0);
    lib_reset();
    lib_add("01 Intro", 200, 61);
    check("a unique name with a wildly wrong length still resolves",
          resume_find_song(H("01 Intro"), 4000) == 0);

    /*
     * The uniqueness count is over the NAME, before the on-disk check — so a
     * name that appears twice but resolves once is still ambiguous. That is
     * the conservative reading, and it is intentional: the second copy may be
     * the one the user was actually playing, just not yet resolved.
     */
    lib_reset();
    lib_add("01 Intro", 200, 0);
    lib_add("01 Intro", 0,   0);
    check("a duplicated name is ambiguous even when only one resolves",
          resume_find_song(H("01 Intro"), 95) < 0);
}

int main(void)
{
    test_resolves();
    test_stale();
    test_ambiguous();

    printf("resume_test: %s (%d failure%s)\n",
           g_fails == 0 ? "PASS" : "FAIL", g_fails, g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
