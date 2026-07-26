# Settings persistence — design

Status: **implemented** (`core/kernel/config.c`, `core/kernel/config.h`,
`fat32_file_lba()` in `core/fs/fat32.c`, `tools/make_config.py`, host tests in
`core/tests/kernel/config_test.c`). The write path is **not yet verified on
hardware** — see the bring-up procedure at the top of `config.c`.

The core idea below is what shipped: a pre-allocated fixed-size file,
overwritten in place, mutating zero filesystem metadata. **Two things in the
original proposal were wrong and were corrected during implementation**; both
are marked ⚠️ CORRECTED in place, because reproducing either of them destroys
user data.

## Problem

`settings_t` (shuffle, repeat, volume, balance, bass, treble, backlight
timeout/brightness, theme) is initialised by `settings_defaults()` on every
boot. There is no write path, so every cold start forgets the user's choices.
The FAT32 driver (`core/fs/fat32.c`) is **read-only** by design, and the disk
holds the user's music — so the bar for "safe to write" is high.

## Non-goals

- General FAT write support (allocating clusters, creating/renaming files,
  updating directory entries, extending file length). Explicitly out of scope —
  that is where disk corruption risk lives.
- Persisting playback position / "resume on startup". That needs per-track
  state and is a separate feature; this covers `settings_t` only.

## Approach: in-place overwrite of a pre-allocated config file

The host importer creates a **fixed-size, contiguous** file `CORECFG.DAT` in
the volume root, once, at import time. The device never allocates, grows, or
shrinks it — it only **overwrites the bytes of its existing data sector(s)**.
Because the file already exists at a known length with clusters already in the
FAT and a directory entry already written, a data-only overwrite touches **zero
filesystem metadata**: no FAT entries, no directory entries, no free-count. The
on-disk structures the read-only driver relies on are never mutated.

This is the same trick the library index uses to be cheap to read
(`CORELIB.IDX` is host-built and only ever read); here we additionally write
back into an equally pre-placed file.

### Why one *physical* sector (⚠️ CORRECTED)

The proposal said: *"We define a 512-byte on-disk record so a write is exactly
one 512-byte ATA sector — atomic at the drive level."*

**That is wrong on this hardware, twice over.**

1. The stock 80 GB drive (MK8010GAH) reports 2 logical sectors per physical
   sector and **rejects sub-physical-sector access with IDNF** — see the
   `ATA_PHYS_LOG` comment in `core/hal/hw/ata.c` (verified on device
   2026-07-18: `count=1` IDNFs, `count=2` at an even LBA succeeds). A
   512-byte write is not merely non-atomic here, it is **refused outright**.
   `ata_write_sectors` enforces that alignment on both LBA and count.
2. Even with a whole physical sector, medium-level atomicity is not the only
   failure mode: the drive can fail the command, or lose power mid-command.
   "The sector is now neither the old nor the new record" has to be
   survivable, and an appeal to atomicity does not make it so.

So the record is **one whole physical sector — `ATA_PHYS_LOG * 512` = 1024
bytes** — at a physical-sector-aligned LBA, and the two-slot scheme below is
**mandatory, not optional**. `CORECFG.DAT` is sized to one cluster (32 KiB by
default); only its first 4 logical sectors (2 slots) are ever written.

## LBA resolution (device)

Mirrors how `CORELIB.IDX` is already found (`index_root_cb` in `main.c`):

1. At mount, `fat32_readdir(fs, fs->root_clus, cb, …)`; the callback matches
   `CORECFG.DAT` (case-insensitive) and captures `first_clus` + `size`.
