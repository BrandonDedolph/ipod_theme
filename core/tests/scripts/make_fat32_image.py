#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# Generate a minimal FAT32 volume image for the fat32 reader host test.
# Deliberately uses BytesPerSector = 2048 (like the stock iPod 80 GB drive)
# so the test exercises the sector-size translation (sec_ratio = 4), and a
# file that spans two clusters so cluster-chain following is covered.
#
# Layout (FS-sectors, SecPerClus = 1 so cluster == FS-sector):
#   0        boot sector (BPB)
#   1        (reserved)
#   2        FAT #1
#   3        FAT #2
#   4        cluster 2  = root directory
#   5        cluster 3  = HELLO.TXT part 1
#   6        cluster 4  = HELLO.TXT part 2
#   7        cluster 5  = "Intentions.flac" (long-name file)
#   8..11    spare data clusters
#
# HELLO.TXT is 3000 bytes of the pattern byte[i] = i & 0xFF (spans clusters
# 3 and 4). "Intentions.flac" exercises the VFAT long-name lookup: its 4-char
# extension can't fit an 8.3 name, so it is written with two 0x0F LFN entries
# (correct checksum) preceding a mangled short entry (INTENT~1.FLA), and can
# only be found by its long name.
#
# VARIANTS. With --variant NAME the generator instead emits a DELIBERATELY
# CORRUPT volume, for tests/fs/fat32_corrupt_test.c. Corrupt images are how you
# find out whether a parser that consumes whatever is on a user's disk fails
# cleanly or hangs / reads out of bounds. See VARIANTS below for the list.
#
# Usage: make_fat32_image.py <output-path> [--variant NAME]
#        make_fat32_image.py --list-variants

import struct
import sys

BPS = 2048          # BytesPerSector
SPC = 1             # SectorsPerCluster
RSVD = 2            # reserved sectors
NFATS = 2
FATSZ = 1           # FS-sectors per FAT
ROOTCLUS = 2
NUM_DATA_CLUS = 8

FILE_CLUS = 3
FILE_SIZE = 3000

# Long-name file: "Intentions.flac" (single cluster 5).
LFN_NAME = "Intentions.flac"
LFN_SHORT = b'INTENT~1FLA'       # mangled 8.3 short name (11 bytes)
LFN_CLUS = 5
LFN_SIZE = 500

DATA_START = RSVD + NFATS * FATSZ          # FS-sector of the data region
TOTAL_SEC = DATA_START + NUM_DATA_CLUS


def set_fat(img, base, entry, val):
    struct.pack_into('<I', img, base + entry * 4, val & 0x0FFFFFFF)


# Standard 8.3 short-name checksum used to bind LFN entries to their 8.3 entry.
def lfn_checksum(short11):
    s = 0
    for c in short11:
        s = (((s & 1) << 7) + (s >> 1) + c) & 0xFF
    return s


# Build the on-disk LFN entries for `longname`, in physical (reverse) order:
# highest sequence first (with the 0x40 last-marker), sequence 1 last, i.e.
# the run that immediately precedes the 8.3 entry.
def lfn_entries(longname, short11):
    chksum = lfn_checksum(short11)
    pos = [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]
    units = [ord(ch) for ch in longname]
    units.append(0x0000)                       # name terminator
    while len(units) % 13 != 0:                 # pad the last piece with 0xFFFF
        units.append(0xFFFF)
    nent = len(units) // 13
    entries = []
    for seq in range(1, nent + 1):
        ent = bytearray(32)
        ent[0] = seq | (0x40 if seq == nent else 0x00)
        ent[11] = 0x0F                          # LFN attribute
        ent[13] = chksum
        chunk = units[(seq - 1) * 13: seq * 13]
        for k in range(13):
            struct.pack_into('<H', ent, pos[k], chunk[k])
        entries.append(ent)
    entries.reverse()                           # on disk: highest seq first
    return entries


# --- corrupt variants -------------------------------------------------------
#
# Everything a FAT parser reads comes off a disk the user controls: a half-
# unplugged copy, a drive with bad sectors, or a volume that simply isn't the
# filesystem we think it is. Each variant below is one realistic way that goes
# wrong, and the corresponding assertion in fat32_corrupt_test.c is always the
# same shape: fail cleanly and bounded, never hang, never read out of bounds.
VARIANTS = {
    "good":
        "the reference volume (what every other fat32 test uses)",
    "cyclic-fat":
        "HELLO.TXT's FAT chain loops 3 -> 4 -> 3, and the ROOT directory's "
        "chain loops onto itself. A reader that just follows next_cluster() "
        "until EOC never terminates.",
    "oob-cluster":
        "HELLO.TXT's first cluster is far past the end of the volume, and the "
        "root holds a second entry whose cluster is 1 (below the first valid "
        "data cluster). Both index outside the data region.",
    "fat16-bpb":
        "a genuine FAT16 boot sector. Its BPB_FATSz32/BPB_RootClus offsets "
        "fall inside FAT16's BS_DrvNum/BS_BootSig/BS_VolID/BS_VolLab fields, "
        "so they read back NONZERO — which is exactly why a FAT32 reader that "
        "only checks 'fatsz != 0 && rootclus >= 2' accepts it and then walks "
        "a FAT16 volume as if it were FAT32.",
    "orphan-lfn":
        "an LFN run whose checksum does not match the 8.3 entry that follows "
        "it (a stale run left by an interrupted rename), plus a second LFN run "
        "at the very end of the directory with no 8.3 entry after it at all.",
    "truncated":
        "the image is cut off after the FAT region: every data-region read "
        "fails. Models a drive that dies mid-transfer.",
}

