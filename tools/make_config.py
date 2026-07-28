#!/usr/bin/env python3
"""make_config.py — create (and verify) CORECFG.DAT, the pre-allocated settings
file the firmware overwrites in place.

WHY THIS TOOL EXISTS
--------------------
The firmware's FAT32 driver is READ-ONLY apart from one hole: kernel/config.c
overwrites the data sectors of an already-existing file. It cannot create,
grow, shrink, move or delete anything — doing so would mean allocating clusters
and rewriting directory entries, which is precisely where filesystem corruption
lives. So the file has to exist before the device ever sees it, and that is this
tool's whole job.

Because the file is written through the host's normal filesystem, its FAT chain
and directory entry are created by an OS that knows how to do that correctly.
The device then only ever changes bytes INSIDE the file's first cluster: no FAT
entry, no directory entry, no free-count, no FSInfo. Nothing the read path
depends on is ever mutated.

WORKFLOW
--------
  1. Mount the iPod's FAT32 data partition (on WSL it is usually the drive
     letter the iPod shows up as; on Linux, /media/... ).

  2. Create the file ONCE, in the VOLUME ROOT (not in Music/):

         python3 tools/make_config.py --create /mnt/d

     It writes CORECFG.DAT: two 1024-byte slots, slot 0 holding a valid
     default record and slot 1 zeroed, padded out to --size bytes (32 KiB by
     default, one cluster on a stock 80 GB volume).

     Re-running is safe: an existing file that already validates is left alone
     unless you pass --force. The point of that guard is that a re-import must
     not silently reset the user's saved settings.

  3. Flush and unmount properly (on WSL: `powershell.exe Write-VolumeCache D`),
     then boot the firmware. It prints, over UART:

         core: cfg load <1|0> writable <1|0> seq <n> lba <slot0>/<slot1>

  4. BEFORE letting the device write anything, confirm that LBA. On the host:

         sudo python3 tools/make_config.py --verify /dev/sdX

     (or point --verify at a raw image). This reads the MBR, finds the FAT32
     partition, walks the root directory for CORECFG.DAT and computes the
     absolute 512-byte LBA of its first cluster using the SAME formula
     core/fs/fat32.c uses:

         part_lba + (data_start + (clus - 2) * sec_per_clus) * (bytes_per_sec / 512)

     The number it prints MUST equal the `lba` the firmware printed. If it does
     not, stop: do not change a setting, power the device down. A mismatch
     there is the bug that overwrites somebody's music library.

     --verify also dumps both slots' magic/version/seq/CRC status, which is how
     you confirm afterwards that saves are alternating slots.

The record format is defined in core/kernel/config.h / config.c; this file is
the host half of it and the two must agree byte for byte.
"""

import argparse
import binascii
import os
import struct
import sys

# ---- record format (must match core/kernel/config.c) ----------------------

SECTOR = 512
PHYS_LOG = 2                      # logical sectors per physical sector
SLOT_BYTES = SECTOR * PHYS_LOG    # 1024 — one whole physical sector
SLOTS = 2
MIN_BYTES = SLOT_BYTES * SLOTS    # 2048 — the smallest file the device accepts

MAGIC = 0x45524F43                # 'C''O''R''E' little-endian
VERSION = 2                       # v1 = settings only; v2 adds the resume locator

OFF_MAGIC, OFF_VERSION, OFF_LENGTH, OFF_SEQ, OFF_PAYLOAD = 0, 4, 6, 8, 12
OFF_CRC = SLOT_BYTES - 4          # 1020; the CRC covers bytes [0, OFF_CRC)

# Payload v1: one byte per field, in this order. Mirrors the P_* enum in
# config.c — order and width are the on-disk contract.
PAYLOAD_FIELDS = [
    ("shuffle",           0),
    ("repeat",            0),     # 0=off 1=all 2=one
    # MUST match settings_defaults() in core/ui/settings.c (which has this on).
    # These are not "the tool's defaults" — the record this writes is the one the
    # firmware loads forever after, and a valid record always beats
    # settings_defaults(). A 0 here silently disabled Resume on a device whose
    # CORECFG.DAT was created by this tool, with every other setting persisting
    # correctly (diagnosed on hardware 2026-07-27).
    ("resume_on_startup", 1),
    ("crossfade",         0),
    ("volume",            70),
    ("bass",              0),     # signed
    ("treble",            0),     # signed
    ("balance",           0),     # signed
    ("backlight_secs",    15),
    ("backlight_bright",  32),
    ("theme",             0),
    ("clicker",           1),
]
PAYLOAD_V1_LEN = len(PAYLOAD_FIELDS)  # 12

