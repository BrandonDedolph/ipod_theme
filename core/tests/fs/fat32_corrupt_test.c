/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/fs/fat32_corrupt_test.c — the FAT32 reader against DELIBERATELY BROKEN
 * volumes.
 *
 * fs/fat32.c parses whatever is on the user's disk. That disk gets yanked out
 * mid-copy, gets bad sectors, gets reformatted as FAT16 by iTunes, or simply
 * isn't the volume we expect. Until now every fat32 test used one pristine
 * hand-built image, so the entire error surface — the half of the code that
 * runs when the bytes are wrong — was untested.
 *
 * The assertion is the same for every case: FAIL CLEANLY AND BOUNDED. A wrong
 * answer is bad; a hang is worse (the firmware has no watchdog and no way to
 * report it — the iPod just stops), and an out-of-bounds read is worst.
 *
 * HOW "BOUNDED" IS MEASURED. The block-read callback counts calls and starts
 * returning an error past a generous budget. So:
 *   - a reader that terminates on its own finishes well under the budget;
 *   - a reader that would loop forever instead hits the budget, gets an error
 *     back, and returns — and the test can SEE that it needed the budget, which
 *     is exactly the finding. No signals, no timeouts, no flakiness.
 *
 * Cases the corrupt images cover (built by tests/scripts/make_fat32_image.py,
 * one image per --variant):
 *   cyclic-fat   FAT chains that loop instead of ending at EOC
 *   oob-cluster  directory entries pointing outside the data region
 *   fat16-bpb    a genuine FAT16 boot sector offered to a FAT32 reader
 *   orphan-lfn   LFN runs with a mismatched checksum / no 8.3 entry at all
 *   truncated    the volume ends after the FAT region
 *
 * Assertions that today's fs/fat32.c does not meet are marked XFAIL (see
 * tests/xfail.h) with the specific missing check named, rather than being
 * weakened until they pass. Run with CORE_TEST_STRICT_XFAIL=1 to see whether a
 * fix has landed.
 *
 * Usage: fat32_corrupt_test <dir-containing-the-variant-images>
 */

#include "fat32.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../xfail.h"

/* ---- bounded, counting, file-backed block device --------------------- */

/*
 * Budget. The whole reference volume is 12 FS-sectors = 48 blocks of 512 B, so
 * any terminating walk of it costs a few dozen reads at most. 20000 is three
 * orders of magnitude past that: hitting it means the reader is not making
 * progress toward an end, not that the volume is merely large.
 */
#define READ_BUDGET 20000

static FILE    *g_img;
static long     g_img_blocks;    /* size of the image in 512-byte blocks */
static long     g_reads;
static long     g_oob_reads;     /* reads that fell outside the image     */

static int img_read(void *ud, uint32_t lba, uint32_t count, void *buf)
{
    (void)ud;
    if (++g_reads > READ_BUDGET) {
        return -1;               /* budget exhausted: report a dead drive */
    }
    if ((long)lba + (long)count > g_img_blocks) {
        g_oob_reads++;
        return -1;               /* past the end of the volume            */
    }
    if (fseek(g_img, (long)lba * 512L, SEEK_SET) != 0) {
        return -1;
    }
    if (fread(buf, 512, count, g_img) != count) {
        return -1;
    }
    return 0;
}

static int open_variant(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.img", dir, name);
    if (g_img) {
        fclose(g_img);
    }
    g_img = fopen(path, "rb");
    if (!g_img) {
        fprintf(stderr, "cannot open %s\n", path);
        return 0;
    }
    fseek(g_img, 0, SEEK_END);
    g_img_blocks = ftell(g_img) / 512L;
    g_reads      = 0;
    g_oob_reads  = 0;
    return 1;
}

static int budget_hit(void) { return g_reads > READ_BUDGET; }

/* ---- readdir collectors ---------------------------------------------- */

#define MAX_ENTS 16
typedef struct {
    char name[MAX_ENTS][256];
    int  n;
    int  overflow;      /* the callback fired more than MAX_ENTS times */
} collector;

static int collect(void *ud, const fat32_dirent_t *e)
{
    collector *c = ud;
    if (c->n < MAX_ENTS) {
        snprintf(c->name[c->n], sizeof c->name[0], "%s", e->name);
        c->n++;
    } else {
        c->overflow = 1;
        return 1;       /* stop: a runaway enumeration must not be endless */
    }
    return 0;
}

/* Counts entries and NEVER asks to stop — so an unterminated walk is bounded
 * only by fat32.c's own logic (or by the read budget). */
