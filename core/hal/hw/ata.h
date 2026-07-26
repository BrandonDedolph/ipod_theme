/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/ata.h — minimal PIO-polled ATA sector reader (PP5022).
 *
 * Read-only, 512-byte LBA28 sectors. Reuses the drive state the
 * chainloading bootloader left behind (powered, spun, PIO-timed), so init
 * is just "select master, wait ready" — see core/docs/hw/04-ata.md. The
 * 80 GB 5.5G is addressed in plain 512-byte sectors here; the "2048-byte
 * sector" is a FAT-layer virtual-sector detail handled above this driver.
 * Asm-free (host-trace-testable).
 */
#ifndef CORE_HAL_HW_ATA_H
#define CORE_HAL_HW_ATA_H

#include <stdint.h>

/* Minimal bring-up: mask the ATA IRQ, select the master device, wait for
 * the drive to report ready. Returns 0 on success, -1 on timeout. */
int ata_init(void);

/*
 * Error codes shared by every command in this driver.
 *
 *   0                  success
 *  -1                  bad argument, or the drive never came ready
 *  -2                  timeout waiting for the transfer (DRQ never arrived)
 *  -3                  drive error (ERR/DF) — RETRYABLE
 *  ATA_ERR_IDNF (-4)   the LBA does not exist / violates the drive's physical-
 *                      sector alignment rule. NOT retryable: an identical
 *                      re-issue can only fail identically, so a caller with a
 *                      retry loop must fail fast on this instead of burning
 *                      its whole budget (04-ata.md, "Per-sector error
 *                      handling": `if (error & ERROR_IDNF) break;`).
 *
 * On any of the failure codes the driver has already run the documented
 * recovery — latch ERROR, drain residual DRQ, soft-reset the channel — so the
 * drive is NOT left mid-command and a caller's retry starts clean. (Before
 * this, a retry over a half-finished multi-sector read could return data
 * shifted by the un-drained residue AND report success.)
 */
#define ATA_ERR_IDNF  (-4)

/*
 * Logical (512-byte) sectors per PHYSICAL sector — the drive's access
 * granularity, and therefore the alignment/size quantum ata_write_sectors()
 * enforces. Public because the WRITE path's callers have to size their records
 * in whole physical sectors: there is no such thing as a 512-byte write on
 * this drive (the stock 80 GB MK8010GAH returns IDNF for any sub-physical-
 * sector access — verified on device 2026-07-18), so anything that reasons
 * about "one atomic sector" must reason in units of this.
 *
 * Kept as one definition rather than duplicated into each caller so a future
 * bump (a 4-logical / 2048-byte-physical drive) cannot leave a stale 2 behind
 * in a write path.
 */
#define ATA_PHYS_LOG  2u

/* Bytes per logical (LBA) sector. */
#define ATA_SECTOR_SZ 512u

/*
 * Read `count` (1..256) 512-byte sectors starting at LBA `lba` into `buf`
 * (must be 16-bit aligned; needs count*512 bytes). Returns 0 on success,
 * negative per the code table above.
 */
int ata_read_sectors(uint32_t lba, uint32_t count, void *buf);

/*
 * IDENTIFY DEVICE: read the drive's 256-word (512-byte) identify block
 * into `buf` (16-bit aligned). Takes no LBA, so it works even when sector
 * reads fail — used to learn the logical sector size (word 106 bit 12;
 * words 117/118 = size in 16-bit words). Returns 0 on success, negative on
 * a not-ready/DRQ timeout or drive error.
 */
int ata_identify(void *buf);

/*
 * Spin the drive DOWN (ATA STANDBY IMMEDIATE) for suspend. The drive still
 * accepts commands; the next media access spins it back up. Returns 0, or
 * negative on a not-ready/timeout.
 */
int ata_standby(void);

/*
 * Spin the drive back UP after ata_standby() and confirm it can transfer:
 * kicks a throwaway read of one whole PHYSICAL sector (this drive IDNFs a
 * sub-physical-sector read, so a 1-sector probe always failed) and waits out
 * the multi-second spin-up. Call once on wake before resuming normal reads.
 * Returns 0, or negative per the code table above. ata_is_parked() is
 * reconciled to 0 on EVERY exit path, success or not.
 */
int ata_wakeup(void);

/*
 * 1 while the platters are spun down (after ata_standby(), until the next read
 * or ata_wakeup() spins them back up). The single shared truth about drive
 * power state — the UI idle-timer parks only when this is 0, so it never
 * re-issues STANDBY on an already-parked drive.
 */
int ata_is_parked(void);

/*
 * Write `count` 512-byte sectors from `buf` to LBA `lba`, then FLUSH CACHE so
 * the data is on the platters and not just in the drive's write cache (which
 * is enabled by default — without the flush, a battery pull between the write
 * and the drive's own writeback loses it silently).
 *
 * *** UNVERIFIED ON HARDWARE, AND DELIBERATELY UNWIRED. *** Nothing in the
 * firmware calls this. It is a reviewed primitive; the calling layer lands
 * separately, and whoever writes it must first prove this on device against a
 * scratch LBA (write, read back, compare). Unlike a bad read, a bad write
 * destroys data.
 *
 * ALIGNMENT: `lba` and `count` must BOTH be multiples of the drive's logical-
 * sectors-per-physical-sector (2 on the stock 80 GB MK8010GAH, which returns
 * IDNF for any sub-physical-sector access), and `buf` must be 16-bit aligned.
 * Misalignment is rejected with -1 rather than emulated by a read-modify-
 * write: an RMW would rewrite bytes the caller never asked to touch and
 * widens the power-loss window over data that was previously safe.
 *
 * Returns 0 on success; -1 on a bad argument (misaligned LBA/count/buffer,
 * zero count, drive not ready), -2 on a timeout, -3 on a drive error (ERR/DF,
 * e.g. IDNF for a bad LBA).
 */
int ata_write_sectors(uint32_t lba, uint32_t count, const void *buf);

#endif /* CORE_HAL_HW_ATA_H */