2. Convert the first cluster to an **absolute LBA** (⚠️ CORRECTED). The
   proposal gave the formula as

   ```
   fs->part_lba + cluster_fs_sector(fs, clus)              /* WRONG */
   ```

   **That is 4x too small on the stock volume and lands inside the FAT.**
   `cluster_fs_sector()` returns **FS-sectors** — units of
   `fs->bytes_per_sec`, which is 2048 on the stock 80 GB volume, so
   `fs->sec_ratio` is 4. Every read path multiplies by `sec_ratio` before it
   reaches the block callback; see `read_fs_sector()`:

   ```c
   fs->read(fs->ud, fs->part_lba + fs_sec * fs->sec_ratio, fs->sec_ratio, buf);
   ```

   and `fat32_read_file()` / `fat32_stream_read()` compose it identically. The
   correct formula, and the one implemented, is

   ```
   fs->part_lba + cluster_fs_sector(fs, clus) * fs->sec_ratio   /* RIGHT */
   ```

   Dropping the factor does not land somewhere harmless: on a real volume it
   lands a few hundred sectors past the partition start, i.e. **inside the
   FAT**, destroying the allocation tables for the whole volume.

   The shipped helper is deliberately not the "just expose the address"
   one-liner the proposal sketched:

   ```c
   int fat32_file_lba(const fat32_t *fs, uint32_t first_clus,
                      uint32_t *lba, uint32_t *max_sectors);
   ```

   It also reports how many contiguous sectors are addressable (exactly one
   cluster — the chain is deliberately **not** followed, so a fragmented file
   can never walk the writer into a neighbouring file), and it **fails
   closed**: on any doubt it returns negative and writes 0 to both out-params,
   so a caller that ignores the return code still has no address.

3. Validate before trusting it: `size >= 2048` (two slots), cluster in range
   via the reader's existing `cluster_valid()`, no uint32 overflow in either
   multiply, the run inside the partition, LBA ≠ 0, LBA above `part_lba`, and
   LBA aligned to `ATA_PHYS_LOG`. If `CORECFG.DAT` is absent or fails any of
   these → **fall back to `settings_defaults()` and disable writes** (the file
   appears after the next import). All of this is re-run **immediately before
   every single write**; no LBA is ever cached across calls.

## On-disk record layout (1024 bytes, little-endian)

```
off   size  field
0     4     magic     'C''O''R''E'  (0x45524F43 LE)
4     2     version   layout version (1)
6     2     length    meaningful payload bytes that follow (fwd-compat)
8     4     seq       monotonic write counter, wrapping (newest wins)
12    N     payload   the packed settings fields (see below), zero-padded
1020  4     crc32     CRC-32 over bytes [0 .. 1020) — EVERYTHING above it
```

⚠️ The CRC covers the **header too**, not just the payload as the proposal had
it. `seq` is what decides which slot wins, so a corrupt `seq` under a
payload-only CRC would let a stale or garbage record beat a good one. Covering
the zero padding as well costs nothing and removes the "which bytes are
covered" question from the format entirely. `config_test.c` asserts a flipped
bit in the header, in the payload, and in the padding are each rejected.

The polynomial is the standard reflected 0xEDB88320 with init/final
0xFFFFFFFF — i.e. plain zlib/`binascii.crc32`, which is what lets
`tools/make_config.py` write a record the firmware accepts. That agreement is
pinned by a test: meson generates a fixture with the real host tool and the
test decodes it with the firmware's own `config_decode()`.

Payload (v1), each a fixed width, endian-fixed — **not** a raw `struct` dump
(avoid ABI/padding coupling):

As shipped (offsets are within the payload; see the `P_*` enum in `config.c`
and `PAYLOAD_FIELDS` in `make_config.py`, which must stay in lockstep):

```
0   shuffle            u8   (0/1)
1   repeat             u8   (0=off 1=all 2=one)
2   resume_on_startup  u8   (0/1)
3   crossfade          u8   (0/1)
4   volume             u8   (0..100)
5   bass               i8   (-12..12)
6   treble             i8   (-12..12)
7   balance            i8   (-100..100)
8   backlight_secs     u8   (0/5/10/15/30/60)
9   backlight_bright   u8   (1..32)
10  theme              u8   (0=Linen 1=Onyx …)
11  clicker            u8   (0=Off …)
```

Loader validates magic + version + length + crc32. Any mismatch → defaults
(writes stay enabled if the *file* resolved — writing slot 0 is how we
recover). Every decoded field is additionally **clamped to its legal range**:
a valid CRC proves the bytes are the bytes that were written, not that they
are sane, and nothing downstream re-validates `settings_t` before it reaches
the hardware.

