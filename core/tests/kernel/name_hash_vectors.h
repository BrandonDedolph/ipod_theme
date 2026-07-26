/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/kernel/name_hash_vectors.h — THE golden vector table for the library
 * locator hash. One table, read by both sides:
 *
 *   C      — tests/kernel/name_hash_test.c, against the verbatim copy of
 *            kernel/main.c's name_hash().
 *   Python — tests/scripts/check_name_hash_parity.py, against
 *            tools/build_index.py's name_hash().
 *
 * The hash is the ONLY thing binding a CORELIB.IDX record to the file on disk,
 * so if the two implementations ever disagree the affected track silently stops
 * resolving — no error, no log, the song just vanishes from the library. That
 * failure is invisible in every other test, which is why this table exists.
 *
 * FORMAT — one macro call per vector, parsed by the Python side with a regex,
 * so keep the shape exactly:
 *
 *   NAME_HASH_VEC(label, utf8_literal, expected_hash)
 *   NAME_HASH_XFAIL(label, utf8_literal, expected_hash, why)
 *
 * `utf8_literal` is the name as raw UTF-8 BYTES: printable ASCII verbatim,
 * everything else as a THREE-digit octal escape (\303). Octal is used rather
 * than \xNN because a C hex escape is greedy — "\xc3\x89douard" would swallow
 * the 'd' into the escape and overflow — while an octal escape always stops
 * after three digits, and Python's unicode_escape codec decodes the same form
 * identically. `expected_hash` is the CORRECT value: what a conforming
 * implementation must produce.
 *
 * NAME_HASH_XFAIL marks a vector the C side is KNOWN to get wrong today; see
 * the note at the bottom of this file.
 */
#ifndef CORE_TESTS_KERNEL_NAME_HASH_VECTORS_H
#define CORE_TESTS_KERNEL_NAME_HASH_VECTORS_H

/* Both macros must be defined by the includer. */
#ifndef NAME_HASH_VEC
#error "define NAME_HASH_VEC before including name_hash_vectors.h"
#endif
#ifndef NAME_HASH_XFAIL
#error "define NAME_HASH_XFAIL before including name_hash_vectors.h"
#endif

/* --- the empty name: the FNV-1a offset basis, unmodified --------------- */
NAME_HASH_VEC("empty", "", 0x811C9DC5u)

/* --- ASCII, and the case fold: all three must collide ------------------ */
NAME_HASH_VEC("ascii-lower", "01. intentions.flac", 0x3704927Fu)
NAME_HASH_VEC("ascii-mixed-case", "01. Intentions.flac", 0x3704927Fu)
NAME_HASH_VEC("ascii-upper", "01. INTENTIONS.FLAC", 0x3704927Fu)
NAME_HASH_VEC("folder-artist-album", "Justin Bieber - Changes", 0x21D67E4Fu)

/* --- smart punctuation folds to ASCII: each pair must collide ---------- */
NAME_HASH_VEC("smart-apostrophe", "Don\342\200\231t Stop", 0x5DF686AFu)
NAME_HASH_VEC("straight-apostrophe", "Don't Stop", 0x5DF686AFu)
NAME_HASH_VEC("smart-double-quote", "\342\200\234Hello\342\200\235", 0xDF47EE8Bu)
NAME_HASH_VEC("straight-double-quote", "\042Hello\042", 0xDF47EE8Bu)
NAME_HASH_VEC("en-dash", "A \342\200\223 B", 0xF0551209u)
NAME_HASH_VEC("em-dash", "A \342\200\224 B", 0xF0551209u)
NAME_HASH_VEC("hyphen", "A - B", 0xF0551209u)

/* --- Latin-1 (2-byte UTF-8). The fold is ASCII-only, so an accented
 *     capital is NOT lowercased — pinned here so nobody "improves" one side
 *     into a Unicode-aware fold without the other. -------------------- */
NAME_HASH_VEC("latin1-accents", "Bj\303\266rk - Vespertine", 0x095F6F0Fu)
NAME_HASH_VEC("latin1-upper-accent", "\303\211douard", 0x665606B4u)

/* --- punctuation soup: nothing here is special-cased ------------------- */
NAME_HASH_VEC("punctuation", "Track #7 (feat. X) [Remix] {2021}!?", 0x7874BD3Fu)

/* --- CJK (3-byte UTF-8), the widest sequence the C encoder handles ------ */
NAME_HASH_VEC("cjk-bmp", "\346\235\261\344\272\254", 0x68DEA76Fu)

/*
 * --- astral plane (4-byte UTF-8) — KNOWN C BUG -------------------------
 *
 * kernel/main.c's name_hash() DECODES a 4-byte sequence correctly
 * (mn_utf8_next handles the 0xF0 lead) but its RE-ENCODER has no 4-byte
 * branch: the final `else` emits a 3-byte sequence unconditionally, so for
 * cp >= 0x10000 it shifts the codepoint into a 3-byte frame and hashes bytes
 * that no encoder would ever produce. tools/build_index.py hashes the real
 * 4-byte UTF-8. The two therefore disagree, and any track whose name contains
 * an emoji or other astral character is written into CORELIB.IDX under a hash
 * the device can never match — the track silently never resolves.
 *
 * The expected values below are the CORRECT ones (what build_index.py
 * produces). The C side is marked XFAIL because the fix belongs in
 * kernel/main.c, which this test does not own.
 */
NAME_HASH_XFAIL("astral-emoji", "\360\237\216\265 track", 0x09055436u,
                "main.c name_hash() re-encodes cp >= 0x10000 as 3 bytes")
NAME_HASH_XFAIL("astral-only", "\360\237\230\200", 0x33A29608u,
                "main.c name_hash() re-encodes cp >= 0x10000 as 3 bytes")

#endif /* CORE_TESTS_KERNEL_NAME_HASH_VECTORS_H */