# A real FAT16 boot sector, byte for byte, so the mis-detection it triggers is
# the one a user would actually hit (an iPod restored to FAT16 by iTunes on a
# small drive, or a card formatted elsewhere).
def build_fat16_bpb():
    img = bytearray(TOTAL_SEC * BPS)
    img[0:3] = b'\xEB\x3C\x90'
    img[3:11] = b'MSDOS5.0'
    struct.pack_into('<H', img, 11, BPS)        # BytesPerSec
    img[13] = SPC                               # SecPerClus
    struct.pack_into('<H', img, 14, RSVD)       # RsvdSecCnt
    img[16] = NFATS                             # NumFATs
    struct.pack_into('<H', img, 17, 512)        # RootEntCnt: NONZERO => FAT16
    struct.pack_into('<H', img, 19, TOTAL_SEC)  # TotSec16 (FAT16 uses this)
    img[21] = 0xF8                              # media descriptor
    struct.pack_into('<H', img, 22, FATSZ)      # FATSz16 (FAT32 leaves this 0)
    struct.pack_into('<I', img, 32, 0)          # TotSec32: 0 on a small FAT16
    # 36..: the FAT16 extended boot record. On FAT32 these same offsets are
    # BPB_FATSz32 (36) and BPB_RootClus (44), which is the whole problem.
    img[36] = 0x80                              # BS_DrvNum
    img[37] = 0x00                              # BS_Reserved1
    img[38] = 0x29                              # BS_BootSig
    struct.pack_into('<I', img, 39, 0x12345678)  # BS_VolID
    img[43:54] = b'NO NAME    '                 # BS_VolLab
    img[54:62] = b'FAT16   '                    # BS_FilSysType
    img[510] = 0x55
    img[511] = 0xAA
    return img