# Payload v2 appends the resume locator: three 32-bit LE fields, no v1 offset
# moved. Mirrors P_RES_* in config.c.
#   resume_hash   name_hash() of the playing track's ext-trimmed filename
#   resume_secs   elapsed seconds within it
#   resume_total  its length, kept as a cross-check against a same-named track
# A fresh file has nothing to resume, so all three are written as 0.
RESUME_FIELDS = ["resume_hash", "resume_secs", "resume_total"]
PAYLOAD_LEN = PAYLOAD_V1_LEN + 4 * len(RESUME_FIELDS)   # 24

SIGNED = {"bass", "treble", "balance"}

FILENAME = "CORECFG.DAT"


def crc32(data: bytes) -> int:
    """The same CRC-32 config.c computes: reflected, poly 0xEDB88320,
    init/final 0xFFFFFFFF — i.e. plain zlib/binascii CRC-32."""
    return binascii.crc32(data) & 0xFFFFFFFF


def encode(values: dict, seq: int) -> bytes:
    """Build one SLOT_BYTES record. `values` overrides the defaults above."""
    rec = bytearray(SLOT_BYTES)
    struct.pack_into("<I", rec, OFF_MAGIC, MAGIC)
    struct.pack_into("<H", rec, OFF_VERSION, VERSION)
    struct.pack_into("<H", rec, OFF_LENGTH, PAYLOAD_LEN)
    struct.pack_into("<I", rec, OFF_SEQ, seq & 0xFFFFFFFF)
    for i, (name, default) in enumerate(PAYLOAD_FIELDS):
        v = int(values.get(name, default))
        if name in SIGNED:
            struct.pack_into("<b", rec, OFF_PAYLOAD + i, max(-128, min(127, v)))
        else:
            struct.pack_into("<B", rec, OFF_PAYLOAD + i, max(0, min(255, v)))
    for j, name in enumerate(RESUME_FIELDS):
        v = int(values.get(name, 0)) & 0xFFFFFFFF
        struct.pack_into("<I", rec, OFF_PAYLOAD + PAYLOAD_V1_LEN + 4 * j, v)
    struct.pack_into("<I", rec, OFF_CRC, crc32(bytes(rec[:OFF_CRC])))
    return bytes(rec)


def decode(rec: bytes):
    """Validate + decode one slot. Returns (seq, {field: value}) or None."""
    if len(rec) < SLOT_BYTES:
        return None
    if struct.unpack_from("<I", rec, OFF_MAGIC)[0] != MAGIC:
        return None
    ver = struct.unpack_from("<H", rec, OFF_VERSION)[0]
    if ver == 0 or ver > VERSION:
        return None
    length = struct.unpack_from("<H", rec, OFF_LENGTH)[0]
    if length < PAYLOAD_V1_LEN or length > OFF_CRC - OFF_PAYLOAD:
        return None
    if crc32(bytes(rec[:OFF_CRC])) != struct.unpack_from("<I", rec, OFF_CRC)[0]:
        return None
    seq = struct.unpack_from("<I", rec, OFF_SEQ)[0]
    out = {}
    for i, (name, _d) in enumerate(PAYLOAD_FIELDS):
        fmt = "<b" if name in SIGNED else "<B"
        out[name] = struct.unpack_from(fmt, rec, OFF_PAYLOAD + i)[0]
    # The v2 tail is gated on the record's own `length`, exactly as
    # config_decode() gates it — so a v1 record (length 12) still verifies
    # here instead of being read out of the zero padding.
    if length >= PAYLOAD_LEN:
        for j, name in enumerate(RESUME_FIELDS):
            out[name] = struct.unpack_from(
                "<I", rec, OFF_PAYLOAD + PAYLOAD_V1_LEN + 4 * j)[0]
    return seq, out


# ---- create ---------------------------------------------------------------

