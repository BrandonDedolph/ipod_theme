/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/kernel/name_hash_ref.c — a VERBATIM COPY of the name_hash() locator
 * from core/kernel/main.c, so it can be host-tested.
 *
 * WHY A COPY: name_hash() and its helper mn_utf8_next() are `static` inside
 * kernel/main.c (a 3500-line file that also pulls in the whole UI, the FAT
 * reader and the HAL), so there is no way to link the real one into a host
 * test without changing main.c. The hash is the SOLE binding between a
 * CORELIB.IDX record and the file on disk, and a second implementation of it
 * lives in tools/build_index.py — three copies of one function, previously
 * with no test at all.
 *
 * THE COPY IS LOAD-BEARING AND MUST NOT DRIFT. Everything between the
 * BEGIN/END markers below is byte-identical to main.c and is diffed against it
 * by tests/scripts/check_name_hash_parity.py (run from `meson test` and from
 * `make verify-hw`). If you change main.c's hash, paste the new text in here;
 * if you "fix" a bug here without fixing main.c, the parity check fails.
 * Do not reformat, re-indent, or rename anything inside the markers.
 */

#include <stdint.h>

#include "name_hash_ref.h"

/* ---- BEGIN VERBATIM COPY OF core/kernel/main.c ------------------------- */

static int mn_utf8_next(const unsigned char **p)
{
    unsigned char c = **p;
    if (c == 0) return -1;
    if (c < 0x80) { (*p)++; return c; }
    int n, cp;
    if      ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
    else { (*p)++; return 0xFFFD; }
    const unsigned char *q = *p + 1;
    for (int i = 0; i < n; i++) {
        if ((q[i] & 0xC0) != 0x80) { (*p)++; return 0xFFFD; }
        cp = (cp << 6) | (q[i] & 0x3F);
    }
    *p += n + 1;
    return cp;
}

static uint32_t name_hash(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    uint32_t h = 0x811c9dc5u;
    for (;;) {
        int cp = mn_utf8_next(&p);
        if (cp < 0) break;
        if      (cp == 0x2018 || cp == 0x2019) cp = '\'';
        else if (cp == 0x201C || cp == 0x201D) cp = '"';
        else if (cp == 0x2013 || cp == 0x2014) cp = '-';
        if (cp >= 'A' && cp <= 'Z') cp += 32;
        unsigned char b[4]; int n;
        if      (cp < 0x80)  { b[0] = (unsigned char)cp; n = 1; }
        else if (cp < 0x800) { b[0] = (unsigned char)(0xC0 | (cp >> 6));
                               b[1] = (unsigned char)(0x80 | (cp & 0x3F)); n = 2; }
        else                 { b[0] = (unsigned char)(0xE0 | (cp >> 12));
                               b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                               b[2] = (unsigned char)(0x80 | (cp & 0x3F)); n = 3; }
        for (int i = 0; i < n; i++) { h ^= b[i]; h *= 0x01000193u; }
    }
    return h;
}

/* ---- END VERBATIM COPY OF core/kernel/main.c --------------------------- */

/* Non-static entry point for the test (outside the copied region). */
uint32_t name_hash_ref(const char *s)
{
    return name_hash(s);
}
