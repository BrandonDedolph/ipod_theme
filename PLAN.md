# Custom iPod Firmware — Plan

A from-scratch firmware for the iPod Video (5G / 5.5G), shipping the
**Cabinet** UI as the shell and **Linen** as the only visual style.
Replaces Rockbox entirely on the device. Uses Rockbox source as a
reference / datasheet but does not link any of its code.

Project codename TBD (placeholder: `core`). Suggestions welcome —
naming convention so far: short, tactile nouns (Cabinet, Linen).

---

# Where the plan stands — 2026-07-28

Phases 0–4 are substantially done and the firmware boots itself on real
hardware. This section is the ledger; everything below the "original plan"
divider is the April document, kept verbatim as the record of what we
*intended* — several of its decisions were reversed for good reasons, and
the reasons are more useful than a tidy rewrite.

## Phase ledger

| Phase | Planned | Where it landed |
|---|---|---|
| **0** — hardware reference | 3–4 wks | **DONE.** `core/docs/hw/` (9 subsystem docs + README), cited, and gated in CI by `check_hw_consistency.py` against `hal/hw/pp5022.h`. Two facts have since been corrected *from the device* (the image checksum has no `MODEL_NUM` seed; an image body is at `devOffset + 0x800`). |
| **1** — bootable skeleton | 3 wks | **DONE for hardware, ABANDONED for the sim.** `crt0.S`, linker script, kernel skeleton, `hal.h` all shipped; first hardware boot 2026-07-17. There is no `core-sim` executable — see "decisions we reversed". |
| **2** — drivers | 6–8 wks | **DONE except USB.** LCD (+`bcm_init`), click-wheel, clock/boost/idle, ATA, FAT32, I²S/DMA/WM8758B, charge + battery all run on silicon. **USB MSC was never started** — recovery goes through the ROM's disk mode instead, which turned out to be enough. |
| **3** — filesystem + audio engine | 4 wks | **DONE for FLAC.** Streaming decode off the disk, tag reading, a host-built index in place of a device-side tagcache. MP3 builds but is disabled; the other codecs are untouched. No ReplayGain, no crossfade, no pitch. |
| **4** — UI integration + installer | 3 wks | **UI DONE; installer NOT.** The whole shell, browser, Now Playing, overlays and Settings are written from scratch in C. The Go CLI can pack/unpack a `.ipod` image and nothing else — `install` / `flash` / `recover` are stubs, and flashing is `ipodpatcher` by hand. |
| **5** — polish & power | ongoing | **In progress.** CPU idle scaling, HDD spin-down, codec power-down + clock gating, suspend-to-RAM, album-art cache, panic screen with a readable on-screen report. Panel sleep at idle is written but switched off. No crash log to disk, no battery-curve calibration. |

## Decisions we reversed, and why

These are the parts of the original document that are now simply wrong.
Each was a deliberate change, not drift.

- **Bootloader: neither option 1 nor option 2. We boot directly.** The plan
  was to keep the Rockbox/ipodloader2 bootloader and ship a chainloaded
  `core.ipod`. As of 2026-07-28 `core` *is* the OSOS image in the firmware
  partition; the boot ROM hands straight to `crt0.S`, which does the MMAP0
  remap itself. No chainloader, no boot menu, and Apple's firmware is no
  longer on the device. The plan's premise that "do not replace the factory
  bootloader" is the low-risk path held right up until it didn't: the
  chainloader was adding a boot menu, seconds of boot time, and a set of
  inherited-state assumptions (BCM idle at frame one, backlight already
  lit) that were silently propping the firmware up. The recovery argument
  that made the plan cautious turned out to be answered by the ROM: **Select
  + Play disk mode runs before any image loads**, proven on this device with
  our firmware as the sole OSOS and a black screen.
- **FAT32: written from scratch, not FatFs.** Vendoring FatFs would have
  pulled a whole write path we did not want on a user's music disk. Ours is
  read-only by construction, every chain walk bounded, every cluster
  validated, and it is fuzz-adjacent tested under ASan/UBSan against corrupt
  images.
- **Codecs: `dr_flac` + `dr_mp3`, not Helix.** No Helix MP3/AAC, no ALAC, no
  Tremor, no libopus. The device is FLAC-only in practice — `dr_mp3`'s float
  synthesis filter cannot hit real time on an FPU-less ARM7, which is the
  concrete form of the plan's own "no floating point in core code" rule
  coming back around. The plan's claim that codecs run on the COP is also
  unrealised: everything decodes on the CPU.
- **Tagcache: a host-built index, not a device-side database.** `CORELIB.IDX`
  is built by `tools/build_index.py` and loaded in one read. No on-device
  scan, no delta rebuild, no atomic-rename machinery — and no write path, so
  the plan's "data durability / atomic tagcache writes" requirement is moot
  rather than met.
- **The simulator does not exist.** `hal/sim/sim_hal.c` builds into a static
  library that no executable links. `make sim` builds the *host test suite*.
  Everything the plan hung off the sim — golden-frame visual regression,
  scripted navigation, boot-timing gates, soak tests, `core test` — is
  therefore not real. What replaced it is the MMIO golden-trace suite: each
  freestanding driver host-compiled against a recording mock bus and
  asserted to emit its exact ordered register grammar. That is a narrower
  net but it is the only automated check of hardware code, and it has caught
  real bugs.
