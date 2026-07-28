/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/kernel/config.c — persistent settings in a pre-allocated file.
 *
 * =========================================================================
 * *** THE WRITE PATH BELOW IS PROVEN ON HARDWARE (2026-07-27). ***
 *
 * The bring-up procedure below was run end to end on the 5.5G: the resolved
 * LBA was confirmed three ways before any write, slot alternation was read
 * back off the raw disk, and `fsck.vfat -n` reported the volume clean
 * afterwards. The procedure is KEPT — not as a warning, but because it is the
 * only way to re-qualify this path after a change to fat32_file_lba(),
 * ata_write_sectors(), the record layout or the host tool. Run it again if you
 * touch any of those.
 *
 * ON-DEVICE TEST — do it in this order, on a disk you can afford to lose:
 *
 *   1. Use a SCRATCH iPod disk (or a disk image restored from a backup you
 *      have actually verified restores). Do NOT do the first write on a disk
 *      holding the only copy of anything.
 *   2. Before flashing, confirm the resolved LBA on the HOST: run
 *      `tools/make_config.py --verify /dev/sdX` (or against an image), which
 *      prints the absolute 512-byte LBA of CORECFG.DAT's first cluster
 *      computed the same way fat32_file_lba() does, plus a hexdump of what is
 *      there now. Write that number down.
 *   3. Boot the firmware and read the UART line "core: cfg lba XXXXXXXX".
 *      It MUST equal the number from step 2, exactly. If it does not, stop —
 *      do not change a setting, power the device off. A mismatch here is the
 *      bug that destroys a library.
 *   4. Only then change one setting, leave the Settings screen, and wait for
 *      "core: cfg save rc 00000000".
 *   5. Power-cycle. Confirm the setting survived AND — this is the part
 *      people skip — remount the disk on the host and confirm the MUSIC IS
 *      STILL THERE and `fsck.vfat -n` reports the volume clean. A write to
 *      the wrong place will often not show up as a lost setting; it shows up
 *      as a corrupt volume.
 *   6. Repeat step 4/5 a few times and confirm the two slots alternate (the
 *      host verify dump shows both slots' seq).
 *
 * If any step fails, the correct response is to make config_writable() return
 * 0 unconditionally, not to "fix it quickly".
 * =========================================================================
 *
 * WHY THIS IS SAFE (when the LBA is right)
 *
 * The host pre-creates CORECFG.DAT with a fixed size. The device only ever
 * overwrites bytes inside that file's FIRST CLUSTER. That means:
 *
 *   - no FAT entry is written (the chain already exists),
 *   - no directory entry is written (the file already exists, at a length
 *     that never changes, so not even the size field moves),
 *   - no free-cluster search and no FSInfo update happen,
 *   - the file's own data is the only thing on the disk that changes.
 *
 * So the entire class of "the allocator got it wrong" bugs is absent by
 * construction, and the residual risk collapses to one question: is the LBA
 * right? Everything else in this file is in service of answering that
 * conservatively, and of refusing when it cannot.
 *
 * WHY TWO SLOTS, AND WHY 1024 BYTES
 *
 * The obvious design is one 512-byte record and an appeal to sector-level
 * atomicity. That is wrong twice over on this hardware. First, the stock
 * 80 GB drive (MK8010GAH) reports 2 logical sectors per physical sector and
 * REJECTS sub-physical-sector access with IDNF — a 512-byte write is not
 * merely non-atomic, it is refused outright, so the record has to be a whole
 * physical sector (ATA_PHYS_LOG * 512 = 1024 B) at an aligned LBA. Second,
 * atomicity of the medium is not the only failure: the drive can fail the
 * command outright, or lose power mid-command, and "the sector is now
 * neither the old nor the new record" has to be survivable.
 *
 * So: two slots, written alternately, each CRC-32 protected over its whole
 * body. A write can only ever damage the slot that is NOT the current
 * newest-valid one, so there is always a good record to fall back to. Load
 * reads both and takes the valid one with the newer sequence number.
 *
 * WHAT THIS MODULE REFUSES TO DO
 *
 *   - Create, extend, truncate, move or delete the file.  (Host tool's job.)
 *   - Follow the file's cluster chain. Only the first cluster is ever
 *     addressed, so fragmentation cannot walk us into a neighbouring file.
 *   - Cache an LBA. Every save re-resolves and re-validates from scratch.
 *   - Write to a misaligned LBA, LBA 0, or anything outside the partition.
 *   - Write anything at all if the file was not found at mount time.
 */

#include "config.h"

#include "hw/ata.h"
#include "../fs/fat32.h"
#include "../ui/settings.h"

/* ---- module state ------------------------------------------------------ */

/*
 * Everything learned at config_load() time. Note what is NOT here: the target
 * LBA. It is recomputed inside config_save() on every call, deliberately —
 * a cached address is an address nobody re-checks, and the whole safety
 * argument of this file rests on re-checking.
 */
static struct {
    fat32_t *fs;          /* the mounted volume, or 0 = module disabled     */
    uint32_t first_clus;  /* CORECFG.DAT's first cluster (validated)        */
    uint32_t size;        /* its directory-entry size, >= CONFIG_MIN_BYTES  */
    uint32_t seq;         /* sequence of the newest valid record seen       */
    uint8_t  slot;        /* slot that record came from (0/1); next write
                           * goes to the OTHER one                          */
    uint8_t  found;       /* 1 once the file resolved to a usable LBA       */
} g_cfg;

/* Scratch for one slot. uint32_t-typed so it is aligned for the halfword
 * data port ata_write_sectors requires (and for the u32 loads below). */
static uint32_t g_slot_buf[CONFIG_SLOT_BYTES / 4u];

/* ---- record format ----------------------------------------------------- */

/*
 * On-disk record (CONFIG_SLOT_BYTES = 1024 bytes, little-endian):
 *
 *   off   size  field
 *   0     4     magic     'C''O''R''E'
 *   4     2     version   layout version (see CONFIG_VERSION)
 *   6     2     length    meaningful payload bytes that follow
 *   8     4     seq       monotonic write counter (wrapping)
 *   12    N     payload   packed settings fields, zero-padded
 *   1020  4     crc32     CRC-32 over bytes [0 .. 1020) — EVERYTHING above
 *
 * `length` — not `version` — is what says how much payload to read. Version
 * gates whether we understand the record at all; length gates which fields are
 * present inside one we do. That is what lets a v1 record (length 12) keep
 * working under a v2 decoder without a migration step.
 *
 * The CRC covering the HEADER (not just the payload) is deliberate: seq is
 * what decides which slot wins, so a corrupt seq with a valid payload CRC
 * would let a stale or garbage record beat a good one. Covering the zero
 * padding too costs nothing and removes a "which bytes are covered" question
 * from the format entirely.
 */
#define CFG_OFF_MAGIC    0u
#define CFG_OFF_VERSION  4u
#define CFG_OFF_LENGTH   6u
#define CFG_OFF_SEQ      8u
#define CFG_OFF_PAYLOAD  12u
#define CFG_OFF_CRC      (CONFIG_SLOT_BYTES - 4u)       /* 1020 */
#define CFG_PAYLOAD_MAX  (CFG_OFF_CRC - CFG_OFF_PAYLOAD)/* 1008 */

#define CFG_MAGIC        0x45524F43u                    /* 'C''O''R''E' LE */

/* Payload v1 field offsets (within the payload). One byte each, fixed width,
 * endian-free — NOT a struct dump, so compiler padding/ABI can never change
 * the on-disk format. */
enum {
    P_SHUFFLE = 0, P_REPEAT, P_RESUME, P_CROSSFADE, P_VOLUME,
    P_BASS, P_TREBLE, P_BALANCE, P_BL_SECS, P_BL_BRIGHT, P_THEME, P_CLICKER,
    CFG_PAYLOAD_V1        /* = 12 */
};

/*
 * Payload v2 APPENDS the resume locator. Written as explicit offsets rather
 * than more enum members because these are 32-bit fields, and because "one
 * enumerator, one byte" stops being true the moment a field is wider than a
 * byte — a silent way to shift every offset after it.
 *
 * Nothing before CFG_PAYLOAD_V1 moves, which is what makes a v1 record on a
 * user's disk still readable: `length` says 12, the three fields below are
 * simply not there, and decode reports "no resume".
 */
#define P_RES_HASH      (CFG_PAYLOAD_V1 + 0u)    /* 12: name_hash of the file */
#define P_RES_SECS      (CFG_PAYLOAD_V1 + 4u)    /* 16: elapsed seconds       */
#define P_RES_TOTAL     (CFG_PAYLOAD_V1 + 8u)    /* 20: track length          */
#define CFG_PAYLOAD_V2  (CFG_PAYLOAD_V1 + 12u)   /* = 24                      */

/* Ceiling for both stored second counts. A day is already absurd for one
 * track; the point is that a CRC-valid but insane record cannot hand the
 * player a seek target built from garbage. (player_seek_to clamps to the real
 * track length as well — this is the belt to that pair of braces.) */
#define CFG_RESUME_SECS_MAX  86400u

/* ---- little-endian helpers --------------------------------------------- */

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

/* ---- CRC-32 ------------------------------------------------------------ */

/*
 * Standard CRC-32 (reflected, polynomial 0xEDB88320, init/final 0xFFFFFFFF) —
 * the same one zlib and Python's binascii.crc32 compute, which is what lets
 * tools/make_config.py write a record this loader accepts.
 *
 * Bitwise, no 1 KB lookup table: this runs twice at boot and once per settings
 * save, over 1020 bytes. Even at 30 MHz that is well under a millisecond, and
 * the table would cost more RAM than the whole record.
 */
static uint32_t crc32_buf(const uint8_t *p, uint32_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = (uint32_t)0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

/* ---- clamps ------------------------------------------------------------ */

/*
 * Clamp on DECODE, not just on encode. A record whose CRC checks out is
 * proof the bytes are the bytes we wrote — it is NOT proof they are sane
 * (an older/newer build, a hand-edited file, a host tool bug). Nothing
 * downstream re-validates settings_t, so a brightness of 255 or a volume of
 * 200 would go straight to the hardware.
 */
static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Unsigned ceiling clamp (the resume second-counts, which have no lower
 * bound to enforce). */
static uint32_t clampu(uint32_t v, uint32_t hi)
{
    return v > hi ? hi : v;
}

/* backlight_secs is a discrete set, not a range: snap to the nearest legal
 * value rather than clamping, so a bogus 7 becomes 5 and not 7. */
static int clamp_bl_secs(int v)
{
    static const int ok[] = { 0, 5, 10, 15, 30, 60 };
    int best = 0, bestd = -1;
    for (unsigned i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        int d = v - ok[i];
        if (d < 0) d = -d;
        if (bestd < 0 || d < bestd) { bestd = d; best = ok[i]; }
    }
    return best;
}

/* ---- record codec (host-testable, no hardware) ------------------------- */

void config_encode(uint8_t *rec, const settings_t *s, uint32_t seq)
{
    for (uint32_t i = 0; i < CONFIG_SLOT_BYTES; i++) {
        rec[i] = 0;                 /* deterministic padding — the CRC covers it */
    }

    wr32(&rec[CFG_OFF_MAGIC],   CFG_MAGIC);
    wr16(&rec[CFG_OFF_VERSION], (uint16_t)CONFIG_VERSION);
    wr16(&rec[CFG_OFF_LENGTH],  (uint16_t)CFG_PAYLOAD_V2);
    wr32(&rec[CFG_OFF_SEQ],     seq);

    uint8_t *p = &rec[CFG_OFF_PAYLOAD];
    p[P_SHUFFLE]   = (uint8_t)(s->shuffle ? 1 : 0);
    p[P_REPEAT]    = (uint8_t)clampi((int)s->repeat, 0, 2);
    p[P_RESUME]    = (uint8_t)(s->resume_on_startup ? 1 : 0);
    p[P_CROSSFADE] = (uint8_t)(s->crossfade ? 1 : 0);
    p[P_VOLUME]    = (uint8_t)clampi(s->volume, 0, 100);
    p[P_BASS]      = (uint8_t)(int8_t)clampi(s->bass,    -12,  12);
    p[P_TREBLE]    = (uint8_t)(int8_t)clampi(s->treble,  -12,  12);
    p[P_BALANCE]   = (uint8_t)(int8_t)clampi(s->balance, -100, 100);
    p[P_BL_SECS]   = (uint8_t)clamp_bl_secs(s->backlight_secs);
    p[P_BL_BRIGHT] = (uint8_t)clampi(s->backlight_bright, 1, 32);
    p[P_THEME]     = (uint8_t)clampi(s->theme,   0, 3);
    p[P_CLICKER]   = (uint8_t)clampi(s->clicker, 0, 3);

    /* v2: the resume locator. Hash 0 means "nothing to resume", so the two
     * second-counts are forced to 0 with it — the record can never carry a
     * position that belongs to no track. */
    uint32_t rh = s->resume_hash;
    uint32_t rs = rh ? clampu(s->resume_secs,  CFG_RESUME_SECS_MAX) : 0u;
    uint32_t rt = rh ? clampu(s->resume_total, CFG_RESUME_SECS_MAX) : 0u;
    wr32(&p[P_RES_HASH],  rh);
    wr32(&p[P_RES_SECS],  rs);
    wr32(&p[P_RES_TOTAL], rt);

    wr32(&rec[CFG_OFF_CRC], crc32_buf(rec, CFG_OFF_CRC));
}

int config_decode(const uint8_t *rec, settings_t *s, uint32_t *seq)
{
    if (rec == 0 || s == 0 || seq == 0) {
        return 0;
    }
    if (rd32(&rec[CFG_OFF_MAGIC]) != CFG_MAGIC) {
        return 0;
    }

    uint32_t ver = rd16(&rec[CFG_OFF_VERSION]);
    /* Version 0 is not a version; anything newer than we understand is a
     * record written by a future build whose payload we cannot interpret
     * field-for-field, so we decline rather than guess. (Forward compat runs
     * the other way: a v1 record stays readable here because `length` says
     * how much of the payload v1 defined.) */
    if (ver == 0 || ver > CONFIG_VERSION) {
        return 0;
    }

    uint32_t len = rd16(&rec[CFG_OFF_LENGTH]);
    if (len < CFG_PAYLOAD_V1 || len > CFG_PAYLOAD_MAX) {
        return 0;
    }

    /* CRC last: it is the expensive check, and it is the one that decides
     * whether any of the above was real. Over bytes [0, CFG_OFF_CRC). */
    if (crc32_buf(rec, CFG_OFF_CRC) != rd32(&rec[CFG_OFF_CRC])) {
        return 0;
    }

    const uint8_t *p = &rec[CFG_OFF_PAYLOAD];
    s->shuffle           = p[P_SHUFFLE] ? 1 : 0;
    s->repeat            = (repeat_mode_t)clampi(p[P_REPEAT], 0, 2);
    s->resume_on_startup = p[P_RESUME] ? 1 : 0;
    s->crossfade         = p[P_CROSSFADE] ? 1 : 0;
    s->volume            = clampi(p[P_VOLUME], 0, 100);
    s->bass              = clampi((int8_t)p[P_BASS],    -12,  12);
    s->treble            = clampi((int8_t)p[P_TREBLE],  -12,  12);
    s->balance           = clampi((int8_t)p[P_BALANCE], -100, 100);
    s->backlight_secs    = clamp_bl_secs(p[P_BL_SECS]);
    s->backlight_bright  = clampi(p[P_BL_BRIGHT], 1, 32);
    s->theme             = clampi(p[P_THEME],   0, 3);
    s->clicker           = clampi(p[P_CLICKER], 0, 3);

    /*
     * The v2 tail, gated on the record's own `length` — so the v1 record
     * sitting on the device right now decodes as "no resume" rather than
     * reading three 32-bit fields out of the zero padding by accident. Always
     * WRITTEN, never left at whatever the caller's defaults held, because a
     * half-populated locator is exactly how you resume into the wrong track.
     */
    if (len >= CFG_PAYLOAD_V2) {
        s->resume_hash  = rd32(&p[P_RES_HASH]);
        s->resume_secs  = clampu(rd32(&p[P_RES_SECS]),  CFG_RESUME_SECS_MAX);
        s->resume_total = clampu(rd32(&p[P_RES_TOTAL]), CFG_RESUME_SECS_MAX);
    } else {
        s->resume_hash  = 0;
        s->resume_secs  = 0;
        s->resume_total = 0;
    }
    if (s->resume_hash == 0) {
        s->resume_secs  = 0;         /* no track => no position (encode's rule, */
        s->resume_total = 0;         /* re-asserted against a hand-edited file) */
    }

    *seq = rd32(&rec[CFG_OFF_SEQ]);
    return 1;
}

int config_seq_newer(uint32_t a, uint32_t b)
{
    /* Wrapping comparison: the distance from b to a, read as signed. A single
     * increment across 0xFFFFFFFF -> 0 is +1, not -4294967295. Only the sign
     * matters, and equal is not newer. */
    return (int32_t)(a - b) > 0;
}

/* ---- LBA resolution ---------------------------------------------------- */

/*
 * Resolve CORECFG.DAT's first cluster to the absolute LBA of SLOT `slot` and
 * fully validate it. Returns 0 and sets *out on success; negative otherwise,
 * with *out set to 0.
 *
 * This runs at load AND immediately before every single write — never once,
 * never cached. It is short on purpose: every line is a reason to refuse.
 */
static int config_slot_lba(uint32_t slot, uint32_t *out)
{
    *out = 0;

    if (!g_cfg.found || g_cfg.fs == 0 || slot >= CONFIG_SLOTS) {
        return -1;
    }
    /* The file must still be at least as big as the region we address. This
     * is the directory-entry size captured at load; we never change it. */
    if (g_cfg.size < CONFIG_MIN_BYTES) {
        return -1;
    }

    /* The one call that turns a cluster into a disk address. It validates the
     * cluster, the arithmetic and the partition bounds, and fails closed. */
    uint32_t base = 0, run = 0;
    if (fat32_file_lba(g_cfg.fs, g_cfg.first_clus, &base, &run) != 0) {
        return -1;
    }

    /* The whole two-slot region must fit inside the SINGLE cluster we
     * resolved — we never follow the chain, so anything past `run` belongs to
     * a cluster we have not proven anything about. */
    if (run < CONFIG_SECTORS) {
        return -1;
    }

    /* Physical-sector alignment. The drive IDNFs a misaligned or
     * sub-physical-sector access, so an unaligned base is not a slow path
     * here, it is a refusal: bouncing it would mean read-modify-writing
     * bytes the caller never asked to touch. In practice base is
     * part_lba + k*sec_ratio, so this only trips on an oddly-placed
     * partition — in which case not saving is exactly right. */
    if ((base % ATA_PHYS_LOG) != 0) {
        return -1;
    }

    uint32_t lba = base + slot * CONFIG_SLOT_SECTORS;

    /* Belt and braces on top of fat32_file_lba: never LBA 0, never past the
     * run we validated, and never at or below the partition's own first
     * sector. `run >= CONFIG_SECTORS` above already implies the bound; state
     * it directly anyway, because this is the last line before a write. */
    if (lba == 0 || lba <= g_cfg.fs->part_lba) {
        return -1;
    }
    if (lba < base || (lba - base) + CONFIG_SLOT_SECTORS > run) {
        return -1;
    }
    if ((lba % ATA_PHYS_LOG) != 0) {
        return -1;
    }

    *out = lba;
    return 0;
}

/* ---- load -------------------------------------------------------------- */

/* Root-directory scan: capture CORECFG.DAT's cluster + size. Mirrors how
 * CORELIB.IDX is located in main.c — the file is found by ENUMERATION, never
 * by a hardcoded address. */
static int cfg_root_cb(void *ud, const fat32_dirent_t *e)
{
    (void)ud;
    if (e->is_dir) {
        return 0;
    }
    /* Match the 8.3 short name: the host tool writes a plain 8.3 name, so
     * this matches whether or not the volume also carries an LFN, and it
     * avoids depending on long-name reassembly for the one file we write. */
    const char *want = CONFIG_FILE_NAME;
    const char *have = e->short_name;
    int i = 0;
    for (; want[i] != '\0' && have[i] != '\0'; i++) {
        char a = want[i], b = have[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) {
            return 0;
        }
    }
    if (want[i] != have[i]) {
        return 0;
    }

    g_cfg.first_clus = e->first_clus;
    g_cfg.size       = e->size;
    return 1;                       /* stop the enumeration — found it */
}

int config_load(fat32_t *fs, settings_t *s)
{
    /* Full reset: a second call must not inherit the first one's state. */
    g_cfg.fs         = 0;
    g_cfg.first_clus = 0;
    g_cfg.size       = 0;
    g_cfg.seq        = 0;
    g_cfg.slot       = 0;
    g_cfg.found      = 0;

    if (fs == 0 || s == 0) {
        return 0;
    }

    if (fat32_readdir(fs, fs->root_clus, cfg_root_cb, 0) != 0) {
        return 0;                   /* disk/geometry error — stay disabled */
    }
    if (g_cfg.first_clus == 0 || g_cfg.size < CONFIG_MIN_BYTES) {
        g_cfg.first_clus = 0;
        g_cfg.size       = 0;
        return 0;                   /* absent, or too small to hold 2 slots */
    }

    /* Provisionally enable so config_slot_lba() will run, then immediately
     * make that conditional on the address actually resolving. */
    g_cfg.fs    = fs;
    g_cfg.found = 1;

    uint32_t lba0 = 0;
    if (config_slot_lba(0, &lba0) != 0) {
        g_cfg.fs    = 0;
        g_cfg.found = 0;            /* unresolvable => writes stay disabled */
        return 0;
    }

    /*
     * Read both slots and pick the winner. Reads go through the volume's own
     * block callback at the SAME absolute LBA config_save() writes — one
     * address, one code path, so load and save cannot drift apart. (Read via
     * fs->read rather than fat32_read_file deliberately: the cached data path
     * would serve a stale copy of a sector we just wrote, and we want the
     * platter's truth here.)
     */
    int      have   = 0;
    uint32_t best   = 0;
    uint8_t  best_i = 0;
    settings_t cand = *s;   /* seeded from the caller's defaults: `have` gates
                             * every use, and this leaves nothing uninitialised
                             * for a compiler (or a future edit) to trip over  */

    for (uint32_t slot = 0; slot < CONFIG_SLOTS; slot++) {
        uint32_t lba = 0;
        if (config_slot_lba(slot, &lba) != 0) {
            continue;
        }
        if (fs->read(fs->ud, lba, CONFIG_SLOT_SECTORS, g_slot_buf) != 0) {
            continue;               /* unreadable slot — the other may be fine */
        }
        settings_t tmp;
        uint32_t   seq = 0;
        if (!config_decode((const uint8_t *)g_slot_buf, &tmp, &seq)) {
            continue;               /* bad magic/version/CRC — torn or garbage */
        }
        if (!have || config_seq_newer(seq, best)) {
            have   = 1;
            best   = seq;
            best_i = (uint8_t)slot;
            cand   = tmp;
        }
    }

    if (!have) {
        /* File is usable but holds no valid record (fresh, or both slots
         * damaged). Saving stays ENABLED — writing slot 0 is exactly how we
         * recover — but the caller keeps its defaults. seq restarts at 0 so
         * the first save writes 1. */
        return 0;
    }

    g_cfg.seq  = best;
    g_cfg.slot = best_i;
    *s         = cand;
    return 1;
}

int config_writable(void)
{
    return g_cfg.found ? 1 : 0;
}

uint32_t config_seq(void)
{
    return g_cfg.seq;
}

int config_probe_lba(uint32_t slot, uint32_t *lba)
{
    if (lba == 0) {
        return -1;
    }
    /* Same resolver the write takes, so what this prints IS what a save would
     * target. Read-only: it cannot itself put a byte on the disk. */
    return config_slot_lba(slot, lba);
}

/* ---- save -------------------------------------------------------------- */

/*
 * THE ONLY CALL TO ata_write_sectors() IN THE FIRMWARE.
 *
 * Order matters: resolve and validate the address FIRST, build the record
 * SECOND, write THIRD, and only update the in-RAM bookkeeping if the write
 * reported success. If anything is wrong we return before a single byte
 * leaves the CPU.
 */
int config_save(const settings_t *s)
{
    if (s == 0) {
        return -1;
    }
    /* Hard gate: no file found at mount => this module does nothing, ever. */
    if (!g_cfg.found || g_cfg.fs == 0) {
        return -1;
    }

    /* Alternate slots: never overwrite the one we last read a good record
     * from, so a failed/torn write can only damage the copy nobody needs. */
    uint32_t slot = (g_cfg.slot ^ 1u) & 1u;

    /* RE-RESOLVE AND RE-VALIDATE, every time. Nothing above this line is a
     * cached address. */
    uint32_t lba = 0;
    if (config_slot_lba(slot, &lba) != 0) {
        return -2;
    }

    uint32_t seq = g_cfg.seq + 1u;      /* wraps; config_seq_newer copes */
    config_encode((uint8_t *)g_slot_buf, s, seq);

    /*
     * ata_write_sectors independently re-checks alignment and issues FLUSH
     * CACHE (0xE7) after the data — load-bearing, because the drive's write
     * cache is on by default and a battery pull (or the idle STANDBY path)
     * would otherwise silently lose the record it just acknowledged.
     */
    int rc = ata_write_sectors(lba, CONFIG_SLOT_SECTORS, g_slot_buf);
    if (rc != 0) {
        /* Leave g_cfg.seq/slot alone: the previous record is still the newest
         * good one on disk, and the next attempt will target this same slot
         * again rather than eating the good one. */
        return rc;
    }

    g_cfg.seq  = seq;
    g_cfg.slot = (uint8_t)slot;
    return 0;
}
