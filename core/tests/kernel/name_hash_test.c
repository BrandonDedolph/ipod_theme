/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/kernel/name_hash_test.c — the C half of the library-locator parity
 * test.
 *
 * The library index binds a CORELIB.IDX record to its file on disk by ONE
 * value: name_hash(). That function exists twice — in kernel/main.c (which
 * resolves the record on device) and in tools/build_index.py (which writes the
 * record on the host) — and its own comment says the two "MUST stay
 * byte-identical". Nothing checked that. If they drift, the affected track is
 * not reported missing; it simply never resolves and disappears from the
 * library.
 *
 * This binary asserts the C side against tests/kernel/name_hash_vectors.h.
 * tests/scripts/check_name_hash_parity.py asserts the Python side against the
 * SAME table, and additionally diffs the copy in name_hash_ref.c against the
 * original in kernel/main.c so the copy cannot go stale.
 *
 * The vectors deliberately include the collision pairs the fold exists for:
 * three case variants of one filename, smart vs straight quotes, and en/em
 * dash vs hyphen must each hash to a SINGLE value — otherwise a rename that
 * only changes quote style silently unbinds the track.
 */

#include <stdio.h>
#include <stdint.h>

#include "name_hash_ref.h"

static int fails;
static int xfails;
static int xpasses;

static void vec(const char *label, const char *utf8, uint32_t want)
{
    uint32_t got = name_hash_ref(utf8);
    if (got == want) {
        printf("[name-hash %s] PASS (%08X)\n", label, got);
        return;
    }
    fprintf(stderr, "[name-hash %s] FAIL: got %08X, want %08X\n",
            label, got, want);
    fails++;
}

static void xfail_vec(const char *label, const char *utf8, uint32_t want,
                      const char *why)
{
    uint32_t got = name_hash_ref(utf8);
    if (got != want) {
        printf("[name-hash %s] XFAIL (got %08X, want %08X) — known bug: %s\n",
               label, got, want, why);
        xfails++;
        return;
    }
    /* The upstream bug got fixed. Say so loudly, but do not fail: the fix
     * lands in kernel/main.c, which this test does not own, and breaking the
     * build the moment someone fixes it would be perverse. Re-paste the
     * function into name_hash_ref.c and promote this vector to NAME_HASH_VEC. */
    printf("[name-hash %s] XPASS — the known bug is FIXED (%s).\n"
           "    ACTION: re-copy name_hash() into tests/kernel/name_hash_ref.c\n"
           "    and change this vector from NAME_HASH_XFAIL to NAME_HASH_VEC.\n",
           label, why);
    xpasses++;
}

/* Independent restatement of the fold, used below to prove the golden table
 * itself is not simply "whatever the implementation printed": for pure-ASCII
 * inputs the hash must equal a plain FNV-1a over the lowercased bytes. */
static uint32_t fnv1a_lower_ascii(const char *s)
{
    uint32_t h = 0x811c9dc5u;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if (c >= 'A' && c <= 'Z') {
            c = (unsigned char)(c + 32);
        }
        h ^= c;
        h *= 0x01000193u;
    }
    return h;
}

static void check(const char *label, int cond)
{
    if (cond) {
        printf("[name-hash %s] PASS\n", label);
    } else {
        fprintf(stderr, "[name-hash %s] FAIL\n", label);
        fails++;
    }
}

int main(void)
{
#define NAME_HASH_VEC(label, lit, want)          vec(label, lit, want);
#define NAME_HASH_XFAIL(label, lit, want, why)   xfail_vec(label, lit, want, why);
#include "name_hash_vectors.h"
#undef NAME_HASH_VEC
#undef NAME_HASH_XFAIL

    /* Cross-check the golden values against an independent ASCII FNV-1a, so a
     * corrupted table cannot quietly agree with a corrupted implementation. */
    check("golden-table-is-fnv1a (ascii)",
          name_hash_ref("01. Intentions.flac") ==
          fnv1a_lower_ascii("01. Intentions.flac"));
    check("golden-table-is-fnv1a (folder)",
          name_hash_ref("Justin Bieber - Changes") ==
          fnv1a_lower_ascii("Justin Bieber - Changes"));

    /* The folds exist to make these collide; state it as its own assertion so
     * a failure reads as "the fold broke", not "a magic number changed". */
    check("case fold collides",
          name_hash_ref("01. INTENTIONS.FLAC") ==
          name_hash_ref("01. intentions.flac"));
    check("smart apostrophe folds to straight",
          name_hash_ref("Don\342\200\231t Stop") == name_hash_ref("Don't Stop"));
    check("smart quotes fold to straight",
          name_hash_ref("\342\200\234Hello\342\200\235") ==
          name_hash_ref("\042Hello\042"));
    check("en/em dash fold to hyphen",
          name_hash_ref("A \342\200\223 B") == name_hash_ref("A - B") &&
          name_hash_ref("A \342\200\224 B") == name_hash_ref("A - B"));

    /* Distinct names must NOT collide — otherwise the "fold" is just a
     * flattener and two tracks resolve to each other's files. */
    check("distinct names differ",
          name_hash_ref("01. Intentions.flac") !=
          name_hash_ref("02. Intentions.flac"));

    /* Non-ASCII is deliberately NOT case-folded (the fold is ASCII-only). If
     * one side ever grows a Unicode fold and the other does not, every
     * accented track unbinds — pin the current contract. */
    check("accented capital is not folded",
          name_hash_ref("\303\211douard") != name_hash_ref("\303\251douard"));

    /* Malformed UTF-8 must terminate and produce *some* value rather than
     * running off the end of the buffer: a truncated 3-byte lead followed by
     * NUL is exactly what a byte-truncated FAT long name looks like. */
    check("truncated multibyte terminates",
          name_hash_ref("abc\342\200") != 0u);

    printf("name-hash: %d failure(s), %d xfail(s), %d xpass(es)\n",
           fails, xfails, xpasses);
    return fails == 0 ? 0 : 1;
}
