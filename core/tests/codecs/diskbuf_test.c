/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/codecs/diskbuf_test.c — host test for the anti-skip disk buffer.
 *
 * The buffer (codecs/diskbuf.c) sits under the read-ahead shim to read the
 * compressed file MEGABYTES ahead in bursts, so the drive head parks between
 * refills. This test backs it with an in-RAM source that COUNTS reads, and
 * checks:
 *   1. Correctness: bytes returned are byte-exact vs the reference across tiny
 *      reads, bulk reads, and SET/CUR/END seeks (the interface the codec uses).
 *   2. Burstiness: with the pump driven, the buffer fills to ~high, then goes
 *      IDLE (pump issues no backing reads) until drained below `low`, then
 *      bursts again — i.e. reads are clustered, not one-per-drain.
 *
 * The code under test is the SAME diskbuf.c the ARM build compiles.
 */

#include "diskbuf.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
static int check(const char *label, int cond)
{
    printf("[%s] %s\n", label, cond ? "PASS" : "FAIL");
    if (!cond) {
        g_fail = 1;
    }
    return cond;
}

/* --- instrumented in-RAM backing source (stands in for the disk) --- */
typedef struct {
    const uint8_t *data;
    size_t         len;
    size_t         pos;
    long           n_reads;
    long           n_seeks;
} counting_src_t;

static size_t cs_read(void *ud, void *buf, size_t n)
{
    counting_src_t *m = (counting_src_t *)ud;
    m->n_reads++;
    size_t avail = (m->pos < m->len) ? (m->len - m->pos) : 0;
    if (n > avail) {
        n = avail;
    }
    memcpy(buf, m->data + m->pos, n);
    m->pos += n;
    return n;
}
static int cs_seek(void *ud, int offset, int origin)
{
    counting_src_t *m = (counting_src_t *)ud;
    m->n_seeks++;
    long base = (origin == DECODER_SEEK_SET) ? 0
              : (origin == DECODER_SEEK_END) ? (long)m->len
                                             : (long)m->pos;
    long np = base + offset;
    if (np < 0 || (size_t)np > m->len) {
        return 0;
    }
    m->pos = (size_t)np;
    return 1;
}
static int64_t cs_tell(void *ud)
{
    return (int64_t)((counting_src_t *)ud)->pos;
}

static void src_init(counting_src_t *m, decoder_source_t *s,
                     const uint8_t *data, size_t len)
{
    m->data = data;
    m->len  = len;
    m->pos  = 0;
    m->n_reads = 0;
    m->n_seeks = 0;
    s->read = cs_read;
    s->seek = cs_seek;
    s->tell = cs_tell;
    s->userdata = m;
}

