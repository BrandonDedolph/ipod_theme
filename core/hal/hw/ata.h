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

#endif /* CORE_HAL_HW_ATA_H */