def do_create(volume: str, size: int, force: bool) -> int:
    if size < MIN_BYTES:
        print(f"error: --size must be at least {MIN_BYTES} "
              f"({SLOTS} slots of {SLOT_BYTES} B)", file=sys.stderr)
        return 2
    if not os.path.isdir(volume):
        print(f"error: {volume} is not a directory", file=sys.stderr)
        return 2

    path = os.path.join(volume, FILENAME)

    if os.path.exists(path) and not force:
        # Never clobber settings the user has already saved. A re-import must
        # be idempotent, not destructive.
        with open(path, "rb") as f:
            head = f.read(MIN_BYTES)
        best = None
        for s in range(SLOTS):
            d = decode(head[s * SLOT_BYTES:(s + 1) * SLOT_BYTES])
            if d and (best is None or d[0] > best[0]):
                best = d
        if best:
            print(f"{path} already exists and holds a valid record "
                  f"(seq {best[0]}) — leaving it alone. Use --force to reset.")
            return 0
        if os.path.getsize(path) >= MIN_BYTES:
            print(f"{path} exists but holds no valid record; rewriting it.")
        else:
            print(f"{path} exists but is too small ({os.path.getsize(path)} B "
                  f"< {MIN_BYTES}); rewriting it.")

    # Slot 0: a valid defaults record at seq 1. Slot 1: zeroed, so the device's
    # first save lands there and the two-slot alternation starts cleanly.
    blob = bytearray(size)
    blob[0:SLOT_BYTES] = encode({}, 1)
    # blob[SLOT_BYTES : 2*SLOT_BYTES] stays zero => invalid, as intended.

    # Write, then fsync — on removable media the data has to actually leave the
    # page cache before the volume is unmounted, or the device sees a file full
    # of nothing.
    with open(path, "wb") as f:
        f.write(bytes(blob))
        f.flush()
        os.fsync(f.fileno())

    print(f"wrote {path}: {size} B, slot 0 = defaults (seq 1), slot 1 = empty")
    print("Now unmount/flush the volume before unplugging "
          "(WSL: powershell.exe Write-VolumeCache D).")
    print("Then boot the firmware and compare its 'cfg ... lba' line against")
    print(f"  sudo python3 {os.path.basename(__file__)} --verify <device-or-image>")
    return 0


# ---- emit (test fixture) --------------------------------------------------

def do_emit(path: str) -> int:
    """Write just the two-slot region (MIN_BYTES) to `path`: slot 0 a valid
    defaults record at seq 1, slot 1 zeroed.

    This is what core/tests/kernel/config_test.c decodes with the FIRMWARE's
    config_decode(). The host encoder and the device decoder have to agree
    byte for byte — if they ever drift, a freshly-created CORECFG.DAT would be
    silently rejected on the device and settings would never persist. Wiring
    that agreement into the test suite is cheaper than discovering it on a
    user's iPod.
    """
    blob = bytearray(MIN_BYTES)
    blob[0:SLOT_BYTES] = encode({}, 1)
    with open(path, "wb") as f:
        f.write(bytes(blob))
    return 0


# ---- verify ---------------------------------------------------------------

def _u16(b, o):
    return b[o] | (b[o + 1] << 8)


def _u32(b, o):
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)


