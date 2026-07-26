/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/kernel/config.h — persistent settings, stored in a PRE-ALLOCATED file.
 *
 * *** THIS IS THE ONLY CODE IN THE FIRMWARE THAT WRITES TO THE USER'S DISK. ***
 *
 * The whole design exists to make that sentence survivable. See config.c for
 * the full rationale; the short version:
 *
 *   - The host creates CORECFG.DAT in the volume root once, at import time,
 *     with a fixed size. The device NEVER creates, grows, shrinks, moves or
 *     deletes it — it only overwrites the bytes of the file's own first
 *     cluster. That mutates ZERO filesystem metadata: no FAT entries, no
 *     directory entries, no free-cluster search, no FSInfo. Everything the
 *     read-only driver depends on stays untouched, so we get persistence
 *     without a general FAT writer (which is where corruption risk lives).
 *
 *   - The target LBA is resolved from the file's own directory entry through
 *     fat32_file_lba(), which validates the cluster, the arithmetic and the
 *     partition bounds, and is RE-RESOLVED AND RE-VALIDATED before every
 *     single write. No LBA is ever cached across calls and no literal LBA
 *     appears anywhere.
 *
 *   - Two slots, alternated by an increasing sequence number, each a whole
 *     PHYSICAL sector (ATA_PHYS_LOG * 512 = 1024 B — this drive rejects
 *     sub-physical-sector access with IDNF, so a "512-byte atomic write" does
 *     not exist here). A failed or torn write can only damage the slot we are
 *     NOT currently reading from, so the previous good record always survives.
 *
 *   - Every record is CRC-32 protected over its entire body, so a half-written
 *     or garbage slot is rejected rather than believed.
 *
 * If anything at all is wrong — file missing, size too small, cluster
 * unresolvable, LBA misaligned — the module disables itself and the caller
 * keeps settings_defaults(). REFUSING TO SAVE IS ALWAYS THE CORRECT ANSWER
 * WHEN IN DOUBT.
 */
#ifndef CORE_KERNEL_CONFIG_H
#define CORE_KERNEL_CONFIG_H

#include <stdint.h>

#include "hw/ata.h"          /* ATA_PHYS_LOG / ATA_SECTOR_SZ: the write quantum */
#include "../fs/fat32.h"
#include "../ui/settings.h"

/* The file the host tool pre-creates in the volume ROOT (8.3 name, so it
 * matches with or without an LFN). tools/make_config.py writes it. */
#define CONFIG_FILE_NAME "CORECFG.DAT"

/*
 * On-disk geometry. One SLOT is one whole physical sector; there are two.
 * CONFIG_MIN_BYTES is the smallest CORECFG.DAT this module will accept — the
 * host tool creates it at least this large (in practice a whole cluster).
 */
#define CONFIG_SLOT_SECTORS (ATA_PHYS_LOG)                    /* 2 LBAs      */
#define CONFIG_SLOT_BYTES   (CONFIG_SLOT_SECTORS * ATA_SECTOR_SZ) /* 1024 B  */
#define CONFIG_SLOTS        2u
#define CONFIG_SECTORS      (CONFIG_SLOT_SECTORS * CONFIG_SLOTS)  /* 4 LBAs  */
#define CONFIG_MIN_BYTES    (CONFIG_SLOT_BYTES * CONFIG_SLOTS)    /* 2048 B  */

/* Record layout version this build writes. Readers accept <= this. */
#define CONFIG_VERSION      1u

/*
 * Bind the module to a mounted volume and load the newest valid record.
 *
 * Enumerates the volume root for CONFIG_FILE_NAME, resolves + validates its
 * first cluster to an absolute LBA, reads both slots through `fs`'s own block
 * callback (the same one every read takes), validates magic/version/CRC on
 * each, and picks the valid slot with the newer sequence number.
 *
 * Returns 1 if a valid record was found and *s was populated from it; 0
 * otherwise — in which case *s is UNTOUCHED and the caller must keep
 * settings_defaults(). A 0 return does not necessarily disable saving: if the
 * file exists and resolves but holds no valid record (a fresh file, or both
 * slots corrupt) we can still write it. config_writable() says which.
 *
 * Safe to call with a null `fs` or `s` (returns 0, module disabled).
 */
int config_load(fat32_t *fs, settings_t *s);

/*
 * 1 if config_load() found and validated a writable CORECFG.DAT, so
 * config_save() has somewhere to go. 0 before config_load(), or whenever the
 * file was absent / too small / unresolvable.
 */
int config_writable(void);

/*
 * Persist `s` to the slot that was NOT most recently read, with a bumped
 * sequence number, then FLUSH CACHE.
 *
 * THE ONLY CALLER OF ata_write_sectors() IN THE FIRMWARE. Re-resolves and
 * re-validates the target LBA from the file's cluster on every call — nothing
 * is cached. Refuses (returns negative, writes nothing) if config_load() never
 * succeeded in finding the file.
 *
 * Call this DEBOUNCED, not on every wheel tick: one write per settling.
 *
 * Returns 0 on success, negative on refusal or on an ATA error. Failure is
 * non-fatal by construction — the previous slot is still intact, so the
 * device simply keeps the older saved settings.
 */
int config_save(const settings_t *s);

/*
 * The sequence number of the record currently believed newest (0 if none).
 * Exposed for diagnostics/UART logging and for the host tests.
 */
uint32_t config_seq(void);

/*
 * DIAGNOSTIC: resolve slot `slot` (0..CONFIG_SLOTS-1) to its absolute
 * 512-byte LBA without writing anything. Returns 0 and sets *lba on success,
 * negative otherwise (with *lba = 0).
 *
 * This exists for ONE reason: the first on-device bring-up must be able to
 * compare the address the firmware would write against the address the host
 * tool computed, BEFORE any write happens. See the procedure at the top of
 * config.c. It resolves fresh, exactly like config_save() does — it does not
 * expose a cached value, because there isn't one.
 */
int config_probe_lba(uint32_t slot, uint32_t *lba);

/* ---- record codec, exposed for host unit tests ------------------------- */

/*
 * Encode `s` into a CONFIG_SLOT_BYTES record with sequence `seq`.
 * `rec` must be CONFIG_SLOT_BYTES and 16-bit aligned. Always succeeds.
 */
void config_encode(uint8_t *rec, const settings_t *s, uint32_t seq);

/*
 * Validate + decode a CONFIG_SLOT_BYTES record. Returns 1 and fills *s and
 * *seq if magic, version, declared length and CRC-32 all check out; 0
 * otherwise (leaving *s untouched). Every decoded field is clamped to its
 * legal range, so even a CRC-valid but hostile record cannot install an
 * out-of-range brightness or volume.
 */
int config_decode(const uint8_t *rec, settings_t *s, uint32_t *seq);

/*
 * Is `a` strictly newer than `b` in the wrapping sequence space? Signed
 * difference, so 0xFFFFFFFF -> 0 reads as "newer" rather than as a 4-billion
 * step backwards. Exposed for the tests.
 */
int config_seq_newer(uint32_t a, uint32_t b);

#endif /* CORE_KERNEL_CONFIG_H */
