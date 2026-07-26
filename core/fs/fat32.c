/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/fs/fat32.c — minimal read-only FAT32 reader (see fat32.h).
 *
 * Portable: no hardware access, no libc. All disk I/O is the caller's
 * 512-byte block-read callback; the volume's BytesPerSector (e.g. 2048 on
 * the stock iPod 80 GB) is translated to 512-byte units here.
 *
 * Two rules this file now enforces everywhere, because the device has no
 * watchdog, no debugger and no serial cable — an unbounded loop or a wild
 * LBA is a brick until the battery is pulled:
 *   - EVERY cluster number is validated (cluster_valid) before it is turned
 *     into a sector address, so a corrupt FAT entry can never be multiplied
 *     into an arbitrary LBA.
 *   - EVERY chain walk is bounded, so a cyclic chain returns FAT32_ECORRUPT
 *     instead of spinning forever.
 */

#include "fat32.h"

/* memcpy: lib/mem.c's word-optimised one on bare metal (roughly 4x a byte
 * loop on ARM), libc's in the host tests. Same declaration either way. */
#include "../lib/mem.h"

/* End-of-cluster-chain marker (FAT32 entries are 28-bit). */
#define FAT_EOC 0x0FFFFFF8u

/*
 * Largest cluster number we will ever accept, as a hard backstop on top of
 * the per-volume ceiling computed at mount (fs->max_clus). FAT32 entries are
 * 28-bit and 0x0FFFFFF0 up is reserved/EOC/bad, so nothing above this is a
 * data cluster under any geometry.
 */
#define FAT_MAX_CLUS_CAP 0x0FFFFFF0u

/*
 * Iteration ceiling for a DIRECTORY cluster chain, and how it is derived.
 *
 * FAT32 caps a directory at 65536 32-byte entries = 2 MB, so the honest
 * bound is "2 MB worth of clusters" — 64 clusters at the stock 32 KB cluster
 * size. We allow 2x that for slack (FAT_DIR_MAX_BYTES) and cap the result at
 * FAT_DIR_MAX_CLUS so a pathological tiny-cluster geometry still can't turn
 * into a long scan. That converts a self-referential directory entry —
 * previously an infinite loop, with IRQs on, no watchdog and no way out but
 * a battery pull — into a scan that gives up in well under a second and
 * returns FAT32_ECORRUPT.
 */
#define FAT_DIR_MAX_BYTES (4u * 1024u * 1024u)
#define FAT_DIR_MAX_CLUS  4096u

static uint32_t dir_walk_limit(const fat32_t *fs)
{
    uint32_t n = FAT_DIR_MAX_BYTES / fs->clus_bytes + 1u;
    return (n > FAT_DIR_MAX_CLUS) ? FAT_DIR_MAX_CLUS : n;
}

/* One-sector FAT cache — the fix for burst-seeking during playback.
 *
 * next_cluster() is called at every cluster boundary while streaming a file,
 * and without a cache it re-reads a FAT sector each time. The FAT lives near
 * the start of the partition while the file's data lives far into the data
 * region, so every one of those lookups seeks the head back to the FAT and
 * then back to the data — hundreds of head seeks interleaved through a single
 * multi-MB read-ahead burst (what you feel as the drive "seeking" mid-play).
 *
 * One FS-sector holds bytes_per_sec/4 FAT entries (512 on a 2048-byte volume),
 * i.e. the chain for ~16 MB of a contiguous file. Caching it collapses those
 * hundreds of FAT re-reads into ONE read per sector's worth of chain, so a
 * refill becomes: seek to the FAT once, then stream the data region. Tagged by
 * the fs pointer so a second mounted volume can never serve a stale sector.
 * Kept separate from the data cache below, which the partial-copy paths tag
 * with data-region sectors. The volume is read-only, so neither cache ever
 * needs write invalidation. */
static uint8_t   fat_cache[4096];
static fat32_t  *fat_cache_fs    = 0;
static uint32_t  fat_cache_sec   = 0;
static int       fat_cache_valid = 0;

/* One-sector DATA cache — the same trick as the FAT cache above, applied to
 * the partial-copy paths (an unaligned head, a sub-sector tail, or a small
 * request that never covers a whole FS-sector).
 *
 * Those paths used to read a whole FS-sector from the platter for EVERY
 * partial copy, with no memory of the previous one. On the stock 2048-byte
 * volume a caller reading 100 bytes at a time pulled 2048 bytes per call —
 * 20x amplification — and consecutive calls inside the SAME sector re-read
 * it from disk every single time. Three separate layers upstream (the 32 KB
 * read-ahead shim, the MB-scale disk buffer, the index loader's explicit
 * 16 KB batching workaround) exist partly to paper over that; this fixes it
 * at the source: consecutive sub-sector reads inside one sector now cost one
 * disk read total.
 *
 * Tagged by (fs, FS-sector) exactly like the FAT cache, so a second mounted
 * volume can never be served a stale sector. The volume is read-only, so no
 * write invalidation is needed — a write path landing later (ata.c's
 * appended write primitive is not wired to anything yet) MUST invalidate
 * both this and fat_cache. */
static uint8_t   dat_cache[4096];
static fat32_t  *dat_cache_fs    = 0;
static uint32_t  dat_cache_sec   = 0;
static int       dat_cache_valid = 0;