- **The Go CLI is one command, not eleven.** `core firmware pack/unpack`
  works. `install`, `update`, `recover`, `info`, `flash`, `debug`, `sim`,
  `test`, `build`, `release` are stubs or absent, there is no embedded sim,
  no signing, no release pipeline, and no ipodpatcher replacement.
- **Cabinet was not ported; it was rewritten.** The plan assumed porting a
  Rockbox plugin's UI logic by swapping `rb->` calls. The shipped UI is
  first-party C written against `core/ui/` primitives, driven by the
  `design_reference/*.jsx` mockups. This is also why the licence posture is
  cleaner than the plan assumed it would need to be.
- **Screenshots are host reproductions, not sim captures.** The plan's
  golden-frame pipeline never existed, so `docs/screens/render.py` is a
  standalone Python/PIL reimplementation of the UI — real Nunito faces, real
  palette, real layout, but not one pixel from firmware code. It can drift
  silently. There is no on-device capture path.

## Open decisions — where they landed

The six questions the original document left open all have answers now.

1. **Bootloader** — neither. Direct boot; our image is the OSOS.
2. **Build system** — Meson, with a thin `Makefile` as the human entry point
   (`make hw` / `make ipod` / `make sim` / `make verify-hw`).
3. **Rust** — no. Pure C11 + ARM assembly throughout.
4. **Settings storage** — a binary record, not a text file: two alternating
   physical-sector slots in a pre-allocated `CORECFG.DAT`, CRC-32 over the
   whole record including the header, sequence number decides the winner.
   Text lost because there is no general FAT writer to rewrite a file with,
   and a fixed-size record can be overwritten in place without touching a
   single byte of filesystem metadata.
5. **Project name** — still `core`. Nothing better proposed.
6. **Curated settings list** — nine rows: Playback, Sound, Theme, Display,
   Clicker, About, Boot Details, Disk Mode, Reset. Every placeholder that
   did nothing (Crossfade, ReplayGain, Skip Length, Stereo Width, Shortcuts,
   Language) was removed rather than shipped inert.

## Performance budget — measured against the targets

The April table was written before anything ran. Here is what the hardware
actually does. Unmeasured rows are marked as such rather than guessed.

| Metric | Target | Actual | |
|---|---|---|---|
| Cold boot → interactive | < 2.0 s | **8.4 s measured**, 5.1 s of it one FLAC seek (since fixed); post-fix total not yet read off the device | Instrumented rather than estimated — Settings → Boot Details shows the per-phase split live |
| Resume from sleep | < 200 ms | not measured | — |
| Selector frame time | ≤ 16.6 ms | not measured; damage-tracked partial presents shipped | — |
| Album-art decode (cached row) | 0 ms | 0 ms — chips are pre-converted RGB565, cache hit is a copy | Met |
| Album-art decode (cold) | < 80 ms | not measured; the 28 px chip is an exact-size file, a 1:1 copy with no resample | — |
| Library load | < 300 ms / 10k | **~1.8 s** for the current library, one read of `CORELIB.IDX` | Missed |
| Binary size | < 300 KB (no codecs) | **298 KB text total, codecs included** | Roughly on target, though not measured the way the row defines it |
| Static RAM (excl. audio buffer) | ≤ 2 MB | **~2.45 MB** (11.45 MB bss − 8 MB disk buffer − 1 MB PCM ring) | Slightly over |
| Audio buffer | 50 MB | **8 MB** anti-skip disk buffer + 1 MB PCM ring | Deliberately smaller; the whole image lives in a 32 MB SDRAM window |
| Idle CPU clock | 24 MHz | **30 MHz + halt** | Changed |
| HDD spin-up frequency | ≤ 1 / 5 min | 20 s idle spin-down; the 8 MB buffer is ~73 s of FLAC ahead | Not measured over a session |
| Screen-on idle power | < 30 mW | not measured — no instrumentation | — |

The quality-bar section below has held up better than the budget, with two
exceptions worth naming: **ReplayGain is not implemented**, and **gapless is
implemented but has not been confirmed by ear on the device**. "Crash
transparency" is half done — there is a panic screen, but nothing writes a
crash log to disk (nothing can; see cluster allocation, below).

## What's next

In the order they're worth doing.

1. **Wire the M3U8 reader into a Playlists UI (read only).** The parser is
   merged, unit-tested and bounded, and is currently wired to *nothing* —
   `--gc-sections` strips it out of the shipped image. Resolving a parsed
   path to a cluster is a segment walk; no new filesystem capability is
   needed. This is the cheapest real feature left.
2. **A screen-tuned font face.** Advances (26.6), kerning (1,700–3,700 pairs
   per face) and per-face tracking are all fixed and *measured* — and the
   type still reads wrong at 9–13 px. Nunito ships no TrueType hinting
   bytecode. The remaining lever is the face, not more spacing tuning.
   `tools/text_preview.c` renders the real stack on the host, so a candidate
   can be judged without a flash.
3. **Re-enable panel sleep at idle.** Disabled because it left the screen
   solid white until a reboot; two of the three stated preconditions
   (`bcm_init()` exists, the post-wake relight is deferred) have since been
   met.