def do_verify(dev: str) -> int:
    """Resolve CORECFG.DAT's absolute LBA the way core/fs/fat32.c does, and
    dump both slots. READ-ONLY — this never writes to `dev`."""
    try:
        f = open(dev, "rb")
    except OSError as e:
        print(f"error: cannot open {dev}: {e}", file=sys.stderr)
        print("(reading a raw block device usually needs sudo)", file=sys.stderr)
        return 2

    with f:
        def rd(lba, count=1):
            f.seek(lba * SECTOR)
            b = f.read(count * SECTOR)
            if len(b) != count * SECTOR:
                raise IOError(f"short read at LBA {lba}")
            return b

        # --- MBR -> the FAT32 partition (type 0x0B / 0x0C) ---
        mbr = rd(0)
        if _u16(mbr, 510) != 0xAA55:
            print("error: no MBR signature — is this the whole disk?",
                  file=sys.stderr)
            return 1
        part_lba = None
        for p in range(4):
            e = 0x1BE + 16 * p
            if mbr[e + 4] in (0x0B, 0x0C):
                part_lba = _u32(mbr, e + 8)
                break
        if part_lba is None:
            print("error: no FAT32 (0x0B/0x0C) partition in the MBR",
                  file=sys.stderr)
            return 1

        # The stock 80 GB iPod records the partition start in 2048-byte units,
        # not 512 — the same ambiguity kernel/main.c resolves by trying both.
        # Try as-is, then x4, and take whichever yields a valid FAT32 BPB.
        for cand in (part_lba, part_lba * 4):
            bs = rd(cand)
            if (_u16(bs, 510) == 0xAA55 and _u16(bs, 11) in (512, 1024, 2048, 4096)
                    and _u16(bs, 22) == 0 and _u16(bs, 17) == 0):
                part_lba = cand
                break
        else:
            print("error: no valid FAT32 BPB at the partition start",
                  file=sys.stderr)
            return 1

        bps = _u16(bs, 11)
        sec_ratio = bps // SECTOR
        spc = bs[13]
        rsvd = _u16(bs, 14)
        nfats = bs[16]
        fatsz = _u32(bs, 36)
        root_clus = _u32(bs, 44)
        data_start = rsvd + nfats * fatsz          # in FS-sectors

        print(f"partition LBA   : {part_lba}")
        print(f"BytesPerSec     : {bps}  (sec_ratio {sec_ratio})")
        print(f"SecPerClus      : {spc}  (cluster {spc * bps} B)")
        print(f"data_start      : FS-sector {data_start}")
        print(f"root cluster    : {root_clus}")

        def clus_lba(clus):
            """The formula core/fs/fat32.c's fat32_file_lba() uses. The
            sec_ratio factor is the whole point: without it the address is
            4x too small on a 2048-byte volume and lands inside the FAT."""
            fs_sec = data_start + (clus - 2) * spc
            return part_lba + fs_sec * sec_ratio

        def read_clus(clus):
            return rd(clus_lba(clus), spc * sec_ratio)

        def fat_next(clus):
            off = clus * 4
            fs_sec = rsvd + off // bps
            sec = rd(part_lba + fs_sec * sec_ratio, sec_ratio)
            return _u32(sec, off % bps) & 0x0FFFFFFF

        # --- walk the root directory for CORECFG.DAT (8.3 short name) ---
        want = b"CORECFG DAT"
        found = None
        clus = root_clus
        for _ in range(4096):                      # bounded, like the firmware
            data = read_clus(clus)
            done = False
            for o in range(0, len(data), 32):
                ent = data[o:o + 32]
                if ent[0] == 0x00:
                    done = True
                    break
                if ent[0] == 0xE5 or ent[11] == 0x0F or (ent[11] & 0x08):
                    continue
                if ent[0:11].upper() == want:
                    first = (_u16(ent, 20) << 16) | _u16(ent, 26)
                    found = (first, _u32(ent, 28))
                    done = True
                    break
            if found or done:
                break
            clus = fat_next(clus)
            if clus < 2 or clus >= 0x0FFFFFF8:
                break

        if not found:
            print(f"\n{FILENAME} NOT FOUND in the volume root.")
            print("The firmware will keep defaults and will not write anything.")
            print("Run --create against the mounted volume first.")
            return 1

        first_clus, size = found
        lba0 = clus_lba(first_clus)
        lba1 = lba0 + PHYS_LOG

        print(f"\n{FILENAME}")
        print(f"  first cluster : {first_clus}")
        print(f"  size          : {size} B "
              f"({'OK' if size >= MIN_BYTES else 'TOO SMALL, need %d' % MIN_BYTES})")
        print(f"  slot 0 LBA    : {lba0}   (0x{lba0:08X})")
        print(f"  slot 1 LBA    : {lba1}   (0x{lba1:08X})")
        print("  alignment     : "
              + ("OK" if lba0 % PHYS_LOG == 0
                 else "MISALIGNED — the device will refuse to save"))
        print()
        print("  >>> These MUST match the firmware's UART line:")
        print(f"  >>>   core: cfg load .. writable .. seq .. "
              f"lba {lba0:08X}/{lba1:08X}")
        print("  >>> If they differ, DO NOT change a setting. Power off.")

        if size < MIN_BYTES:
            return 1

        print()
        blob = rd(lba0, PHYS_LOG * SLOTS)
        newest = None
        for s in range(SLOTS):
            raw = blob[s * SLOT_BYTES:(s + 1) * SLOT_BYTES]
            d = decode(raw)
            if d:
                seq, vals = d
                print(f"  slot {s}: VALID   seq={seq}  {vals}")
                if newest is None or seq > newest[0]:
                    newest = (seq, s)
            else:
                kind = "empty" if not any(raw) else "invalid (magic/version/CRC)"
                print(f"  slot {s}: {kind}")
        if newest:
            print(f"  -> firmware will load slot {newest[1]} (seq {newest[0]}) "
                  f"and write slot {1 - newest[1]} next")
        else:
            print("  -> no valid record; firmware keeps defaults and will "
                  "write slot 0 on the first save")

    return 0


def main():
    ap = argparse.ArgumentParser(
        description="Create/verify CORECFG.DAT, the pre-allocated settings "
                    "file the firmware overwrites in place.")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--create", metavar="MOUNTPOINT",
                   help="create CORECFG.DAT in the root of a MOUNTED volume")
    g.add_argument("--verify", metavar="DEVICE",
                   help="read-only: resolve CORECFG.DAT's absolute LBA from a "
                        "raw disk/image and dump both slots")
    g.add_argument("--emit", metavar="FILE",
                   help="write just the %d-byte two-slot region to FILE "
                        "(test fixture: the firmware's decoder must accept "
                        "it — see core/tests/kernel/config_test.c)" % MIN_BYTES)
    ap.add_argument("--size", type=int, default=32 * 1024,
                    help="file size in bytes (default 32768 = one cluster on "
                         "a stock 80 GB volume; minimum %d)" % MIN_BYTES)
    ap.add_argument("--force", action="store_true",
                    help="overwrite an existing CORECFG.DAT even if it holds "
                         "a valid record (resets saved settings)")
    args = ap.parse_args()

    if args.create:
        return do_create(args.create, args.size, args.force)
    if args.emit:
        return do_emit(args.emit)
    return do_verify(args.verify)


if __name__ == "__main__":
    sys.exit(main())