/* ---- little helpers -------------------------------------------------- */

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static char upcase(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

/* Read one FS-sector (relative to the partition) into `buf` (which must
 * hold bytes_per_sec bytes), via the caller's 512-byte-sector callback. */
static int read_fs_sector(fat32_t *fs, uint32_t fs_sec, void *buf)
{
    return fs->read(fs->ud, fs->part_lba + fs_sec * fs->sec_ratio,
                    fs->sec_ratio, buf);
}

/* Read one FS-sector of FILE DATA through the data-sector cache. On success
 * *out points at the cached sector (valid until the next data read) and 0 is
 * returned; -1 on a disk read error. NOT re-entrant by design: the caller
 * must finish copying out of *out before it reads again. */
static int read_data_sector(fat32_t *fs, uint32_t fs_sec, const uint8_t **out)
{
    if (!(dat_cache_valid && dat_cache_fs == fs && dat_cache_sec == fs_sec)) {
        if (read_fs_sector(fs, fs_sec, dat_cache) != 0) {
            dat_cache_valid = 0;
            return -1;
        }
        dat_cache_fs    = fs;
        dat_cache_sec   = fs_sec;
        dat_cache_valid = 1;
    }
    *out = dat_cache;
    return 0;
}

/*
 * Is `clus` a data cluster this volume can actually address?
 *
 * Everything downstream depends on this. cluster_fs_sector() computes
 * data_start + (clus-2)*sec_per_clus, which OVERFLOWS uint32_t for a large
 * cluster number and then yields an arbitrary LBA — so a single flipped bit
 * in the FAT during a track read used to silently return data from an
 * unrelated part of the disk instead of failing. The ceiling (fs->max_clus,
 * computed at mount) plus the explicit no-wrap test below make that
 * impossible: a bad cluster number is now rejected, not followed.
 */
static int cluster_valid(const fat32_t *fs, uint32_t clus)
{
    if (clus < 2u || clus >= fs->max_clus) {
        return 0;
    }
    /* data_start + (clus-2)*sec_per_clus must stay inside uint32_t. */
    if ((clus - 2u) > (0xFFFFFFFFu - fs->data_start) / fs->sec_per_clus) {
        return 0;
    }
    return 1;
}

/* First FS-sector of a data cluster (clusters are numbered from 2). Callers
 * must have passed `clus` through cluster_valid() first — that is what
 * guarantees this multiply-add cannot wrap. */
static uint32_t cluster_fs_sector(const fat32_t *fs, uint32_t clus)
{
    return fs->data_start + (clus - 2u) * fs->sec_per_clus;
}

/* Follow the FAT: next cluster after `clus`. Returns 0 on read error, and
 * >= FAT_EOC at the end of the chain — INCLUDING when the FAT entry is not a
 * valid data cluster (free, reserved, out of range, or pointing past the end
 * of the volume). Previously the raw 28-bit value was handed straight back
 * with no comparison against the volume's capacity at all. FAT entries never
 * cross an FS-sector boundary (bytes_per_sec is a multiple of 4). */
static uint32_t next_cluster(fat32_t *fs, uint32_t clus)
{
    if (!cluster_valid(fs, clus)) {
        return FAT_EOC;
    }

    uint32_t byte_off = clus * 4u;
    uint32_t fs_sec   = fs->fat_start + byte_off / fs->bytes_per_sec;
    uint32_t in_off   = byte_off % fs->bytes_per_sec;

    /* The entry has to live inside the FAT region (which ends where the data
     * region starts). cluster_valid() already implies this for a sane BPB;
     * the check costs nothing and keeps a corrupt geometry from turning a
     * FAT lookup into a read of file data. */
    if (fs_sec >= fs->data_start) {
        return FAT_EOC;
    }

    /* Serve from the FAT cache when it already holds this sector for this
     * volume; otherwise fetch it once and tag it. This is what keeps the
     * head parked over the data region through a whole read-ahead burst. */
    if (!(fat_cache_valid && fat_cache_fs == fs && fat_cache_sec == fs_sec)) {
        if (read_fs_sector(fs, fs_sec, fat_cache) != 0) {
            fat_cache_valid = 0;
            return 0;
        }
        fat_cache_fs    = fs;
        fat_cache_sec   = fs_sec;
        fat_cache_valid = 1;
    }

    uint32_t nxt = rd32(&fat_cache[in_off]) & 0x0FFFFFFFu;
    return cluster_valid(fs, nxt) ? nxt : FAT_EOC;
}

/* Case-insensitive ASCII compare of two NUL-terminated names. Callers only
 * ever look up ASCII names (CORELIB.IDX, folder.art, a track filename), so a
 * byte compare with ASCII case folding is exactly right: a UTF-8 multibyte
 * name simply can't equal an ASCII request. */
static int name_eq_ci(const char *a, const char *b)
{
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        if (upcase(*a) != upcase(*b)) {
            return 0;
        }
    }
    return *a == '\0' && *b == '\0';
}

/* Format a raw 11-byte on-disk 8.3 field into a NUL-terminated display name.
 * The stored form is space-padded and split base(8)+ext(3) with no dot
 * ("HELLO   TXT", "README     "); enumeration needs the human "NAME.EXT"
 * form. Trailing spaces are trimmed from the base and the extension, and the
 * '.' is inserted only when an extension actually survives — so a dotless
 * name stays dotless. `out` must hold at least 13 bytes (8 + 1 + 3 + NUL). */