Version handling: `version == 0` or `version > CONFIG_VERSION` is declined
rather than half-interpreted. Forward compatibility runs the other way — a v1
record stays readable by a future v2+ loader because `length` says how much of
the payload v1 defined.

### Torn-write safety (two slots — REQUIRED, ⚠️ CORRECTED)

The proposal called this optional, on the grounds that a single 512-byte write
is atomic. See "Why one *physical* sector" above: that premise does not hold
here, so **two slots are mandatory**.

Two slots (physical sector 0 and physical sector 1 of the file) with the `seq`
counter: read both, pick the valid record with the newer `seq`, and always
write to the *other* slot. A failed, torn, or power-interrupted write can only
damage the slot that is not the current newest-valid one, so a good previous
record always survives. Costs one extra physical sector in the same cluster.

`seq` comparison is a **wrapping** signed difference, not `a > b`: at
`0xFFFFFFFF → 0` a naive comparison picks the older slot and silently reverts
the user's settings by one save, forever. `config_test.c` covers the wrap.

## Write flow (device)

Trigger: debounced, **not** on every wheel tick. Persist when the user leaves
Settings (or on a 2–3 s idle after the last change), and coalesce — one write
per settling, so a volume sweep is a single sector write, not dozens.

As shipped, in `config_save()` — order matters, and address validation comes
first so a refusal costs nothing:

1. Refuse outright if `CORECFG.DAT` was not found at mount time.
2. Pick the slot that is **not** the one the current record was read from.
3. **Re-resolve and re-validate** that slot's LBA from the file's own cluster
   via `fat32_file_lba()`. Nothing is cached across calls.
4. Pack `settings_t` → payload, bump `seq`, compute crc32, zero-pad to 1024.
5. `ata_write_sectors(lba, ATA_PHYS_LOG, buf)` — which independently re-checks
   alignment and issues FLUSH CACHE.
6. Only on success, update the in-RAM `seq`/slot. On failure they are left
   alone, so the previous good record stays the newest and the next attempt
   re-targets the same disposable slot.

Best-effort: failure is logged over UART and non-fatal — settings simply do
not persist that session; nothing else is affected.

Triggers (`kernel/main.c`): every site that mutates `g_settings` calls
`settings_touch()`; `settings_commit(0)` runs once per main-loop pass and
writes 3 s after the last change; `settings_commit(1)` forces a write when the
user leaves the Settings screen, on suspend, and on PMU power-off. The
debounced (non-forced) path additionally declines while the drive is parked and
audio is playing, so a settings write never costs an audible spin-up for
something with no deadline — the forced paths cover every graceful exit.

The write is placed **before** the idle spin-down check in the main loop, so a
due save lands while the platters are still turning.

## HAL surface: `ata_write_sectors`

Already landed in `core/hal/hw/ata.c` (see its banner). Signature as shipped:

```c
int ata_write_sectors(uint32_t lba, uint32_t count, const void *buf);
```

`lba` and `count` must **both** be multiples of `ATA_PHYS_LOG`, and `buf`
16-bit aligned; misalignment is **rejected**, not bounced — a read-modify-write
of a physical sector would rewrite bytes the caller never asked to touch and
widen the power-loss window over data that was previously safe.

`config.c` is the **only** caller in the firmware. Mitigations, as implemented:

- **Guard LBA range.** The address comes exclusively from the validated
  directory lookup + `fat32_file_lba()`, re-run before every write. There is no
  LBA literal anywhere in the write path, and the resolver never returns 0.
- **Cache flush (0xE7)** after the data so the record is durable before we
  claim success — the drive's write cache is on by default, so without it a
  battery pull, a MENU+SELECT reset, or the idle STANDBY path loses an
  acknowledged write silently.