int main(void)
{
    /* Deterministic pseudo-random reference (LCG — reproducible). */
    enum { LEN = 400000u, CAP = 65536u, LOW = 8192u, HIGH = 57344u };
    static uint8_t ref[LEN];
    static uint8_t buf[CAP];
    uint32_t x = 0x1234567u;
    for (size_t i = 0; i < LEN; i++) {
        x = x * 1103515245u + 12345u;
        ref[i] = (uint8_t)(x >> 16);
    }

    counting_src_t   cs;
    decoder_source_t inner, db_src;
    diskbuf_t        db;

    /* --- Test 1: tiny sequential reads are byte-exact (pump undriven → the
     * synchronous fallback serves them) --- */
    {
        src_init(&cs, &inner, ref, LEN);
        diskbuf_init(&db, &inner, buf, CAP, LOW, HIGH);
        diskbuf_as_source(&db, &db_src);

        int ok = 1;
        size_t total = 0;
        uint8_t tmp[3];
        for (;;) {
            size_t got = db_src.read(db_src.userdata, tmp, sizeof tmp);
            if (got == 0) {
                break;
            }
            if (memcmp(tmp, ref + total, got) != 0) {
                ok = 0;
                break;
            }
            total += got;
        }
        check("tiny-reads-exact", ok && total == LEN);
        check("tiny-reads-tell", db_src.tell(db_src.userdata) == (int64_t)LEN);
    }

    /* --- Test 2: a large bulk read is byte-exact --- */
    {
        src_init(&cs, &inner, ref, LEN);
        diskbuf_init(&db, &inner, buf, CAP, LOW, HIGH);
        diskbuf_as_source(&db, &db_src);

        static uint8_t big[100000];
        size_t got = db_src.read(db_src.userdata, big, sizeof big);
        int ok = (got == sizeof big) && (memcmp(big, ref, sizeof big) == 0);
        check("bulk-read-exact", ok);
        check("bulk-read-tell", db_src.tell(db_src.userdata) == (int64_t)sizeof big);
    }

    /* --- Test 3: SET/CUR/END seeks reposition correctly --- */
    {
        src_init(&cs, &inner, ref, LEN);
        diskbuf_init(&db, &inner, buf, CAP, LOW, HIGH);
        diskbuf_as_source(&db, &db_src);

        uint8_t tmp[64];
        int ok = 1;

        ok &= db_src.seek(db_src.userdata, 200000, DECODER_SEEK_SET);
        ok &= (db_src.read(db_src.userdata, tmp, 64) == 64);
        ok &= (memcmp(tmp, ref + 200000, 64) == 0);

        ok &= db_src.seek(db_src.userdata, 500, DECODER_SEEK_CUR);
        ok &= (db_src.read(db_src.userdata, tmp, 64) == 64);
        ok &= (memcmp(tmp, ref + 200000 + 64 + 500, 64) == 0);

        ok &= db_src.seek(db_src.userdata, 0, DECODER_SEEK_END);
        ok &= (db_src.tell(db_src.userdata) == (int64_t)LEN);
        ok &= (db_src.read(db_src.userdata, tmp, 64) == 0);

        ok &= db_src.seek(db_src.userdata, -64, DECODER_SEEK_END);
        ok &= (db_src.read(db_src.userdata, tmp, 64) == 64);
        ok &= (memcmp(tmp, ref + LEN - 64, 64) == 0);

        check("seeks-exact", ok);
    }

    /* --- Test 4: a backward seek within the buffered window costs no disk --- */
    {
        src_init(&cs, &inner, ref, LEN);
        diskbuf_init(&db, &inner, buf, CAP, LOW, HIGH);
        diskbuf_as_source(&db, &db_src);

        uint8_t tmp[16];
        db_src.read(db_src.userdata, tmp, 16);       /* primes a window at 0 */
        long reads_after_prime = cs.n_reads;

        int ok = 1;
        db_src.seek(db_src.userdata, 4, DECODER_SEEK_SET);
        ok &= (db_src.read(db_src.userdata, tmp, 16) == 16);
        ok &= (memcmp(tmp, ref + 4, 16) == 0);
        ok &= (cs.n_reads == reads_after_prime);     /* served from RAM */
        check("window-reread-no-disk", ok);
    }

    /* --- Test 5: the pump is BURSTY — it fills to ~high then idles, and the
     * backing reads cluster instead of one-per-drain --- */
    {
        src_init(&cs, &inner, ref, LEN);
        diskbuf_init(&db, &inner, buf, CAP, LOW, HIGH);
        diskbuf_as_source(&db, &db_src);

        /* Pump with nothing consumed: it should burst up to >= HIGH then stop. */
        int burst_reads = 0;
        for (int i = 0; i < 200; i++) {
            if (diskbuf_pump(&db, 8192u) > 0) {
                burst_reads++;
            }
        }
        int filled_high = diskbuf_fill_ahead(&db) >= HIGH;
        /* The burst is what the whole buffer exists for: the fill must take a
         * bounded run of productive pumps and then STOP, not dribble one read
         * out of every call forever. */
        int bursty = (burst_reads > 0 && burst_reads < 200);
        /* Once topped out, further pumps must be no-ops (drive idle). */
        long reads_before_idle = cs.n_reads;
        for (int i = 0; i < 50; i++) {
            diskbuf_pump(&db, 8192u);
        }
        int idle_when_full = (cs.n_reads == reads_before_idle);
        check("pump-fills-to-high", filled_high);
        check("pump-burst-then-stops", bursty);
        check("pump-idle-when-full", idle_when_full);

        /* Drain a little (above low): pump must STAY idle — the hysteresis. */
        uint8_t sink[4096];
        db_src.read(db_src.userdata, sink, sizeof sink);   /* fill_ahead still > LOW */
        long reads_before = cs.n_reads;
        diskbuf_pump(&db, 8192u);
        check("pump-idle-above-low", cs.n_reads == reads_before);

        /* Drain below low: the next pump must WAKE and burst again. */
        while (diskbuf_fill_ahead(&db) >= LOW) {
            db_src.read(db_src.userdata, sink, sizeof sink);
        }
        long reads_at_low = cs.n_reads;
        int refill = 0;
        for (int i = 0; i < 200; i++) {
            if (diskbuf_pump(&db, 8192u) > 0) {
                refill++;
            }
        }
        check("pump-refills-below-low", cs.n_reads > reads_at_low && refill > 0);
        check("pump-refills-to-high", diskbuf_fill_ahead(&db) >= HIGH);
    }

    /* --- Test 6: full drive-through the pump is byte-exact (interleave reads
     * and pumps like the player's main loop, then read to EOF) --- */
    {
        src_init(&cs, &inner, ref, LEN);
        diskbuf_init(&db, &inner, buf, CAP, LOW, HIGH);
        diskbuf_as_source(&db, &db_src);

        int ok = 1;
        size_t total = 0;
        uint8_t tmp[277];                    /* odd size to exercise wrap */
        for (;;) {
            /* pump a couple of chunks each "loop pass" */
            diskbuf_pump(&db, 8192u);
            diskbuf_pump(&db, 8192u);
            size_t got = db_src.read(db_src.userdata, tmp, sizeof tmp);
            if (got == 0) {
                break;
            }
            if (memcmp(tmp, ref + total, got) != 0) {
                ok = 0;
                break;
            }
            total += got;
        }
        check("pumped-drivethrough-exact", ok && total == LEN);
    }

    printf("diskbuf_test: %s\n", g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}