def main():
    if "--list-variants" in sys.argv:
        for name, why in VARIANTS.items():
            print(f"{name}: {why}")
        return
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    variant = "good"
    if "--variant" in sys.argv:
        variant = sys.argv[sys.argv.index("--variant") + 1]
        args = [a for a in args if a != variant]
    if len(args) != 1:
        sys.exit("usage: make_fat32_image.py <output-path> [--variant NAME]")
    if variant not in VARIANTS:
        sys.exit(f"unknown variant {variant!r}; "
                 f"known: {', '.join(sorted(VARIANTS))}")
    out_path = args[0]

    if variant == "fat16-bpb":
        with open(out_path, 'wb') as fh:
            fh.write(build_fat16_bpb())
        return

    img = bytearray(TOTAL_SEC * BPS)

    # --- boot sector / BPB (FS-sector 0) ---
    img[0:3] = b'\xEB\x58\x90'
    img[3:11] = b'MSDOS5.0'
    struct.pack_into('<H', img, 11, BPS)        # BytesPerSec
    img[13] = SPC                               # SecPerClus
    struct.pack_into('<H', img, 14, RSVD)       # RsvdSecCnt
    img[16] = NFATS                             # NumFATs
    img[21] = 0xF8                              # media descriptor
    struct.pack_into('<H', img, 19, 0)          # TotSec16: 0, per the FAT32 spec
    struct.pack_into('<I', img, 32, TOTAL_SEC)  # TotSec32: the real sector count
    struct.pack_into('<I', img, 36, FATSZ)      # FATSz32
    struct.pack_into('<I', img, 44, ROOTCLUS)   # RootClus
    img[510] = 0x55
    img[511] = 0xAA                             # boot signature

    # --- FATs (both copies identical) ---
    for base in (RSVD * BPS, (RSVD + FATSZ) * BPS):
        set_fat(img, base, 0, 0x0FFFFFF8)       # media
        set_fat(img, base, 1, 0x0FFFFFFF)       # reserved / EOC
        set_fat(img, base, 2, 0x0FFFFFFF)       # root dir: single cluster
        set_fat(img, base, 3, 4)                # HELLO.TXT: clus 3 -> 4
        set_fat(img, base, 4, 0x0FFFFFFF)       # HELLO.TXT: clus 4 -> EOC
        set_fat(img, base, LFN_CLUS, 0x0FFFFFFF)  # long-name file: single clus

        if variant == "cyclic-fat":
            # HELLO.TXT: 3 -> 4 -> 3 -> ... , and the root directory points at
            # itself. Neither chain ever reaches an EOC marker.
            set_fat(img, base, 2, ROOTCLUS)
            set_fat(img, base, 4, FILE_CLUS)

    # --- root directory (cluster 2 == FS-sector DATA_START) ---
    hello_clus = FILE_CLUS
    if variant == "oob-cluster":
        # Well past the last data cluster on this volume (and past what the
        # image file even contains), so every read of it is off the end.
        hello_clus = 0x00FF0000

    e = DATA_START * BPS
    img[e:e + 11] = b'HELLO   TXT'              # 8.3 name
    img[e + 11] = 0x20                          # attr: archive
    struct.pack_into('<H', img, e + 20, (hello_clus >> 16) & 0xFFFF)
    struct.pack_into('<H', img, e + 26, hello_clus & 0xFFFF)
    struct.pack_into('<I', img, e + 28, FILE_SIZE)

    if variant == "oob-cluster":
        # A second entry pointing BELOW the first legal data cluster. Cluster
        # numbers 0 and 1 are reserved; (clus - 2) underflows for cluster 1.
        e += 32
        img[e:e + 11] = b'LOWCLUS TXT'
        img[e + 11] = 0x20
        struct.pack_into('<H', img, e + 20, 0)
        struct.pack_into('<H', img, e + 26, 1)
        struct.pack_into('<I', img, e + 28, FILE_SIZE)

    if variant == "orphan-lfn":
        # A stale LFN run: it claims the name "Ghost.flac" but its checksum
        # binds it to a DIFFERENT short name, so a conforming reader must throw
        # the run away and surface the following file under its own 8.3 name.
        e += 32
        for ent in lfn_entries("Ghost.flac", b'NOTTHIS FLA'):
            img[e:e + 32] = ent
            e += 32
        img[e:e + 11] = b'REAL    TXT'
        img[e + 11] = 0x20
        struct.pack_into('<H', img, e + 20, 0)
        struct.pack_into('<H', img, e + 26, LFN_CLUS)
        struct.pack_into('<I', img, e + 28, LFN_SIZE)

    # "Intentions.flac": its LFN run, then the mangled 8.3 entry.
    e += 32
    for ent in lfn_entries(LFN_NAME, LFN_SHORT):
        img[e:e + 32] = ent
        e += 32
    img[e:e + 11] = LFN_SHORT                   # 8.3 short name
    img[e + 11] = 0x20                          # attr: archive
    struct.pack_into('<H', img, e + 20, (LFN_CLUS >> 16) & 0xFFFF)
    struct.pack_into('<H', img, e + 26, LFN_CLUS & 0xFFFF)
    struct.pack_into('<I', img, e + 28, LFN_SIZE)

    if variant == "orphan-lfn":
        # A dangling LFN run with NO 8.3 entry after it: the directory simply
        # ends. Nothing may be surfaced for it, and the walk must still stop.
        e += 32
        for ent in lfn_entries("Dangling.flac", b'DANGLE~1FLA'):
            img[e:e + 32] = ent
            e += 32

    if variant == "cyclic-fat":
        # Fill the REST of the root cluster with deleted (0xE5) slots so there
        # is no 0x00 end-of-directory marker. Without this the walk stops at
        # the terminator and never follows the looping chain at all — the
        # cluster has to be full for the cycle to matter.
        e += 32
        clus_end = (DATA_START + (ROOTCLUS - 2)) * BPS + SPC * BPS
        while e < clus_end:
            img[e] = 0xE5
            e += 32
        e -= 32                                 # leave `e` on the last slot
    # the following entry is left 0x00 => end of directory

    # --- file content (clusters 3,4, contiguous from FS-sector DATA_START+1) ---
    f = (DATA_START + (FILE_CLUS - 2)) * BPS
    for i in range(FILE_SIZE):
        img[f + i] = i & 0xFF

    # --- long-name file content (cluster 5), a distinct pattern ---
    f = (DATA_START + (LFN_CLUS - 2)) * BPS
    for i in range(LFN_SIZE):
        img[f + i] = (i ^ 0x5A) & 0xFF

    if variant == "truncated":
        # Cut the image off right after the FAT region: the BPB and the FATs
        # are readable, every data-region read fails. A drive that died
        # mid-transfer looks exactly like this.
        img = img[:DATA_START * BPS]

    with open(out_path, 'wb') as fh:
        fh.write(img)


if __name__ == '__main__':
    main()
