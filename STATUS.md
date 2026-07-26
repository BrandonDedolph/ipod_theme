# Status — picking up where we left off

The README is the canonical public story; this doc is the running list of
what works, what doesn't, and what to pick up next.

## Where we are right now (2026-07-22)

**A full music player on real hardware.** The whole bare-metal stack is
proven end to end on an actual iPod 5.5G 80GB — boot + MMAP0 remap →
clock/PLL → timer/IRQ → LCD (BCM present) → I²C/WM8758/I²S → DMA →
ATA PIO → FAT32 — and on top of it a real player: **streaming FLAC
off the iPod's own disk** (read-ahead ring, not preload, so full-length
tracks play), a host-built library index (`CORELIB.IDX`) for instant
Songs / Albums / Artists / Genres, and the full Linen/Onyx UI —
album-art chips, 120×120 now-playing cover, scrolling marquee, settings,
volume/lock overlays, battery gauge. On-screen framebuffer console is the
cable-free debug channel; there is NO serial cable (confirm hw state on
screen instead).

**Dev-environment note:** the clean flash environment is **native Linux**
(the iPod is a real `/dev/sdX`, so `ipodpatcher` writes the firmware
partition and data copies persist). The WSL path used in recent sessions
copies over Windows interop (`/mnt` writes don't persist — must go
Windows-native + `Write-VolumeCache`), and the device drops out of disk
mode frequently, so flashing retries until `D:` reappears. Toolchain on
Arch is all official `extra`: `pacman -S arm-none-eabi-gcc
arm-none-eabi-binutils arm-none-eabi-newlib meson ninja pkgconf`, then
`make hw` / `make ipod` / `make sim` from `core/`. The bootloader
(ipodloader2) stays installed and chainloads our `.ipod`.

## What works on the device today

- **Bring-up** — cold boot + MMAP0 SDRAM remap, clock/PLL (30 MHz, refcounted
  80 MHz boost), IRQ + 100 Hz timer, unified-cache management.
- **Display** — BCM framebuffer present path; on-screen console for cable-free
  debug; the full RGB565 UI.
- **Audio** — WM8758B bring-up over I²C, I²S transport, DMA-driven continuous
  playback fed by an SPSC PCM ring drained by the DMA-completion ISR.
- **Storage** — PIO ATA reader (aligned bulk reads straight into the caller
  buffer) + from-scratch read-only FAT32 (long names decoded to UTF-8).
- **Streaming decode** — `dr_flac` freestanding, fed by a read-ahead disk
  source; a full-length track streams off the disk while the UI stays live.
  **FLAC only.** `dr_mp3` is built and linked and passes its host KAT, but MP3
  is switched off on the device (`CORE_ENABLE_MP3 0`, `core/kernel/main.c`) and
  `classify_ext()` does not surface `.mp3` at all, so those files are invisible
  in the browser. Its float synthesis filter cannot hit real time on this
  FPU-less CPU — the PCM ring starves and playback stutters. Parked, not
  removed; re-enabling needs a fixed-point or second-core decoder.
- **Library** — host-built `CORELIB.IDX` loads in one read → instant Songs /
  Albums / Artists / Genres; records carry UTF-8 fields + a normalized-name hash
  that binds each to its file independent of quote/case style (falls back to a
  per-file tag scan if the index is absent).
- **Power management** — a whole shipped layer that this list omitted entirely.
  Five commits, all verified on the device: CPU scaled to 30 MHz and halted at
  idle; the HDD spun down at idle, including while paused; the LCD panel put to
  sleep at backlight-off idle; the codec powered down and the audio clocks gated
  at stop; and the lock/unlock plate repainted on every Hold edge. A two-tier
  power button with suspend-to-RAM sits on top (see `hal/hw/power.c`).
  *(Written from the commit log — the behaviour of each is documented in the
  commits and the drivers themselves, which are the place to check details.)*
- **Full UTF-8 names** — atlas covers Latin-1 + smart punctuation, the text
  renderer decodes UTF-8, and display sources tag text so FAT-illegal characters
  (`?,*,:,/`) show correctly.
- **Browsing UI** — main menu, Music submenu, Artists / Albums / Songs / Genres,
  album detail (hero art + tracklist with per-disc sections + durations), with a
  scrolling marquee for long titles and taller two-line rows carrying 28px
  album-art chips.
- **Now Playing** — 120×120 cover, title/artist/album, TRACK N OF M, elapsed /
  −remaining, a rounded progress bar, shuffle/repeat tokens, battery.
- **Overlays** — volume (skinny-wave speaker icon + fill bar + %), lock/unlock
  padlock modals, charging screen, boot splash. Anti-aliased modal/progress
  corners.