4. **Search.** Not implemented, and the index is already in RAM.
5. **Playlist writing — i.e. FAT32 cluster allocation.** The only write we
   have is an in-place overwrite of one pre-allocated file's first cluster,
   which touches zero filesystem metadata *by design*. Saving a playlist
   means allocating and chaining clusters, updating the FAT and FSInfo, and
   growing a directory entry, on a disk full of someone's music. This also
   unblocks crash logs to disk. It is a project, not a chunk — do not let it
   ride along with item 1.

Verification debt worth clearing while nearby: the 500 mA charge-current
change needs an inline USB current meter, and gapless needs a listen.

---

---

# The original plan, April 2026 — kept for the record

*Everything below this line is the document as first written, before any
code existed. Read it as intent, not as a description of the system. The
ledger above says where each part actually went.*

---

## Goals

**Lean and fast is the whole point.** Every architectural decision below
is downstream of that. If a feature can't earn its cycles, RAM, or
binary bytes, it doesn't ship.

- One device, one UI, one theme. No plugins, no games, no file browser,
  no voice, no theme engine, no language packs, no settings nobody uses.
- Boot directly into Cabinet. No Rockbox menu underneath, no second
  shell to escape to.
- **Faster than Rockbox in measurable ways**, not vibes:
  - Cold boot under 2 seconds (Rockbox: 5–8 s).
  - UI frame at 60 fps with selector moves in ≤16 ms.
  - HDD wakes ≤1 per 5 min sustained playback at 256 kbps.
  - Helix MP3/AAC outperform libmad/libfaad2 on ARM7 — measurable
    cycle reduction per decoded frame.
- Battery life at parity with or better than Rockbox (CPU boost
  discipline, larger audio buffer, fewer HDD spin-ups, lower idle floor).
- A clean license posture (no GPL-by-association — most components are
  BSD / MIT / Apache / RPSL / public domain).
- A hardware reference doc as a side-effect, valuable on its own.

## Performance budget

These are hard targets, treated as test gates — not aspirations.

| Metric                              | Target          | Rockbox (ref) |
|-------------------------------------|-----------------|---------------|
| Cold boot → interactive             | < 2.0 s         | ~5–8 s        |
| Resume from sleep                   | < 200 ms        | ~500 ms       |
| Selector frame time (UI)            | ≤ 16.6 ms       | ~30 ms        |
| Album-art decode (cached row)       | 0 ms (LRU hit)  | ~200 ms       |
| Album-art decode (cold)             | < 80 ms         | ~200 ms       |
| Tagcache load (10k tracks)          | < 300 ms        | ~1.5 s        |
| Binary size (kernel + UI, no codecs)| < 300 KB        | ~700 KB       |
| Static RAM (everything but A-buf)   | ≤ 2 MB          | ~6 MB         |
| Audio buffer (5.5G, 64 MB device)   | 50 MB           | ~30 MB        |
| Idle CPU clock                      | 24 MHz          | 30 MHz        |
| HDD spin-up frequency (256k MP3)    | ≤ 1 / 5 min     | ~1 / 2 min    |
| Screen-on idle power draw           | target < 30 mW  | ~50 mW        |

Numbers we don't promise: battery hours (depends on cell age) and
peak decode load (depends on file). We do commit to instrumenting them
and tracking regressions per build.

## Quality bar

Lean is about discipline, not cutting corners. These are things we
**will not** compromise to hit the performance budget — if a target
above conflicts with one of these, the target moves, not the quality.

- **Bit-perfect audio decode.** Every codec output matches the reference
  decoder bit-for-bit. We test against known-answer vectors per build.
- **Gapless playback** on all formats that support it (MP3 LAME tags,
  AAC, FLAC, ALAC, Vorbis, Opus). No 50 ms inter-track silence.
- **ReplayGain** (track + album) and a clean software volume curve.
  No clipping on full-scale signals at unity gain.
- **DAC driver correctness.** I²S clocks within spec, no jitter from
  CPU contention, proper de-emphasis where the source asks for it.
  The 5.5G's Wolfson is set up to spec, not "close enough."
- **Anti-aliased typography.** Nunito rendered with proper subpixel /
  grayscale anti-aliasing, hinted appropriately for 320×240. Not
  Rockbox's bitmap-font look.
- **No tearing.** UI updates synchronized to the LCD's frame interval;
  partial updates land between frames, not across them.
- **Smooth animations.** Selector slide, page transitions, art crossfade
  at 60 fps, easing curves chosen for feel, not just "they work."
- **Robust at every system boundary.** Malformed ID3 tags, truncated
  files, unplugged USB, full disks, corrupt FAT, low battery — none of
  these crash the firmware or corrupt the user's library. Internal code
  paths trust each other; boundary code is paranoid.
- **Crash transparency.** A panic produces a readable on-screen report
  *and* writes a crash log to disk for post-mortem. We don't hide
  failures.
- **Data durability.** Tagcache writes are atomic (write to temp, fsync,
  rename). Settings the same. Power loss never leaves a half-written
  index.

The leanness rules below are how we afford this quality bar — by
spending the budget where it shows, not on overhead.

## Design principles

These are non-negotiable rules. Every line of code respects them.

1. **No malloc.** Every allocation is static, decided at link time. The
   sub-allocator inside the audio buffer is a bump pointer with reset
   points, not a heap.
2. **No floating point in core code.** ARM7 has no FPU; every `float`
   is a soft-float library call (~100+ cycles). Fixed-point everywhere.
   Codecs are the exception (their internal fixed-point is already done).