- **Clock bracket.** The whole request (data phase *and* flush) is wrapped in
  `cpu_boost`/`cpu_unboost`, as every read command already is. `IDE0_PRI_TIMING0`
  is never rewritten, so the PIO strobes are frozen at the chainloader's 80 MHz
  calibration while `kernel/clock.c` moves the core between 30 and 80. A read at
  the wrong clock is slow or fails loudly; a **write** at the wrong clock can
  land corrupt bytes on the platter, which does not fail at all. The debounced
  save runs from the idle main loop — exactly where the core may be at 30 MHz.
- **Single cluster only.** The chain is never followed, so fragmentation cannot
  walk the writer into a neighbouring file. Contiguity is therefore *not* a
  correctness precondition, only a convenience if the record ever grows.

## Host tool: `tools/make_config.py`

A standalone tool rather than a fold into `build_index.py`, so creating the
config file is not coupled to re-importing music.

- `--create MOUNTPOINT` writes `CORECFG.DAT` into the **volume root**: slot 0 a
  valid defaults record at `seq 1`, slot 1 zeroed (so the device's first save
  starts the alternation cleanly), padded to `--size` (32 KiB default = one
  cluster on a stock 80 GB volume). `fsync`s before returning, and **refuses to
  overwrite a file that already validates** unless `--force` — a re-import must
  not silently reset the user's saved settings.
- `--verify DEVICE` is **read-only** and is the pre-flight for the first
  on-device write: it walks the MBR → BPB → root directory on a raw disk or
  image and prints the absolute LBA of `CORECFG.DAT`'s first cluster computed
  with the same formula `fat32_file_lba()` uses, plus both slots' validity and
  `seq`. That number must match the firmware's UART `lba` line **before**
  anyone changes a setting.
- `--emit FILE` writes just the two-slot region; meson uses it to generate the
  fixture the host test decodes with the firmware's own `config_decode()`.

## Testing (host — done)

`core/tests/kernel/config_test.c`, `meson test -C build-sim config`. Links the
real `fat32.c` and `ata.c`, so the traces are the drivers' own.

- **LBA resolution** over an in-RAM FAT32 volume with BytesPerSector 2048 at a
  non-zero partition base — the geometry that makes the right and wrong
  formulas differ. Pins the exact LBA and explicitly asserts it is **not** the
  `sec_ratio`-less value. Plus fail-closed behaviour on cluster 0/1, an
  out-of-range cluster, null args, and a zeroed (never-mounted) `fat32_t`.
- **Record codec:** round-trip of every field at range extremes; rejection of
  bad magic, version 0, a future version, length 0, an oversized length, a
  flipped payload bit, a flipped **header** bit, a flipped **padding** bit, an
  all-zero slot, and garbage; and clamping of a CRC-valid but out-of-range
  record.
- **Two-slot selection:** newer `seq` wins either way round; a slot torn
  exactly as the hardware would tear it (first logical sector new, second still
  old, including its CRC) loses to the older good one; a virgin slot and a
  garbage slot are skipped; **wraparound** resolves to the wrapped slot;
  both-bad keeps defaults but stays writable; absent / too-small file disables
  writing.
- **Write grammar:** the exact `WRITE SECTORS 0x30` task-file programming (LBA
  bytes checked, not just addresses), two DRQ-out phases of 256 halfwords each
  carrying the encoded record, the post-data BSY wait, and `FLUSH CACHE 0xE7` —
  538 events matched exactly, with **slot alternation** asserted across two
  consecutive saves. Plus: a refused save emits **zero** bus events.
- **Host/device format agreement:** the fixture written by the real
  `make_config.py` is decoded by the firmware's `config_decode()`.

**What the tests cannot prove:** that the drive does what the trace says. The
register grammar, the alignment and the addresses are checked against a mock
bus; the physical write is not. See the bring-up procedure at the top of
`config.c`.

## Device bring-up (NOT YET DONE)

The full procedure is at the top of `core/kernel/config.c`. In short: scratch
disk, confirm the firmware's UART `lba` line equals `make_config.py --verify`'s
number **before** changing any setting, then save / power-cycle / confirm the
setting survived **and** that the music is still there and `fsck.vfat -n`
reports the volume clean.
```
