/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/xfail.h — assertions that document a KNOWN bug instead of hiding it.
 *
 * Some of the tests in this tree were written against the *documented*
 * contract (hal/hal.h, fs/fat32.h) rather than against what the code happens
 * to do today, because the contract is what the rest of the firmware relies
 * on. Where the current implementation does not yet meet it, and the fix lives
 * in a file the test does not own, deleting or weakening the assertion would
 * lose the finding. Instead:
 *
 *   XPECT(...)  — a normal assertion; failing fails the binary.
 *   XFAIL(...)  — an assertion known to fail today. A failure prints XFAIL
 *                 with the reason and does NOT fail the binary; a PASS prints
 *                 a loud XPASS telling you to promote it to XPECT.
 *
 * Set CORE_TEST_STRICT_XFAIL=1 in the environment to turn every XFAIL into a
 * hard failure — use it to check whether a fix has landed, and in CI once the
 * known bugs are cleared.
 */
#ifndef CORE_TESTS_XFAIL_H
#define CORE_TESTS_XFAIL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *suite;
    int fails;     /* real failures                                   */
    int xfails;    /* known bugs, still broken (informational)        */
    int xpasses;   /* known bugs that now pass (promote the assertion) */
} xfail_ctx;

static inline int xfail_strict(void)
{
    const char *e = getenv("CORE_TEST_STRICT_XFAIL");
    return e != NULL && strcmp(e, "0") != 0 && e[0] != '\0';
}

static inline void xpect(xfail_ctx *c, const char *label, int cond)
{
    if (cond) {
        printf("[%s %s] PASS\n", c->suite, label);
        return;
    }
    fprintf(stderr, "[%s %s] FAIL\n", c->suite, label);
    c->fails++;
}

/* `why` must name the file that owns the fix, so the report is actionable. */
static inline void xfail(xfail_ctx *c, const char *label, int cond,
                         const char *why)
{
    if (!cond) {
        if (xfail_strict()) {
            fprintf(stderr, "[%s %s] FAIL (strict xfail) — %s\n",
                    c->suite, label, why);
            c->fails++;
            return;
        }
        printf("[%s %s] XFAIL — known bug: %s\n", c->suite, label, why);
        c->xfails++;
        return;
    }
    printf("[%s %s] XPASS — the known bug appears FIXED (%s).\n"
           "    ACTION: promote this XFAIL to XPECT so it stays fixed.\n",
           c->suite, label, why);
    c->xpasses++;
}

/* Returns the process exit status and prints the one-line summary. */
static inline int xfail_done(xfail_ctx *c)
{
    printf("%s: %d failure(s), %d known-bug xfail(s), %d xpass(es)\n",
           c->suite, c->fails, c->xfails, c->xpasses);
    return c->fails == 0 ? 0 : 1;
}

#endif /* CORE_TESTS_XFAIL_H */