static void fmt_83(const uint8_t raw[11], char *out)
{
    int o = 0;

    int base_len = 8;
    while (base_len > 0 && raw[base_len - 1] == ' ') {
        base_len--;
    }
    for (int i = 0; i < base_len; i++) {
        out[o++] = (char)raw[i];
    }

    int ext_len = 3;
    while (ext_len > 0 && raw[8 + ext_len - 1] == ' ') {
        ext_len--;
    }
    if (ext_len > 0) {
        out[o++] = '.';
        for (int i = 0; i < ext_len; i++) {
            out[o++] = (char)raw[8 + i];
        }
    }
    out[o] = '\0';
}

/* ---- VFAT long-filename (LFN) reassembly ----------------------------- */
/*
 * A long name is stored in one or more 0x0F-attribute entries that PRECEDE
 * the file's 8.3 entry, in reverse order (highest sequence first). Each LFN
 * entry carries 13 UTF-16LE code units at byte offsets 1..10, 14..25, 28..31,
 * a 1-based sequence number in byte 0 (bit 0x40 marks the last/first-logical
 * piece), and the 8.3 checksum in byte 13. We accumulate the pieces by their
 * sequence index into one flat ASCII buffer, then match the caller's ASCII
 * name against it. Non-ASCII code points (> 0x7F) become a sentinel that can
 * never match an ASCII request; names longer than the cap are marked unusable
 * and simply fall back to the 8.3 match.
 */
#define FAT_LFN_MAX 128  /* longest long-name we reassemble (chars). Must clear
                          * the longest real "NN. Title.flac" — a feature-heavy
                          * title like "16. TRAGIC (feat. Youngboy Never Broke
                          * Again & Internet Money).flac" is 67 chars; at 64 the
                          * reassembly gave up and fell back to the ugly 8.3
                          * short name ("16TRAG~1"), which then failed to match
                          * the library index and lost the track's metadata.
                          * 128 covers any realistic track name; the buffer
                          * (a stack local) stays small for a freestanding build. */

/* Byte offset of each of the 13 chars inside a 32-byte LFN entry. */
static const uint8_t lfn_pos[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};

typedef struct {
    uint16_t lfn[FAT_LFN_MAX]; /* assembled long name as UTF-16 code units (BMP;
                                * UTF-8-encoded when the dirent is built)       */
    int  max_idx;           /* highest slot written, -1 if none               */
    int  term;              /* terminator (0x0000) position, -1 if none       */
    int  bad;               /* saw an out-of-range piece -> unusable          */
    int  have_sum;          /* at least one fragment contributed a checksum   */
    uint8_t sum;            /* 8.3 checksum every fragment in the run carries */
} lfn_acc_t;

/*
 * Standard VFAT 8.3 checksum (byte 13 of every LFN entry): the ONE field
 * that binds a long-name run to its short entry. We used to ignore it
 * entirely, which meant a stray orphaned 0x0F entry — say seq 5, writing
 * slots 52..64 — made lfn_length() report 65 and lfn_to_utf8() emit ~52
 * characters of whatever happened to be on the stack as a filename.
 * Validating it (plus zeroing the accumulator in lfn_reset) closes that off.
 */
static uint8_t lfn_checksum(const uint8_t name83[11])
{
    uint8_t s = 0;
    for (int i = 0; i < 11; i++) {
        s = (uint8_t)(((s & 1u) << 7) + (s >> 1) + name83[i]);
    }
    return s;
}

static void lfn_reset(lfn_acc_t *a)
{
    a->max_idx  = -1;
    a->term     = -1;
    a->bad      = 0;
    a->have_sum = 0;
    a->sum      = 0;
    /* Zero the code units too. `lfn_acc_t acc` is a plain stack local in the
     * directory walk, so without this a run with holes in it named the file
     * after uninitialised stack. */
    memset(a->lfn, 0, sizeof a->lfn);
}

/* Fold one 0x0F LFN entry into the accumulator. */
static void lfn_add(lfn_acc_t *a, const uint8_t *e)
{
    uint32_t seq = (uint32_t)(e[0] & 0x3Fu);   /* strip the 0x40 last-marker */
    if (seq == 0) {
        a->bad = 1;                            /* not a valid sequence index */
        return;
    }

    /* Every fragment of one run carries the same 8.3 checksum; a change of
     * checksum mid-run means these fragments do not belong together. */
    uint8_t sum = e[13];
    if (!a->have_sum) {
        a->sum      = sum;
        a->have_sum = 1;
    } else if (a->sum != sum) {
        a->bad = 1;
        return;
    }

    uint32_t base = (seq - 1u) * 13u;
    for (int k = 0; k < 13; k++) {
        uint16_t u   = rd16(&e[lfn_pos[k]]);
        uint32_t idx = base + (uint32_t)k;
        if (u == 0x0000) {                     /* name terminator */
            if (a->term < 0 || (int)idx < a->term) {
                a->term = (int)idx;
            }
        } else if (u == 0xFFFF) {
            /* padding past the terminator: nothing to store */
        } else if (idx >= FAT_LFN_MAX) {
            a->bad = 1;                         /* longer than we handle */
        } else {
            a->lfn[idx] = u;                    /* keep the full code unit; the
                                                * dirent build UTF-8-encodes it */
            if ((int)idx > a->max_idx) {
                a->max_idx = (int)idx;
            }
        }
    }
}

/* Length of the assembled long name, or -1 if there isn't a usable one.
 * `sum` is the checksum of the 8.3 entry the run is claimed to belong to: a
 * run that doesn't match it is not this file's name and is rejected (the
 * caller then falls back to the 8.3 short name). */