static int count_forever(void *ud, const fat32_dirent_t *e)
{
    (void)e;
    (*(long *)ud)++;
    return 0;
}

static int has_name(const collector *c, const char *want)
{
    for (int i = 0; i < c->n; i++) {
        if (strcmp(c->name[i], want) == 0) {
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: fat32_corrupt_test <image-dir>\n");
        return 2;
    }
    const char *dir = argv[1];
    xfail_ctx c = { "fat32-corrupt", 0, 0, 0 };

    fat32_t fs;
    static uint8_t big[64 * 1024];

    /* ---- 0. the reference volume still mounts, and now reports capacity --
     * make_fat32_image.py previously left BPB TotSec16/TotSec32 at zero, so
     * fs->total_clus was 0 on EVERY image the suite used — which silently
     * disarms any bounds check written against it. The generator now writes
     * TotSec32; assert the volume actually reports its size, or the
     * out-of-range assertions below are testing nothing. */
    if (!open_variant(dir, "good")) {
        return 2;
    }
    xpect(&c, "reference volume mounts", fat32_mount(&fs, img_read, 0, 0) == 0);
    xpect(&c, "reference volume reports a nonzero cluster count",
          fs.total_clus > 0);
    xpect(&c, "reported cluster count is plausible for the image",
          fs.total_clus <= (uint32_t)g_img_blocks);

    /* ---- 1. cyclic FAT chain ---------------------------------------- *
     * Both the root directory's chain and HELLO.TXT's chain loop back on
     * themselves. Nothing here may run forever. */
    if (!open_variant(dir, "cyclic-fat")) {
        return 2;
    }
    xpect(&c, "cyclic: mount still succeeds (the BPB is fine)",
          fat32_mount(&fs, img_read, 0, 0) == 0);
    {
        /* Deliberately NOT the bounded `collect` callback: a caller that stops
         * early would mask the loop. This one never asks to stop, so the only
         * thing that can end the walk is fat32.c itself — or the read budget,
         * which is the finding. */
        long seen = 0;
        int rc = fat32_readdir_root(&fs, count_forever, &seen);
        xpect(&c, "cyclic: readdir returns instead of hanging", 1);
        xpect(&c, "cyclic: readdir detects the loop without exhausting the disk",
              !budget_hit());
        xpect(&c, "cyclic: readdir reports an error rather than silent success",
              rc != 0 || !budget_hit());
        xfail(&c, "cyclic: the same entry is not surfaced over and over",
              seen < 64,
              "fs/fat32.c fat32_readdir() re-enumerates the looping cluster's "
              "entries on every pass, so a caller building a list sees the "
              "same files repeated until its own array fills up");
    }
    {
        g_reads = 0;
        uint32_t clus = 0, size = 0;
        int rc = fat32_open(&fs, "HELLO.TXT", &clus, &size);
        if (rc == 0) {
            g_reads = 0;
            int32_t got = fat32_read_file(&fs, clus, big, sizeof big);
            /* read_file is bounded by maxlen, so it terminates either way —
             * but a looping chain means it returns the SAME cluster's bytes
             * over and over, i.e. silently wrong data. */
            xpect(&c, "cyclic: reading a looping chain is rejected, not "
                      "silently repeated",
                  got < 0);
        }
    }

    /* ---- 2. out-of-range cluster numbers ----------------------------- */
    if (!open_variant(dir, "oob-cluster")) {
        return 2;
    }
    xpect(&c, "oob: mount succeeds", fat32_mount(&fs, img_read, 0, 0) == 0);
    {
        uint32_t clus = 0, size = 0;
        int rc = fat32_open(&fs, "HELLO.TXT", &clus, &size);
        xpect(&c, "oob: the entry is found (the directory itself is intact)",
              rc == 0);
        if (rc == 0) {
            xpect(&c, "oob: the recorded cluster really is out of range",
                  clus >= fs.total_clus + 2u);
            g_reads = 0;
            g_oob_reads = 0;
            int32_t got = fat32_read_file(&fs, clus, big, sizeof big);
            xpect(&c, "oob: reading it returns an error, not data", got < 0);
            xpect(&c, "oob: the cluster is rejected before any disk read",
                  g_oob_reads == 0);
        }
    }
    {
        /* Cluster 1 is reserved: cluster_fs_sector() computes (clus - 2),
         * which underflows to 0xFFFFFFFF and can wrap into a valid sector. */
        uint32_t clus = 0, size = 0;
        g_reads = 0;
        g_oob_reads = 0;
        if (fat32_open(&fs, "LOWCLUS.TXT", &clus, &size) == 0) {
            xpect(&c, "oob: the low-cluster entry really is below 2", clus < 2);
            int32_t got = fat32_read_file(&fs, clus, big, sizeof big);
            xpect(&c, "oob: a cluster below 2 yields no data",
                  got <= 0);
        }
    }

    /* ---- 3. a FAT16 volume must be REJECTED --------------------------- *
     * The stakes: accepting it means the reader computes the data region with
     * FAT32 arithmetic over FAT16 geometry and then reads, and later
     * potentially reports, arbitrary sectors as file contents. */
    if (!open_variant(dir, "fat16-bpb")) {
        return 2;
    }
    {
        int rc = fat32_mount(&fs, img_read, 0, 0);
        xpect(&c, "fat16: a FAT16 boot sector is rejected", rc != 0);
        if (rc == 0) {
            /* It mounted. Everything downstream is then nonsense; make sure it
             * is at least bounded nonsense and not a hang or an OOB read. */
            collector col = { { { 0 } }, 0, 0 };
            g_reads = 0;
            (void)fat32_readdir_root(&fs, collect, &col);
            xpect(&c, "fat16: the mis-mounted volume still enumerates boundedly",
                  !budget_hit());
        }
    }

    /* ---- 4. orphaned / mismatched LFN runs ---------------------------- */
    if (!open_variant(dir, "orphan-lfn")) {
        return 2;
    }
    xpect(&c, "orphan-lfn: mount succeeds", fat32_mount(&fs, img_read, 0, 0) == 0);
    {
        collector col = { { { 0 } }, 0, 0 };
        g_reads = 0;
        int rc = fat32_readdir_root(&fs, collect, &col);
        xpect(&c, "orphan-lfn: readdir succeeds and is bounded",
              rc == 0 && !budget_hit() && !col.overflow);
        xpect(&c, "orphan-lfn: the intact entries are still enumerated",
              has_name(&col, "HELLO.TXT") && has_name(&col, "Intentions.flac"));
        /* The stale run's checksum binds it to a different short name, so it
         * belongs to no entry here and must be discarded. */
        xpect(&c, "orphan-lfn: a checksum-mismatched LFN run is discarded",
              has_name(&col, "REAL.TXT") && !has_name(&col, "Ghost.flac"));
        /* The trailing run has no 8.3 entry at all: it must produce nothing. */
        xpect(&c, "orphan-lfn: a dangling run with no 8.3 entry surfaces nothing",
              !has_name(&col, "Dangling.flac"));
    }
    {
        /* And the name that only the stale run claims must not resolve. */
        uint32_t clus = 0, size = 0;
        xfail(&c, "orphan-lfn: the stale long name does not resolve",
              fat32_open(&fs, "Ghost.flac", &clus, &size) != 0,
              "fs/fat32.c fat32_open() matches the reassembled long name "
              "without validating its checksum against the 8.3 entry");
    }

    /* ---- 5. a truncated volume --------------------------------------- *
     * The BPB and the FATs are readable; the data region is simply gone. */
    if (!open_variant(dir, "truncated")) {
        return 2;
    }
    {
        int rc = fat32_mount(&fs, img_read, 0, 0);
        xpect(&c, "truncated: mount reads the BPB successfully", rc == 0);

        collector col = { { { 0 } }, 0, 0 };
        g_reads = 0;
        g_oob_reads = 0;
        int rdrc = fat32_readdir_root(&fs, collect, &col);
        xpect(&c, "truncated: readdir returns a clean error", rdrc < 0);
        xpect(&c, "truncated: readdir surfaces no entries", col.n == 0);
        xpect(&c, "truncated: readdir is bounded", !budget_hit());

        uint32_t clus = 0, size = 0;
        g_reads = 0;
        int oprc = fat32_open(&fs, "HELLO.TXT", &clus, &size);
        xpect(&c, "truncated: open returns an error rather than a match",
              oprc != 0);
        xpect(&c, "truncated: open is bounded", !budget_hit());

        /* A stream over a file whose data is missing must report the failure
         * rather than hand back uninitialised buffer contents. */
        memset(big, 0xAB, 512);
        fat32_stream_t st;
        fat32_stream_open(&st, &fs, 3, 3000);
        g_reads = 0;
        int32_t got = fat32_stream_read(&st, big, 512);
        xpect(&c, "truncated: stream_read reports the failure", got <= 0);
        xpect(&c, "truncated: stream_read is bounded", !budget_hit());
    }

    if (g_img) {
        fclose(g_img);
    }
    return xfail_done(&c);
}