- **Settings** — Playback (shuffle / repeat), Sound (volume / bass / treble /
  balance via the WM8758 EQ), Theme (Linen ↔ Onyx live palette swap), Display
  (backlight timeout), Clicker (7 piezo click profiles), About (dashboard:
  song/album/artist counts, storage, battery), Reset.
- **Battery gauge** — proportional fill, turns red at ≤20%.

## What's NOT done (pick up next)

1. **Settings persistence** — designed
   (`core/docs/design/settings-persistence.md`), not built. The firmware's FAT
   is read-only, so persisting settings needs an ATA/FAT write path. Highest-value
   next feature.
2. **Playlists / Podcasts / Audiobooks / Composers** — greyed placeholders in the
   menus; no backing implementation yet.
3. **More codecs** — AAC / ALAC / Vorbis / Opus / WAV are stubbed in
   `codecs/README.md`; only FLAC + MP3 are wired.
4. **Search** — not implemented.
5. **On-device screenshot capture** — the README shots are faithful renders
   (`docs/screens/render.py`); the SDL sim's ATA is a stub, so it can't load a
   library to capture real screens. A sim disk backing (or a device capture path)
   would enable true screenshots.
6. **Library sync is manual** — build the index on the host (`tools/build_index.py`),
   convert art (`tools/coreart.py`), and copy to the device.

## Testing

`meson test -C build-sim` from `core/` (**31/31** green):

- **Codec KAT** — FLAC + MP3 decoders bit-exact against reference PCM.
- **MMIO golden traces** — each freestanding hw driver is host-compiled against a
  recording mock bus (`-DMMIO_MOCK`) and asserted to emit its exact ordered
  register grammar (I²C, WM8758 bring-up, I²S, DMA, LCD, UART, clock, timer,
  battery, volume, backlight, and the PMU standby write).
- **Audio ping-pong** — the buffer state machine in `hal/hw/audio.c`: buffers
  alternate, no audio is repeated or dropped across DMA completions, a short
  source read zero-pads exactly the tail.
- **Player queue** — `core/player/player.c`'s queue, auto-advance, repeat,
  shuffle, next/prev, pause/resume and the incremental queue builder, with the
  real player compiled against fake disk/codec/DAC.
- **Library locator hash** — golden vectors asserted from BOTH the C side and
  `tools/build_index.py`, plus a diff proving the testable copy still matches
  `kernel/main.c`. The hash is the only thing binding an index record to its
  file; if the two implementations drift the track silently stops resolving.
- **FAT32** — the happy path, plus corrupt images: cyclic FAT chains,
  out-of-range clusters, a FAT16 boot sector, orphaned LFN runs, a truncated
  volume.
- **readahead / diskbuf / scheduler / console / settings / clickwheel / pcm-ring
  / text / thumb** — unit tests.

Some of those assert the *documented* contract rather than current behaviour and
are marked as expected failures where the fix belongs to another file. Run
`CORE_TEST_STRICT_XFAIL=1 meson test -C build-sim` to turn every such marker
into a hard failure and see what is still outstanding.

**Read this before trusting a green suite.** For a long stretch this section
said "25/25 green" and that number came from *stale gcc-14 binaries* — `meson
test --no-rebuild` happily runs whatever is already in the build directory. A
real rebuild failed: gcc had rolled 14 → 16 underneath the tree, gcc 15 had
added `-Wunterminated-string-initialization`, and the `-Werror` hw build no
longer compiled. `.github/workflows/ci.yml` now builds everything from scratch
on every push for exactly this reason, on both a pinned toolchain and the
current rolling one, and runs the suite again under ASan + UBSan.

Also gated in CI:

- `make verify-hw` — linker/crt0 layout asserts and the header↔doc address
  consistency check.
- **Size budget** — `tests/scripts/check_size.sh` runs after every link, prints
  text/data/bss, and fails past the documented budget. Current image: ~225 KB
  text, ~204 B data, ~10.1 MB bss inside a 32 MB SDRAM window.

The device build (`make hw` / `make ipod`) is the authoritative compile for the
firmware; the clang lints about `hw/pp5022.h` / `LCD_WIDTH` / `mmio_*` are
include-path noise from editor tooling, not build errors.

## Repository

GitHub: https://github.com/BrandonDedolph/ipod-core.

**`main` is NOT necessarily current.** This line used to claim it was; work
lands on local branches first and several commits have sat unpushed at a time,
so treat the remote as a floor, not the truth. Check before assuming:

```bash
git log --oneline origin/main..HEAD    # commits you have that the remote doesn't
git status -sb                          # ahead/behind for the current branch
```

See the README for the overview, `core/README.md` for the firmware build, and
`PLAN.md` for the roadmap.