3. **No printf in production builds.** Debug builds get a UART-backed
   `dbg()` macro that compiles out entirely with `-DRELEASE`.
4. **No libc dependency** in shipping code. We provide minimal
   `memcpy` / `memset` / `strcmp` / `strlen` / basic math — that's it.
5. **No abstractions without users.** Every interface has at least one
   real caller before it's written. No "for future flexibility" code.
6. **No background work without a budget.** Tasks declare their CPU
   slice; the scheduler enforces it. UI never starves.
7. **Damage-tracked rendering.** The UI redraws only the rectangles
   that changed. No full-frame repaints in the steady state.
8. **Pre-compute aggressively.** Glyph atlases, chrome bitmaps,
   tagcache indices — anything that can be done at boot or build time
   is done then, not in the hot path.
9. **Whole-program LTO + `-Os`** + `-ffunction-sections` /
   `-fdata-sections` / `--gc-sections`. Dead code does not ship.
10. **One language: English.** No language file infrastructure, no
    `.lng` files, no `str(LANG_*)` indirection. Strings are string
    literals.

## Non-goals

- Multi-target support. 5G and 5.5G share one build with a runtime
  branch in driver init. No other devices.
- Backwards compatibility with Rockbox themes, plugins, settings,
  tagcache files, or config formats. We migrate the user once at first
  boot and never look back.
- Third-party extensibility. There is no plugin API.
- A from-scratch codec library. We vendor existing decoders — the
  audio decoding problem is solved.

## Architecture overview

```
+---------------------------------------------+
|  Cabinet UI (C, compiled in)                |   ← apps/ui/
+---------------------------------------------+
|  Linen renderer (chrome, fonts, layout)     |   ← apps/ui/render/
+---------------------------------------------+
|  Tagcache (flat binary, mmap)               |   ← apps/db/
+---------------------------------------------+
|  Audio engine (decode → DSP → DMA)          |   ← apps/audio/
|  Codec ABI shim                             |
+--------+--------+--------+--------+---------+
| Helix  | Helix  | dr_    | ALAC   | Tremor  |
| MP3    | AAC    | flac   | (Apple)| (Vorbis)|   ← codecs/ (vendored)
+--------+--------+--------+--------+---------+
|  FatFs (FAT32, vendored)                    |   ← fs/
+---------------------------------------------+
|  HAL: LCD / Click wheel / ATA / Charge /    |   ← hal/
|       USB / DMA / Clock / GPIO / I2C        |
+---------------------------------------------+
|  Kernel: cooperative tasks, IRQs, COP IPC,  |   ← kernel/
|          static memory map, no malloc       |
+---------------------------------------------+
|  Boot: crt0.S, vector table, image header   |   ← boot/
+---------------------------------------------+
                  PP5021C SoC
```

**Memory model:** static. One large audio buffer (~50 MB) carved at
link time, with a sub-allocator for transient PCM / decoded art / ID3
metadata. No `malloc`, no fragmentation, no leaks.

**Concurrency:** cooperative tasks on CPU, codec work on COP, talking
via a tiny mailbox. No preemptive scheduler. IRQs handle DMA and
button events only.

## Codec stack

| Format | Library             | License        | Notes |
|--------|---------------------|----------------|-------|
| MP3    | Helix MP3           | RPSL (GPL-compat)| Faster than libmad on ARM7 |
| AAC    | Helix AAC           | RPSL           | Embedded gold standard |
| FLAC   | dr_flac             | Public domain  | Single-header, fixed-point |
| ALAC   | Apple's ALAC        | Apache 2.0     | Reference impl, portable |
| Vorbis | Tremor              | BSD            | Xiph fixed-point fork |
| Opus   | libopus             | BSD            | Xiph reference, fixed-point mode |
| WAV    | Hand-written        | (ours)         | ~200 lines |

A common shim adapts each to a uniform `decoder_t { open, decode_frame,
seek, close }` interface. Codecs run on the COP; PCM goes through a
shared ring buffer to the CPU's DMA pump.

## Toolchain

**Firmware (on-device, ARM):**
- `arm-none-eabi-gcc` 13+ (modern toolchain; gcc 9 is fine if needed).
- Build system: **Meson** (preferred) or plain Make. Pick one and don't
  reach for autotools.
- Debug: serial UART through the dock connector (pin documented),
  panic dumps to LCD if UART is unavailable.
- Optional: Rust for new code. ARM7TDMI is a Rust target (`thumbv4t-none-eabi`)
  via `cargo build -Zbuild-std`. Defer this decision until phase 2 — C
  is the safe default.

**Host tooling (everything off-device):**
- **Go 1.22+**. One static binary per platform, no runtime dependencies
  the user has to install. Cross-compiles trivially from one host.

## Host tooling: the `core` CLI

All user-facing and developer-facing tooling ships as a **single Go
binary** named `core`. Cross-compiled to `linux/amd64`, `linux/arm64`,
`darwin/amd64`, `darwin/arm64`, `windows/amd64`. Distributed via GitHub
Releases. No interpreters, no scripts the user has to install, no
"first install ipodpatcher then…" — just download `core` and run it.

### Subcommands