static int lfn_length(const lfn_acc_t *a, uint8_t sum)
{
    if (a->bad || !a->have_sum || a->sum != sum) {
        return -1;
    }
    if (a->term >= 0) {
        return a->term;
    }
    if (a->max_idx >= 0) {
        return a->max_idx + 1;
    }
    return -1;
}

/* UTF-8-encode the assembled long name into `dst` (capacity `cap`, always
 * NUL-terminated). Truncates on a char boundary if it would overflow — real
 * names are far shorter than the buffer.
 *
 * Surrogate pairs are COMBINED into the 4-byte form. Encoding each UTF-16
 * code unit independently (what this did before) emits two 3-byte sequences
 * for D800-DFFF, i.e. CESU-8, which is not valid UTF-8 — and the consequence
 * was silent: those bytes can never match the host tool's FNV-1a hash over
 * real UTF-8, so any track with an emoji or other non-BMP character simply
 * dropped out of the library. An unpaired surrogate (either half) becomes
 * U+FFFD rather than propagating garbage. The 4-byte form fits: the loop
 * already reserves 4 bytes of headroom before the NUL.
 */
static void lfn_to_utf8(const uint16_t *lfn, int len, char *dst, int cap)
{
    int bi = 0;
    for (int i = 0; i < len && bi + 4 < cap; i++) {
        uint32_t cp = lfn[i];

        if (cp >= 0xD800u && cp <= 0xDBFFu) {           /* high surrogate */
            uint32_t lo = (i + 1 < len) ? lfn[i + 1] : 0u;
            if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                i++;                                     /* consumed the pair */
            } else {
                cp = 0xFFFDu;                            /* unpaired high */
            }
        } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {    /* stray low surrogate */
            cp = 0xFFFDu;
        }

        if (cp < 0x80u) {
            dst[bi++] = (char)cp;
        } else if (cp < 0x800u) {
            dst[bi++] = (char)(0xC0u | (cp >> 6));
            dst[bi++] = (char)(0x80u | (cp & 0x3Fu));
        } else if (cp < 0x10000u) {
            dst[bi++] = (char)(0xE0u | (cp >> 12));
            dst[bi++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            dst[bi++] = (char)(0x80u | (cp & 0x3Fu));
        } else {
            dst[bi++] = (char)(0xF0u | (cp >> 18));
            dst[bi++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
            dst[bi++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            dst[bi++] = (char)(0x80u | (cp & 0x3Fu));
        }
    }
    dst[bi] = '\0';
}

/* ---- public API ------------------------------------------------------ */

int fat32_mount(fat32_t *fs, fat_read_fn read, void *ud, uint32_t part_lba)
{
    fs->read     = read;
    fs->ud       = ud;
    fs->part_lba = part_lba;

    /* A fresh mount may reuse this fat32_t's address for a different volume;
     * drop any FAT / data sector cached under the old geometry. */
    fat_cache_valid = 0;
    dat_cache_valid = 0;

    uint8_t bs[512];
    if (read(ud, part_lba, 1, bs) != 0) {
        return -1;
    }
    if (bs[510] != 0x55 || bs[511] != 0xAA) {
        return -2;   /* no boot signature */
    }

    uint32_t byts = rd16(&bs[11]);
    if (byts != 512 && byts != 1024 && byts != 2048 && byts != 4096) {
        return -3;
    }
    uint32_t rsvd     = rd16(&bs[14]);
    uint32_t num_fats = bs[16];
    uint32_t fatsz    = rd32(&bs[36]);   /* FATSz32 (FS sectors)         */
    uint32_t rootclus = rd32(&bs[44]);   /* BPB_RootClus                 */

    /*
     * Confirm this is FAT32 BEFORE trusting offsets 36 and 44 at all. On a
     * FAT16 BPB those bytes are BS_DrvNum / BS_BootSig / BS_VolID and part of
     * the volume label — typically nonzero, so every other check here passed
     * and the driver went off walking a "root cluster" that does not exist.
     * The two fields that actually discriminate are FATSz16 (offset 22) and
     * RootEntCnt (offset 17): both are zero on FAT32 and both are nonzero on
     * FAT12/16 by definition (a FAT16 volume must have a fixed-size root
     * directory and a 16-bit FAT size).
     */
    if (rd16(&bs[22]) != 0 || rd16(&bs[17]) != 0) {
        return -5;   /* FAT12/FAT16 (or not a FAT BPB at all) */
    }

    fs->bytes_per_sec = byts;
    fs->sec_ratio     = byts / 512u;
    fs->sec_per_clus  = bs[13];
    fs->root_clus     = rootclus;
    if (fs->sec_per_clus == 0 || fatsz == 0 || num_fats == 0 || rootclus < 2) {
        return -4;
    }
    fs->fat_start   = rsvd;
    fs->data_start  = rsvd + num_fats * fatsz;
    fs->clus_bytes  = fs->sec_per_clus * byts;

    /* Capacity: total FS sectors (TotSec32, or TotSec16) minus the reserved +
     * FAT region, in clusters. */
    uint32_t totsec = rd32(&bs[32]);
    if (totsec == 0) totsec = rd16(&bs[19]);
    fs->total_clus = (totsec > fs->data_start)
                   ? (totsec - fs->data_start) / fs->sec_per_clus : 0;

    /*
     * Cluster-number ceiling (EXCLUSIVE) — every chain step is checked
     * against this, and every chain walk is bounded by it. Two independent
     * bounds, whichever is tighter:
     *
     *  1. Capacity, total_clus + 2 (clusters are numbered from 2).
     *     CAVEAT: total_clus is derived from TotSec32/TotSec16, and the host
     *     test-image generator (tests/scripts/make_fat32_image.py) never
     *     writes either field — nor does the in-RAM image inside fat32_test —
     *     so total_clus is 0 across the ENTIRE existing test suite. Treating
     *     0 as "no clusters" would reject every cluster on those volumes and
     *     break the tests, so 0 means "unknown" here and we fall through to
     *     bound 2 alone. A real volume formatted by any OS carries TotSec32.
     *
     *  2. What the FAT can even address: one FAT of `fatsz` FS-sectors holds
     *     fatsz * bytes_per_sec / 4 entries, and a cluster with no FAT entry
     *     is not a cluster. This is derived from BPB fields the driver
     *     already validates (fatsz != 0), so it is a real bound even when
     *     the size fields are missing — on the synthetic test images it comes
     *     out at 512 and 128 clusters respectively, which is exactly right.
     *
     * Both are clamped to FAT_MAX_CLUS_CAP; cluster_valid() adds the
     * no-uint32-wrap test on top.
     */
    uint32_t per_sec = byts / 4u;                 /* FAT entries per FS-sector */
    uint32_t cap = (fatsz > FAT_MAX_CLUS_CAP / per_sec)
                 ? FAT_MAX_CLUS_CAP               /* would overflow: clamp     */
                 : fatsz * per_sec;
    if (fs->total_clus != 0 && fs->total_clus < FAT_MAX_CLUS_CAP - 2u) {
        uint32_t by_size = fs->total_clus + 2u;
        if (by_size < cap) {
            cap = by_size;
        }
    }
    if (cap > FAT_MAX_CLUS_CAP) {
        cap = FAT_MAX_CLUS_CAP;
    }
    fs->max_clus = cap;

    /* The root cluster itself has to be addressable, or nothing else here
     * can be trusted either. */
    if (!cluster_valid(fs, fs->root_clus)) {
        return -4;
    }

    /* Free clusters: cheap read of the FSInfo sector's FSI_Free_Count (offset
     * 488), validated by its three signatures. 0xFFFFFFFF = "unknown" (we don't
     * scan the whole FAT — too slow on an 80 GB volume). */
    fs->free_clus = 0xFFFFFFFFu;
    uint32_t fsinfo = rd16(&bs[48]);
    if (fsinfo != 0 && fsinfo != 0xFFFFu) {
        uint8_t fi[512];
        if (read(ud, part_lba + fsinfo * fs->sec_ratio, 1, fi) == 0 &&
            rd32(&fi[0])   == 0x41615252u &&
            rd32(&fi[484]) == 0x61417272u &&
            rd32(&fi[508]) == 0xAA550000u) {
            fs->free_clus = rd32(&fi[488]);
        }
    }
    return 0;
}

int fat32_readdir(fat32_t *fs, uint32_t dir_clus, fat32_dir_cb cb, void *ud)
{
    /*
     * THE directory walk. It used to be one of two: fat32_open carried a
     * near-identical 65-line copy of this cluster-chain follow, FS-sector
     * read and LFN reassembly, which only the tests ever called and which was
     * free to drift from the copy that actually ships. The lookup path is now
     * a callback over this function (fat32_open_in, below), so there is
     * exactly one traversal to keep correct.
     *
     * Every real entry (files AND subdirectories, is_dir set accordingly) is
     * surfaced through the callback. The LFN run accumulates across the 0x0F
     * fragments that precede each 8.3 entry; it is reset on anything that
     * breaks a run (deleted slot, volume label, a "."/".." link) and is
     * accepted for an entry only when its checksum binds it to that entry.
     */
    lfn_acc_t acc;
    lfn_reset(&acc);

    /*
     * The directory sector is read into this LOCAL, not into a shared static
     * scratch buffer. Reentrancy: the entry pointer below indexes into this
     * buffer across the cb() call, and cb() is arbitrary caller code that
     * routinely does disk I/O through this same API (resolving album art,
     * opening the index, descending a level). Parsing out of the shared
     * buffer — which is what this did — meant any such callback clobbered the
     * sector mid-loop and the rest of that directory parsed as garbage; it
     * was safe only by accident of who happened to call what. The cost of
     * making it structurally safe is one FS-sector of stack (<= 4 KB) per
     * readdir frame out of ~89 KB, and the shared scratch buffer goes away.
     */
    uint8_t sec[4096];

    uint32_t clus  = dir_clus;
    uint32_t guard = dir_walk_limit(fs);   /* bounded walk — see the #define */

    while (cluster_valid(fs, clus)) {
        if (guard-- == 0) {
            return FAT32_ECORRUPT;   /* cyclic or absurdly long chain */
        }
        uint32_t csec = cluster_fs_sector(fs, clus);
        for (uint32_t s = 0; s < fs->sec_per_clus; s++) {
            if (read_fs_sector(fs, csec + s, sec) != 0) {
                return FAT32_EIO;
            }
            for (uint32_t o = 0; o + 32u <= fs->bytes_per_sec; o += 32u) {
                const uint8_t *e = &sec[o];
                if (e[0] == 0x00) {
                    return 0;               /* end of directory: done */
                }
                if (e[0] == 0xE5) {
                    lfn_reset(&acc);        /* deleted (drop any LFN run) */
                    continue;
                }
                if ((e[11] & 0x0F) == 0x0F) {
                    lfn_add(&acc, e);       /* LFN fragment for the next 8.3 */
                    continue;
                }
                if ((e[11] & 0x08) != 0) {
                    lfn_reset(&acc);        /* volume label: not a real entry */
                    continue;
                }
                if (e[0] == '.') {
                    /* "." (self) and ".." (parent) links inside a subdirectory:
                     * any 8.3 entry whose raw name starts with '.'. A browser
                     * wants only real children, so drop these (and any LFN run,
                     * though these never carry one). */
                    lfn_reset(&acc);
                    continue;
                }

                /* Real 8.3 entry: build the dirent. Prefer the reassembled
                 * long name, but only when its checksum binds it to THIS
                 * entry; otherwise fall back to the formatted 8.3 short name.
                 * The name buffer lives in the caller's fat32_dirent_t (their
                 * stack), and the long name is capped at FAT_LFN_MAX (< 256),
                 * so it always fits with room for the terminator. */
                fat32_dirent_t ent;
                int llen = lfn_length(&acc, lfn_checksum(e));
                if (llen >= 0) {
                    lfn_to_utf8(acc.lfn, llen, ent.name, (int)sizeof ent.name);
                } else {
                    fmt_83(e, ent.name);
                }
                fmt_83(e, ent.short_name);  /* always the raw 8.3, for lookup */
                ent.is_dir     = (e[11] & 0x10) ? 1 : 0;
                ent.first_clus = ((uint32_t)rd16(&e[20]) << 16) | rd16(&e[26]);
                ent.size       = ent.is_dir ? 0u : rd32(&e[28]);

                lfn_reset(&acc);            /* LFN run belonged to this entry */

                if (cb(ud, &ent) != 0) {
                    return 0;               /* caller asked to stop early */
                }
            }
        }
        clus = next_cluster(fs, clus);
        if (clus == 0) {
            return FAT32_EIO;
        }
    }
    return 0;
}

int fat32_readdir_root(fat32_t *fs, fat32_dir_cb cb, void *ud)
{
    /* Thin wrapper: the root is just the directory at fs->root_clus. */
    return fat32_readdir(fs, fs->root_clus, cb, ud);
}

/* ---- name lookup (a callback over the one directory walk) ------------- */

typedef struct {
    const char *want;      /* requested name, ASCII, case-insensitive */
    uint32_t    clus;
    uint32_t    size;
    int         found;
} open_ctx_t;

static int open_match_cb(void *ud, const fat32_dirent_t *ent)
{
    open_ctx_t *c = (open_ctx_t *)ud;

    /* Match the display name (the VFAT long name when there is one) OR the
     * raw 8.3 short name, so a mangled short name like INTENT~1.FLA still
     * resolves for a file whose long name is "Intentions.flac". */
    if (name_eq_ci(c->want, ent->name) ||
        name_eq_ci(c->want, ent->short_name)) {
        c->clus  = ent->first_clus;
        c->size  = ent->size;
        c->found = 1;
        return 1;          /* stop the walk */
    }
    return 0;
}

int fat32_open_in(fat32_t *fs, uint32_t dir_clus, const char *name,
                  uint32_t *first_clus, uint32_t *size)
{
    open_ctx_t c;
    c.want  = name;
    c.clus  = 0;
    c.size  = 0;
    c.found = 0;

    int rc = fat32_readdir(fs, dir_clus, open_match_cb, &c);
    if (rc != 0) {
        return rc;                 /* FAT32_EIO / FAT32_ECORRUPT */
    }
    if (!c.found) {
        return FAT32_ENOENT;
    }
    *first_clus = c.clus;
    *size       = c.size;
    return 0;
}

int fat32_open(fat32_t *fs, const char *name,
               uint32_t *first_clus, uint32_t *size)
{
    /* Root-directory lookup: the documented default. */
    return fat32_open_in(fs, fs->root_clus, name, first_clus, size);
}

int32_t fat32_read_file(fat32_t *fs, uint32_t clus, void *buf, uint32_t maxlen)
{
    uint8_t *out   = (uint8_t *)buf;
    uint32_t total = 0;
    uint32_t guard = fs->max_clus;   /* a chain can't outlast the volume */

    while (cluster_valid(fs, clus) && total < maxlen) {
        if (guard-- == 0) {
            return FAT32_ECORRUPT;   /* cyclic chain */
        }
        uint32_t csec      = cluster_fs_sector(fs, clus);
        uint32_t remaining = maxlen - total;

        if (remaining >= fs->clus_bytes) {
            /* Whole cluster straight into the caller's buffer, one call. */
            if (fs->read(fs->ud, fs->part_lba + csec * fs->sec_ratio,
                         fs->sec_per_clus * fs->sec_ratio, out) != 0) {
                return -1;
            }
            out   += fs->clus_bytes;
            total += fs->clus_bytes;
        } else {
            /* Partial final cluster: copy FS-sector by FS-sector, stopping at
             * maxlen (through the data-sector cache so we never overrun the
             * caller's buffer — and so a re-read of the same sector is free). */
            for (uint32_t s = 0; s < fs->sec_per_clus && total < maxlen; s++) {
                const uint8_t *src;
                if (read_data_sector(fs, csec + s, &src) != 0) {
                    return -1;
                }
                uint32_t take = maxlen - total;
                if (take > fs->bytes_per_sec) {
                    take = fs->bytes_per_sec;
                }
                memcpy(out, src, take);
                out   += take;
                total += take;
            }
        }

        /* Advance only when there is more to deliver. The advance used to be
         * unconditional at the bottom of the loop, before the `total < maxlen`
         * re-check at the top — so a file whose size is an exact multiple of
         * the cluster size did one extra FAT read AFTER its last byte, and if
         * that read failed the call returned -1 despite having delivered every
         * requested byte. fat32_stream_read has always gotten this right; this
         * now matches it. */
        if (total >= maxlen) {
            break;
        }
        clus = next_cluster(fs, clus);
        if (clus == 0) {
            return -1;
        }
    }
    return (int32_t)total;
}

void fat32_stream_open(fat32_stream_t *st, fat32_t *fs,
                       uint32_t first_clus, uint32_t size)
{
    st->fs        = fs;
    st->clus      = (size == 0) ? 0 : first_clus;
    st->clus_off  = 0;
    st->remaining = size;
}

int32_t fat32_stream_read(fat32_stream_t *st, void *buf, uint32_t len)
{
    fat32_t *fs    = st->fs;
    uint8_t *out   = (uint8_t *)buf;
    uint32_t total = 0;

    uint32_t guard = fs->max_clus;   /* a chain can't outlast the volume */

    while (total < len && st->remaining > 0 && cluster_valid(fs, st->clus)) {
        if (guard-- == 0) {
            return FAT32_ECORRUPT;   /* cyclic chain */
        }
        uint32_t sec_in_clus = st->clus_off / fs->bytes_per_sec;
        uint32_t off_in_sec  = st->clus_off % fs->bytes_per_sec;
        uint32_t base_sec    = cluster_fs_sector(fs, st->clus) + sec_in_clus;

        /* How much this iteration can copy: bounded by the request and by
         * what's left of the file. */
        uint32_t want = len - total;
        if (want > st->remaining) {
            want = st->remaining;
        }

        uint32_t take;
        if (off_in_sec != 0) {
            /* Unaligned head: copy the tail of one FS-sector out of the data
             * cache. Successive small reads inside the same sector — the
             * decoder's normal access pattern — now hit the cache instead of
             * re-reading the same 2048 bytes off the platter every call. */
            uint32_t sec_avail = fs->bytes_per_sec - off_in_sec;
            take = want < sec_avail ? want : sec_avail;
            const uint8_t *src;
            if (read_data_sector(fs, base_sec, &src) != 0) {
                return -1;
            }
            memcpy(out, src + off_in_sec, take);
        } else {
            /* Sector-aligned: read as many WHOLE contiguous FS-sectors as
             * fit — bounded by the request and the cluster boundary — in ONE
             * bulk fs->read straight into the caller buffer. This is what
             * keeps throughput up: one large aligned block read instead of a
             * per-sector read that ata_read_sectors would inflate to a full
             * physical sector each (4x amplification when the FS sector is
             * smaller than the drive's physical sector). Matches the
             * whole-cluster path in fat32_read_file. */
            uint32_t secs_left = fs->sec_per_clus - sec_in_clus;
            uint32_t whole     = want / fs->bytes_per_sec;
            if (whole > secs_left) {
                whole = secs_left;
            }
            if (whole > 0) {
                take = whole * fs->bytes_per_sec;
                if (fs->read(fs->ud, fs->part_lba + base_sec * fs->sec_ratio,
                             whole * fs->sec_ratio, out) != 0) {
                    return -1;
                }
            } else {
                /* Less than one FS-sector left to satisfy (a partial tail, or
                 * a sub-sector request): copy out of the data cache. */
                take = want;
                const uint8_t *src;
                if (read_data_sector(fs, base_sec, &src) != 0) {
                    return -1;
                }
                memcpy(out, src, take);
            }
        }

        out           += take;
        total         += take;
        st->clus_off  += take;
        st->remaining -= take;

        /* Advance to the next cluster only when the current one is fully
         * consumed AND more file remains — so we never walk the FAT (and
         * risk a spurious read error) once we've returned the last byte. */
        if (st->clus_off == fs->clus_bytes && st->remaining > 0) {
            st->clus     = next_cluster(fs, st->clus);
            st->clus_off = 0;
            if (st->clus == 0) {
                return -1;   /* read error walking the FAT */
            }
        }
    }
    return (int32_t)total;
}

uint32_t fat32_stream_skip(fat32_stream_t *st, uint32_t n)
{
    fat32_t *fs    = st->fs;
    uint32_t done  = 0;
    uint32_t guard = fs->max_clus;   /* a chain can't outlast the volume */

    while (n > 0 && st->remaining > 0 && cluster_valid(fs, st->clus)) {
        if (guard-- == 0) {
            break;      /* cyclic chain — stop, report what we skipped */
        }
        /* Skip within the current cluster by just moving the cursor — no data
         * read. Only the FAT is touched, when we step to the next cluster. */
        uint32_t clus_left = fs->clus_bytes - st->clus_off;
        uint32_t take      = n < clus_left ? n : clus_left;
        if (take > st->remaining) {
            take = st->remaining;
        }

        st->clus_off  += take;
        st->remaining -= take;
        done          += take;
        n             -= take;

        if (st->clus_off == fs->clus_bytes && st->remaining > 0) {
            st->clus     = next_cluster(fs, st->clus);
            st->clus_off = 0;
            if (st->clus == 0) {
                break;      /* FAT read error — stop, report what we skipped */
            }
        }
    }
    return done;
}

/* ---------------------------------------------------------------------------
 * ABSOLUTE LBA RESOLUTION — THE ONE PLACE A MISTAKE BECOMES DATA LOSS.
 *
 * Every other function in this file only ever READS. This one exists so a
 * caller can WRITE back into a pre-allocated file's own data area
 * (kernel/config.c), and it is the single point where a wrong number stops
 * being a failed read and starts being somebody's music library overwritten.
 * Read the formula note below before changing a character of it.
 *
 * THE FORMULA. cluster_fs_sector() returns FS-SECTORS — units of
 * fs->bytes_per_sec (2048 on the stock 80 GB volume), NOT 512-byte LBAs. Every
 * read path in this file multiplies by fs->sec_ratio before it reaches the
 * block callback; see read_fs_sector() above:
 *
 *     fs->read(fs->ud, fs->part_lba + fs_sec * fs->sec_ratio, fs->sec_ratio, …)
 *
 * and the bulk paths in fat32_read_file() / fat32_stream_read() compose it
 * identically. So the absolute 512-byte LBA of a cluster's first sector is
 *
 *     fs->part_lba + cluster_fs_sector(fs, clus) * fs->sec_ratio
 *
 * WITHOUT the "* sec_ratio" the result is 4x too small on a 2048-byte volume,
 * and that does not land somewhere harmless — it lands a few hundred sectors
 * past the partition start, i.e. INSIDE THE FAT. Writing there destroys the
 * allocation tables for the whole volume.
 *
 * Returns 0 and fills *lba / *max_sectors only when every check below passes;
 * on ANY doubt it returns an error and writes 0 to both out-params, so a
 * caller that ignores the return code still cannot address a sector.
 * ------------------------------------------------------------------------- */
int fat32_file_lba(const fat32_t *fs, uint32_t first_clus,
                   uint32_t *lba, uint32_t *max_sectors)
{
    if (fs == 0 || lba == 0 || max_sectors == 0) {
        return FAT32_EINVAL;
    }
    /* Fail closed: never leave a plausible-looking address behind. */
    *lba         = 0;
    *max_sectors = 0;

    if (fs->sec_ratio == 0 || fs->sec_per_clus == 0) {
        return FAT32_ECORRUPT;      /* unmounted / impossible geometry */
    }

    /* 1. The cluster must be one this volume can actually address. Same gate
     *    every read takes, and it is what makes the multiply below provably
     *    non-wrapping in FS-sector units. */
    if (!cluster_valid(fs, first_clus)) {
        return FAT32_ECORRUPT;
    }

    uint32_t fs_sec = cluster_fs_sector(fs, first_clus);

    /* 2. FS-sectors -> 512-byte sectors, overflow-checked BEFORE the multiply
     *    rather than after (an overflowed product is already a wild address;
     *    there is nothing left to detect once it has wrapped). */
    if (fs_sec > 0xFFFFFFFFu / fs->sec_ratio) {
        return FAT32_ECORRUPT;
    }
    uint32_t rel = fs_sec * fs->sec_ratio;

    /* 3. …then add the partition base, same check again. */
    if (rel > 0xFFFFFFFFu - fs->part_lba) {
        return FAT32_ECORRUPT;
    }
    uint32_t abs_lba = fs->part_lba + rel;

    /* 4. A data cluster is never sector 0 of the disk (the MBR) nor the
     *    partition's own boot sector: data_start is at least the reserved
     *    sectors plus the FATs past the partition start, so abs > part_lba
     *    always holds for a sane BPB. Assert it anyway — this is the check
     *    that catches "the caller handed us a struct that was never mounted",
     *    where every field is zero and the arithmetic above is perfectly
     *    happy to hand back LBA 0. */
    if (abs_lba == 0 || abs_lba <= fs->part_lba) {
        return FAT32_ECORRUPT;
    }

    /* 5. Contiguously addressable run: exactly ONE cluster. We deliberately do
     *    NOT follow the chain — the next cluster of a fragmented file is
     *    somewhere else entirely, and a caller that assumed contiguity would
     *    run off the end of this cluster straight into a neighbouring file.
     *    One cluster is 32 KB on the stock volume, far more than the config
     *    record needs. */
    uint32_t run = fs->sec_per_clus * fs->sec_ratio;    /* <= 255 * 8 */
    if (run == 0 || run > 0xFFFFFFFFu - abs_lba) {
        return FAT32_ECORRUPT;
    }

    /* 6. The whole run must sit inside the partition. total_clus comes from
     *    TotSec32/TotSec16 and is 0 ("unknown") on the synthetic test images
     *    and on a BPB missing both size fields; there cluster_valid()'s
     *    FAT-addressability ceiling (step 1) is the only bound available and
     *    we have nothing tighter to add. When it IS known, use it — it is the
     *    only check here that knows where the volume ENDS. */
    if (fs->total_clus != 0) {
        uint32_t end_fs_sec = fs->data_start;
        if (fs->total_clus <= (0xFFFFFFFFu - end_fs_sec) / fs->sec_per_clus) {
            end_fs_sec += fs->total_clus * fs->sec_per_clus;
            if (end_fs_sec <= 0xFFFFFFFFu / fs->sec_ratio) {
                uint32_t end_rel = end_fs_sec * fs->sec_ratio;
                if (end_rel <= 0xFFFFFFFFu - fs->part_lba) {
                    uint32_t end_lba = fs->part_lba + end_rel;
                    if (abs_lba + run > end_lba) {
                        return FAT32_ECORRUPT;
                    }
                }
            }
        }
    }

    *lba         = abs_lba;
    *max_sectors = run;
    return 0;
}