| Command           | Purpose |
|-------------------|---------|
| `core install`    | First-time install onto a stock iPod (writes bootloader + firmware) |
| `core update`     | Update an iPod already running our firmware |
| `core recover`    | Restore factory firmware (un-install) or reflash |
| `core info`       | Detect connected iPod; report model, capacity, firmware version |
| `core flash`      | Dev: push `core.ipod` + assets to attached iPod, prompt eject |
| `core debug`      | Open dock-connector UART stream (live log viewer) |
| `core sim`        | Launch the interactive simulator (window) |
| `core test`       | Run headless test suite: boot, scripted nav, golden-frame diff, codec KAT |
| `core build`      | Convenience wrapper for the C firmware build (`make hw` / `make sim`) |
| `core release`    | Build, sign, and package a release zip |

Every command takes `--json` for scripting; every command is idempotent
where possible; every destructive operation prompts unless `--yes`.

### Internal Go packages

```
cli/
├── cmd/core/                # main, subcommand wiring
├── internal/
│   ├── ipod/                # USB device detection, capability probe
│   ├── firmware/            # iPod firmware-partition format, image header, checksums
│   ├── bootloader/          # ipodpatcher-equivalent: partition R/W, patch ops
│   ├── disk/                # FAT32 access (vendors github.com/diskfs/go-diskfs)
│   ├── release/             # zip layout, signing, integrity
│   ├── sim/                 # embedded sim binary mgmt, headless control, frame capture
│   ├── golden/              # PNG diff for visual regression
│   ├── serial/              # UART over dock (vendors go.bug.st/serial)
│   ├── tagcache/            # host-side indexer (mirrors firmware format for `core release`)
│   └── ui/                  # terminal UI (progress, prompts, colored output)
└── go.mod
```

### Replacing ipodpatcher

The current install flow leans on `ipodpatcher` (mature C tool, GPL-2,
~3000 LoC). We absorb its functionality into `internal/bootloader/`
in Go so the user installs **one** binary. ipodpatcher source is the
reference; we re-implement the firmware-partition R/W, image format,
and checksum logic clean. The Rockbox bootloader image itself is
embedded in the Go binary (it's < 50 KB) and written to the firmware
partition by `core install`.

### Embedded simulator

The C simulator (`core-sim`, links the kernel + codecs against the SDL
HAL) ships **inside** the Go binary via `go:embed`, one variant per
host platform. `core sim` extracts to the user's cache dir on first run
and exec's it. Result: the user gets a true single-binary install of
the dev environment too. Binary size budget: ~30 MB for the Go CLI
including the embedded sim, well under what users tolerate for desktop
tools.

For CI / headless mode, the sim has a no-SDL `--headless` build that
renders to a memory framebuffer, dumps PNGs to a directory, and exits
deterministically. `core test` drives this entirely from Go.

### Privileges

Writing the firmware partition needs raw block-device access:
- Linux: `/dev/sdX` (sudo or udev rule)
- macOS: `/dev/diskN` (sudo, plus `diskutil unmountDisk`)
- Windows: `\\.\PhysicalDriveN` (Administrator)

`core install` detects the platform, prompts for elevation cleanly
(via `pkexec` / `osascript -e 'do shell script ... with administrator
privileges'` / Windows UAC manifest), and never silently escalates.

## Repo layout (proposed)

```
core/
├── boot/                  # crt0.S, image header, linker script
├── kernel/                # tasks, IPC, static memory, IRQs
├── hal/
│   ├── lcd/               # 5G + 5.5G init sequences
│   ├── wheel/             # quadrature decode, accel
│   ├── ata/               # IDE, spin-down state machine
│   ├── usb/               # MSC, control endpoints
│   ├── charge/            # battery curve, charge SM
│   ├── audio/             # I²S, DMA, DAC init (5G / 5.5G branch)
│   └── clock/             # PLL, boost, idle
├── fs/                    # FatFs vendor + adapter
├── codecs/                # Helix, dr_flac, ALAC, Tremor, libopus, WAV
├── apps/
│   ├── audio/             # engine, DSP, replaygain
│   ├── db/                # tagcache build/query
│   └── ui/
│       ├── cabinet.c      # the shell (originally ported from a Rockbox plugin)
│       └── render/        # Linen chrome, font cache, primitives
├── cli/                   # the `core` Go binary (host tooling, see above)
│   ├── cmd/core/
│   ├── internal/
│   └── go.mod
├── sim/                   # SDL-based C simulator (links firmware code with sim HAL)
├── docs/
│   └── hw/                # phase-0 hardware reference (see below)
└── tests/
    ├── golden/            # reference frames for visual regression
    ├── codec-vectors/     # known-answer inputs + expected PCM hashes
    └── scripts/           # JSON test scenarios consumed by `core test`
```

## Bootloader strategy

**Do not** replace the iPod's boot ROM or factory bootloader. Risk is
too high for the marginal value.

Two options for chain-loading our firmware:

1. **Repurpose the Rockbox bootloader** — it's already widely installed,
   documented, and recoverable. Our firmware ships as a `core.ipod`
   image in the same format the Rockbox bootloader expects. User
   installs the Rockbox bootloader once, then drops our image on the
   FAT partition. Lowest risk, fastest to first boot.
2. **Fork the Rockbox bootloader** — minor edits to its image-loading
   stage so our header/magic differs. Slightly cleaner branding, but
   means we own the bootloader maintenance too. Defer.

**Recommendation:** start with option 1, revisit later. The bootloader
is ~5 KB; whether we own it doesn't change the architectural story.

**Recovery path:** the iPod has a hardware "diagnostic mode" reachable
by holding Select+Play at boot, which lets the factory firmware reflash
from disk. We document this prominently. Users who brick recover by
forcing factory firmware mode and reinstalling.

## Local development & simulator

The HAL is split into two implementations sharing one interface:

```
hal/
├── hal.h           # the contract: lcd_*, wheel_*, ata_*, audio_*, ...
├── hw/             # ARM target — touches real registers
└── sim/            # host target — backed by SDL + a disk-image file
```

The kernel, codecs, audio engine, and Cabinet UI link against `hal.h`
and don't care which backend is below. Two build outputs:

- `core.ipod` — ARM, flashable, what ships.
- `core-sim` — native host binary, opens an SDL window at 320×240.

**The sim's job:**

| HAL surface  | Sim implementation |
|--------------|--------------------|
| LCD          | SDL surface, real partial-update semantics |
| Click wheel  | keyboard mapping (←/→ tracks, ↑/↓ scroll, Enter select, Esc back, hold Shift = fast-scroll) |
| ATA          | file-backed disk image (`disk.img`, FAT32) |
| Audio        | SDL_audio output, real-time playback works |
| USB          | no-op; mount the disk image on host directly to add music |
| Charge       | scripted battery curve; can simulate "low battery" path |
| Clock        | wall-clock time + real `usleep`; boost is logged but not enforced |
| UART debug   | stdout |

**Dev loop (sim):**

```bash
core build sim     # builds core-sim under sim/, ~5s after first build
core sim disk.img  # opens the SDL window — play, navigate, all real
```

The sim runs codecs natively on the host CPU, so playback is real-time
and we can iterate on UI without a working ARM audio path. ASan + UBSan
on sim builds catches bugs that would be a hardware mystery otherwise.

**Headless mode** (`core test` and `core sim --headless`):
renders frames to PNG, accepts a JSON input script, exits cleanly. The
Go test runner drives this for visual regression — golden reference
frames live in `tests/golden/`, the Go `internal/golden` package does
pixel diff and produces a side-by-side HTML report on failures. The
runner replaces the early shell + Xvfb + Python harness entirely.

**Dev loop (hardware):**

```bash
core build hw            # produces build/core.ipod
core flash               # auto-detects mounted iPod, syncs files, prompts eject
core debug               # opens dock-connector UART stream live
```

Total cycle from `core build hw` to "watching the new build run" is
~15 seconds with the iPod already mounted. `core flash --watch`
re-flashes on every rebuild for tight inner loops.

## Install & update flow

### First install (factory iPod → our firmware)

The user downloads one binary (`core` for their platform) and runs:

```bash
core install                    # auto-detects iPod, prompts for confirmation
                                # writes bootloader + firmware in one step
```

Under the hood:

1. `core` detects the connected iPod (USB scan), reports model and
   capacity, asks the user to confirm.
2. Prompts for elevation if the platform needs it (sudo / UAC).
3. Writes the embedded Rockbox bootloader to the firmware partition
   (re-implementing the ipodpatcher logic in Go).
4. Mounts the data partition; writes `/core.ipod` and `/.core/` assets:
   ```
   /core.ipod              ← firmware image (the bootloader loads this)
   /.core/
       fonts/              ← pre-rasterized glyph atlases
       chrome/             ← Linen background bitmaps
       defaults.cfg        ← factory settings
       version             ← ABI / firmware version stamp
   /Music/                 ← user's library (untouched)
   ```
5. Unmounts cleanly; tells the user to disconnect and reboot.

On reboot the bootloader finds `/core.ipod`, loads it, our firmware
boots into Cabinet and scans the music library on first run.

### Recovery (any time the device won't boot)

Hold **Select + Play** at boot → iPod enters Apple's diagnostic disk
mode (in ROM, works regardless of firmware state). From a host:

```bash
core recover                    # restores factory firmware + bootloader
core recover --reflash          # re-installs our firmware fresh
```

We document this prominently in `RECOVERY.md` shipped in every release.

### Updates (already running our firmware)

Two paths, both safe:

**Path A — host-driven update (default):**

```bash
core update                     # downloads latest release, applies it
core update path/to/release.zip # applies a specific zip
```

The tool:
- Detects the connected iPod (in update mode).
- Verifies the release zip's signature.
- Writes new files alongside old (`/core.ipod.new`, etc.).
- Atomically renames into place (FAT directory-entry rename is atomic).
- Keeps `/core.ipod.prev` as a fallback the bootloader uses if the new
  image fails to boot N times.
- Unmounts cleanly.

**Path B — on-device self-update (later, optional):**

The firmware includes a "Settings → Update from disk" flow that, given
a release zip already on the device, validates and installs it without
needing a host. Useful for users without a computer handy.

### Versioning & migration

- `core.ipod` carries an embedded version string and ABI version.
- On boot, firmware compares ABI version against `/.core/version`. If
  the on-disk format has changed (tagcache layout, settings schema), it
  runs a forward-only migrator before booting Cabinet.
- Tagcache and settings writes are atomic (write-temp, fsync, rename),
  so a power loss mid-update never corrupts state.
- We never auto-downgrade. Downgrade is a manual reflash.

### Release artifacts

Two distinct artifacts per release, both published to GitHub Releases.

**1. Host CLI binaries** — what users download:

```
core-linux-amd64
core-linux-arm64
core-darwin-amd64
core-darwin-arm64
core-windows-amd64.exe
checksums.txt              ← signed (minisign or cosign)
```

Each is a single static Go binary, ~30 MB (includes embedded sim).
Self-updating: `core update --self` pulls the latest CLI for the host
platform.

**2. Firmware payload zip** — what `core install` / `core update`
writes to the iPod:

```
core-firmware-v0.3.0.zip
├── core.ipod                  ← firmware image
├── .core/                     ← assets (fonts, chrome, defaults)
├── INSTALL.md                 ← first-install guide
├── RECOVERY.md                ← brick recovery
├── CHANGELOG.md
└── manifest.json              ← signed manifest, version, ABI
```

Target firmware-payload size: < 2 MB. The CLI fetches this from GitHub
Releases on `core update`, verifies the signature against an embedded
public key, then writes it to the iPod. Users never need to handle this
zip directly unless they want to.

## Testing strategy

Five layers, all driven by `core test`. The Go test runner is the
single entry point; underneath it shells out to the C sim, compares
codec outputs, drives serial logging during hardware runs, and produces
HTML reports.

1. **Unit tests** (host, fast, run on every commit):
   - C: codec known-answer tests — decode reference inputs (in
     `tests/codec-vectors/`), compare PCM output bit-for-bit.
   - C: tagcache parse/serialize roundtrip; layout math; button-decode
     state machine; IPC mailbox semantics.
   - Go: `cli/` packages — image-format parser, USB device detection,
     bootloader image patcher, FAT32 helpers.
   - Run via `core test unit` (wraps the C test binary + `go test`).
2. **Sim integration tests** (host, slower):
   - `core test sim` runs JSON scenarios from `tests/scripts/`:
     boot-to-interactive timing (< 2 s gate), scripted navigation, frame
     capture, golden-frame diff, audio decode hash.
   - Stress: 1000 random navigations, check sub-allocator high-water
     marks return to baseline.
3. **Hardware smoke tests** (manual checklist, every release):
   - `core test hw` walks the user through a checklist, capturing UART
     logs and timing as they go. Logs land in `tests/runs/<timestamp>/`.
   - Cold boot, USB roundtrip, charge cycle, sleep/wake, every codec,
     hardware buttons including hold switch, brightness range, edge-case
     files (missing tags, huge tags, malformed APIC).
4. **Battery-life regression** (dedicated device, weekly):
   - `core test battery` starts a scripted playback session, leaves the
     device running, scrapes the UART for power telemetry, plots
     mAh/hour over time across builds.
5. **Soak tests** (sim, overnight):
   - `core test soak` runs headless playback of a long playlist, dumps
     stack / sub-allocator stats every 60 s, fails if any high-water
     mark moves.

CI (GitHub Actions) runs layers 1 and 2 on every push using `core
test`. Layers 3 and 4 are release gates. Layer 5 runs nightly on a
self-hosted runner.

## Phases

### Phase 0 — Hardware reference doc (3–4 weeks evenings)

**Deliverable:** `docs/hw/` — a single coherent reference for the iPod
5G/5.5G hardware, written by reading Rockbox source + leaked PP5022
docs + iPodLinux wiki and stitching into one artifact.

Sections:
- PP5021C SoC: memory map, clock tree, PLL register sequences, COP IPC
- LCD controller: init sequence (5G and 5.5G variants), framebuffer
  layout, partial-update protocol, dither registers
- Click wheel: quadrature decode, button matrix, accel algorithm
- ATA controller: register set, spin-up/spin-down, DMA setup
- Audio: Wolfson DAC variants (5G vs 5.5G), I²S protocol, DMA chain
- Power: charge controller, battery curve, sleep modes
- USB: PHY init, endpoint setup
- Dock connector: UART pinout, accessory detect

This is valuable independent of the firmware project. If we stop here,
we've still produced the cleanest iPod 5G hardware reference on the
internet.

**Exit criteria:** every register we'll touch in phase 2 has a documented
purpose and example sequence.

### Phase 1 — Bootable skeleton + sim parity + Go CLI scaffold (3 weeks)

**Deliverable:** a `core.ipod` image that boots on hardware, a
`core-sim` host binary that runs against the same kernel + HAL
contract, and the Go `core` CLI scaffolded with `build`, `sim`, and
`flash` subcommands working.

- `crt0.S` + linker script + image header (Rockbox-bootloader-compatible)
- Kernel skeleton: task table, IRQ vector, idle task
- `hal/hal.h` contract defined and frozen
- `hal/hw/`: minimal LCD init (solid color), UART driver
- `hal/sim/`: SDL window, stdout UART, stub everything else
- C build: `make hw`, `make sim` produce both binaries
- Go CLI scaffold: `cli/` module with `core build`, `core sim`,
  `core flash` (file copy to mounted iPod) working
- `core-sim` binary embedded into the Go binary via `go:embed`
- ASan-clean sim build, golden-frame harness scaffolded in Go

**Exit criteria:** both firmware targets run the same kernel and reach
the idle loop; sim shows a colored SDL window via `core sim`, hardware
shows a colored LCD via `core flash` + reboot.

### Phase 2 — Drivers (6–8 weeks)

The slog. Each driver clean-room from phase-0 docs, with Rockbox source
as cross-reference.

Order (rough):
1. LCD partial updates + framebuffer
2. Click wheel (input)
3. Clock / boost / idle
4. ATA / IDE (storage)
5. FAT32 (vendor FatFs, write adapter)
6. Audio path: I²S out, DMA, DAC init (no decoding yet — play a sine wave)
7. Charge controller + battery monitor
8. USB MSC (last; hardest)

**Exit criteria:** all drivers exercised by a small test harness. Audio
plays a generated tone. USB MSC mounts on a host. Battery percentage
reads correctly.

### Phase 3 — Filesystem + audio engine (4 weeks)

- FatFs adapter exposed as a clean `file_t` API
- Codec ABI shim
- Vendor and integrate Helix MP3 first; get a hardcoded MP3 file
  playing end-to-end
- DSP: replaygain, crossfade, pitch (defer EQ to phase 5)
- Tagcache: build format, indexer, query API
- Add remaining codecs one at a time

**Exit criteria:** point the firmware at a real iPod-mounted music
library, scan the tagcache, play a track via a hardcoded path.

### Phase 4 — UI integration + installer/updater (3 weeks)

Cabinet's plugin source already implements the UI logic. Port effort:
- Replace `rb->` plugin-API calls with our equivalents (button, LCD,
  tagcache, audio_play, file)
- Replace the Linen `.wps`/`.sbs` chrome with a code-driven renderer
  in `apps/ui/render/`
- Replace `.fnt` bitmap fonts with FreeType + cached glyph atlases
  (pre-rasterized at build time for shipping sizes)
- Wire atomic write/rename for tagcache + settings
- Implement update mode in firmware (USB MSC + "safe to update" screen)

Go CLI (round out from phase 1 scaffold to ship-ready):
- `core install` — bootloader install (port ipodpatcher logic to Go),
  firmware + assets write, elevation prompts per platform
- `core update` — fetch from GitHub Releases, verify signature, atomic
  apply with rollback fallback
- `core recover` — restore factory bootloader / reflash
- `core info`, `core debug`, `core release` — round out the surface
- Cross-platform CI matrix (Linux/macOS/Windows × amd64/arm64) producing
  signed release binaries

**Exit criteria:** Cabinet runs as the shell with all flows working
(Music chain, Now Playing, settings). A user with no prior tooling can
download `core` for their platform, run `core install`, end up with our
firmware on their iPod; run `core update` later to upgrade; run `core
recover` to restore factory if needed.

### Phase 5 — Polish & power (ongoing)

- Power tuning: idle clock floor, HDD spin-down timer, LCD dim/off
- Battery curve refinement (empirical, multiple cells)
- Settings UI (curated subset: ~10 knobs)
- USB MSC robustness across hosts
- Crash handler: panic screen with context, written to disk for post-mortem
- Album-art LRU cache
- Tagcache delta scan on USB eject

## Open decisions

These need answers before phase 2 starts; phase 0 doesn't depend on them.

1. **Bootloader:** repurpose Rockbox's (default) or fork it?
2. **Build system:** Meson or plain Make? (Lean Meson; trivial to switch.)
3. **Rust opt-in:** allow Rust in new code from phase 2, or stay pure C?
   (Lean: pure C through phase 4, revisit for phase 5.)
4. **Settings storage:** flat key=value text file (Rockbox style) or
   a small binary blob? (Lean: text — trivial, debuggable, future-proof.)
5. **Project name:** `core` is a placeholder. Want something with
   more character.
6. **Curated settings list:** which ~10 knobs survive? Needs a pass.

## Risks

- **USB stack complexity** — historically the hardest single piece on
  any embedded project. Mitigation: defer to last in phase 2, have a
  fallback plan (ship without MSC initially; users sync via SD-adapter
  swap until USB lands).
- **Battery curve drift across cell ages** — what works on a 2008 cell
  may misread a 2024 replacement. Mitigation: empirical calibration
  table, leave room for user override.
- **Bricking during early flash cycles** — mitigated by sticking with
  the existing Rockbox bootloader and documenting the diagnostic-mode
  recovery path prominently.
- **Codec licensing audit** — RPSL is GPL-compatible but unfamiliar; we
  should review before phase 3 to confirm distribution terms match what
  we want.
- **Time budget** — 4–6 months of evenings is the realistic estimate;
  could double if any of {USB, ATA DMA, charge controller} hits an
  undocumented hardware quirk. Phase 0 derisks most of this by
  surfacing unknowns early.

## Effort summary

| Phase | Duration (evenings) | Deliverable |
|-------|---------------------|-------------|
| 0     | 3–4 weeks           | Hardware reference doc |
| 1     | 3 weeks             | Bootable skeleton + sim parity + Go CLI scaffold |
| 2     | 6–8 weeks           | All drivers working (hw + sim stubs) |
| 3     | 4 weeks             | Audio playback end-to-end |
| 4     | 3 weeks             | Cabinet shell + Go installer/updater (cross-platform, signed) |
| 5     | ongoing             | Polish, power, settings, battery regression |
| **Total to v1.0** | **~4–5 months** | Daily-driver firmware |

Phase 0 is the right starting point regardless: it's a self-contained
artifact, it's the cheapest decision-quality information we can buy,
and it makes every subsequent phase faster.
