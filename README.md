# core — a modern music player for the iPod 5.5G

`core` is a **from-scratch, Apache-2.0 firmware** for the 5th-generation
iPod ("iPod Video", PortalPlayer PP5022). It boots on real hardware,
reads music off the iPod's own disk, and plays it back through a custom
Nunito/Linen interface.

It's **a different take on the iPod, not a replacement for another
firmware.** Where [Rockbox](https://www.rockbox.org/) is the do-everything
option, `core` aims at one thing: a clean, modern player experience on the
original hardware. It's independent firmware in its own right — not a
Rockbox theme, patch, or plugin, and it contains no copied Rockbox code.

> ### ▶ This runs on a real iPod. Not an emulator, not a simulator.
> It boots and plays on **actual 2006 Apple hardware** — a physical iPod
> 5.5G. Written from scratch in C and ARM assembly, flashed to the device,
> coming up from a cold start and streaming music straight off the iPod's
> own hard drive. Bare metal: no OS, no libc, nothing between this code and
> the silicon. *(The screenshots and GIFs below are host-rendered
> reproductions of the on-device UI — same layout, palette and Nunito
> faces, drawn by `docs/screens/render.py`. They are not photographs and
> not framebuffer captures; the panel is hard to photograph cleanly and
> the firmware has no screenshot path.)*

**And it's the firmware, not a payload.** There is no chainloader and no
boot menu. `core` is written into the iPod's firmware partition as the
OSOS image, so Apple's boot ROM hands control straight to our `crt0.S`,
which does the SDRAM remap to `0x00000000` itself. Installing replaces the
whole OSOS — Apple's firmware is no longer on the device. The way back is
unchanged and unconditional: the boot ROM's **Select + Play** disk mode
runs before any image loads, so nothing we flash can pre-empt it.

The whole bare-metal stack is proven end-to-end on an actual iPod 5.5G:
boot ROM → `crt0.S` + memory remap → clock/PLL → timer/IRQ → LCD (BCM
framebuffer present) → I²C/WM8758B/I²S first sound → DMA playback → ATA PIO
reader → FAT32 → streaming FLAC decode → audio out the headphone jack —
and, going the other way, the first bytes this firmware has ever *written*
back to the user's disk.

<p align="center">
  <img src="docs/screens/demo.gif" alt="core UI in motion — main menu to Now Playing" width="420">
  <br><em>Cold boot → browse → play, on the real device.</em>
</p>

See the [**Screens**](#screens) gallery below for a full tour.

---

## What it is

- **Cleanroom-by-facts.** Every driver is written from hardware facts —
  PortalPlayer register maps, the WM8758B datasheet, the FAT32 spec. Where
  those facts were cross-referenced against existing sources (including
  Rockbox), only non-copyrightable facts were taken — register addresses,
  bit values, init sequences — never code bodies; the implementations are
  our own. See [`core/docs/hw/`](core/docs/hw/) for the subsystem-by-
  subsystem hardware reference the drivers were written against, which
  cites its sources.
- **Bare metal.** No RTOS, no libc on the device. A small cooperative
  kernel, a static-arena allocator, and our own `mem.c` back the
  freestanding decoders; integer division and soft-float come from the
  compiler runtime (`libgcc`), never libc.
- **Real audio.** `dr_flac` is compiled freestanding
  (`-DCORE_FREESTANDING`) and fed by a read-ahead disk source on top of an
  8 MB anti-skip buffer, into an SPSC PCM ring drained by the DMA-completion
  ISR. Streaming, not preload — a full-length track plays off the disk.
  Track hand-over is gapless: at end-of-stream the next track is opened over
  the same arena, buffer and ring, and when two tracks share a sample rate
  the DAC is never stopped at all *(implemented and unit-tested; not yet
  confirmed by ear on the device)*. **The device is FLAC-only today:**
  `dr_mp3` is built and linked, but MP3 is disabled
  (`CORE_ENABLE_MP3 0` in `core/kernel/main.c`) and `.mp3` files are
  hidden from the browser entirely — its float synthesis filter can't
  hit real time on this FPU-less CPU, so the ring starves and playback
  stutters. Re-enabling it needs a fixed-point or second-core decoder.
- **Real type.** `core/ui/text.c` is a libc-free, gamma-correct
  antialiased text renderer that draws pre-rasterized Nunito glyph
  atlases straight into the RGB565 framebuffer — no FreeType, no malloc,
  all `.rodata`. It decodes UTF-8 and covers Latin-1 + smart punctuation,
  so accented names and curly quotes render true. The pen is 26.6 fixed
  point and carries the fractional advance across a whole string; each of
  the six faces ships its own kerning table (1,700–3,700 pairs) and its own
  tracking value, solved from measured ink-to-ink spacing rather than
  guessed.
- **A real library UI.** Browse by Artists / Albums / Songs / Genres —
  plus an artist's whole discography as one **All Songs** list — off a
  host-built index (`CORELIB.IDX`) that loads in one read and holds up to
  6000 songs / 1024 albums / 512 artists / 128 genres in full UTF-8. Album-
  art chips, a 120×120 now-playing cover, a scrolling marquee for long
  titles, a warm-light **Linen** theme and a warm-dark **Onyx** one, plus
  settings (tone/balance, backlight, click profiles), volume and lock
  overlays, and a battery gauge that warns red when low.
- **It remembers.** Settings persist across reboots to a pre-allocated
  `CORECFG.DAT` — two alternating slots, each a whole physical sector, each
  CRC-32 checked, written through the ATA write path with the target LBA
  re-resolved and re-validated before every write. The device never creates,
  grows, moves or deletes the file, so no FAT metadata is ever touched. On
  top of that, **Resume** brings you back on the track you left, *paused*,
  at the saved position — bound by a folded name hash (confirmed by
  duration where possible), so it survives a library rebuild.

## Screens

A tour of what's on the device. *(Host-rendered reproductions, not device
captures — see the note at the top.)*

### Browse your whole library

Main menu → Music → browse by **Artist / Album / Song / Genre**, all off a
host-built index that loads in one read. Two-line rows carry album-art
chips; long titles scroll a marquee. An artist's **All Songs** row collapses
their whole discography into one list, with the album on the sub-line.

<p align="center"><img src="docs/screens/browse.gif" alt="browsing the library" width="360"></p>

<table>
  <tr>
    <td><img src="docs/screens/mainmenu.png" width="260" alt="Main menu"></td>
    <td><img src="docs/screens/music.png" width="260" alt="Music menu"></td>
    <td><img src="docs/screens/artists.png" width="260" alt="Artists"></td>
  </tr>
  <tr>
    <td><img src="docs/screens/albums.png" width="260" alt="Albums"></td>
    <td><img src="docs/screens/songs.png" width="260" alt="Songs"></td>
    <td><img src="docs/screens/genres.png" width="260" alt="Genres"></td>
  </tr>
  <tr>
    <td><img src="docs/screens/allsongs.png" width="260" alt="An artist's All Songs"></td>
    <td></td>
    <td></td>
  </tr>
</table>

### Now Playing

A 120×120 cover, marquee title, artist/album, `TRACK N OF M`, elapsed /
−remaining, and a rounded progress bar. The volume overlay's speaker icon
grows its sound waves as you turn it up.

<p align="center"><img src="docs/screens/volume.gif" alt="volume overlay with growing sound waves" width="360"></p>

<table>
  <tr>
    <td><img src="docs/screens/nowplaying.png" width="260" alt="Now Playing"></td>
    <td><img src="docs/screens/detail.png" width="260" alt="Album detail"></td>
    <td><img src="docs/screens/volume.png" width="260" alt="Volume overlay"></td>
  </tr>
</table>

### Two themes — Linen &amp; Onyx

The same UI in a warm-light and a warm-dark palette, swapped live from
Settings.

<p align="center"><img src="docs/screens/themes.gif" alt="Linen and Onyx themes" width="360"></p>

<table>
  <tr>
    <td><img src="docs/screens/nowplaying.png" width="260" alt="Now Playing — Linen"></td>
    <td><img src="docs/screens/nowplaying_onyx.png" width="260" alt="Now Playing — Onyx"></td>
  </tr>
  <tr>
    <td><img src="docs/screens/albums.png" width="260" alt="Albums — Linen"></td>
    <td><img src="docs/screens/albums_onyx.png" width="260" alt="Albums — Onyx"></td>
  </tr>
</table>

### Settings

Nine rows, and they stick: Playback (shuffle / repeat / resume), Sound
(volume / bass / treble / balance via the WM8758B EQ), a theme picker,
Display (backlight timeout + brightness), **seven** piezo click profiles, an
About dashboard, **Boot Details**, Disk Mode, and Reset. Everything but the
diagnostics is saved to disk and comes back after a reboot.

<p align="center"><img src="docs/screens/settings.gif" alt="adjusting a Sound slider" width="360"></p>

<table>
  <tr>
    <td><img src="docs/screens/settings.png" width="260" alt="Settings"></td>
    <td><img src="docs/screens/sound.png" width="260" alt="Sound"></td>
    <td><img src="docs/screens/clicker.png" width="260" alt="Clicker profiles"></td>
  </tr>
  <tr>
    <td><img src="docs/screens/theme.png" width="260" alt="Theme picker"></td>
    <td><img src="docs/screens/about.png" width="260" alt="About"></td>
    <td><img src="docs/screens/bootdetails.png" width="260" alt="Boot Details"></td>
  </tr>
</table>

**Boot Details** is the diagnostics page: a live phase breakdown of the last
cold boot — LCD/BCM bring-up, disk + mount, library load, resume, and the
unattributed remainder — as a stacked proportional bar plus a legend, with
the FLAC decode cost against the 44.1 kHz real-time budget, the underrun
count, and the settings file's slot LBAs. The remainder is *derived* (total
minus the named phases) and the total is measured independently rather than
summed, so unmeasured time shows up instead of vanishing.

### System

Boot splash, charging screen, and the Hold-switch lock / unlock overlays.

<p align="center"><img src="docs/screens/lock.gif" alt="lock and unlock overlays" width="360"></p>

<table>
  <tr>
    <td><img src="docs/screens/boot.png" width="260" alt="Boot splash"></td>
    <td><img src="docs/screens/charging.png" width="260" alt="Charging"></td>
    <td><img src="docs/screens/lock.png" width="260" alt="Unlocked"></td>
    <td><img src="docs/screens/locked.png" width="260" alt="Locked"></td>
  </tr>
</table>

## Hardware target

| | |
|---|---|
| Device | iPod 5.5G (Video), 80 GB |
| SoC | PortalPlayer PP5022 (dual ARM7TDMI, ARMv4T) |
| Audio DAC | Wolfson WM8758B over I²C control + I²S data |
| Display | 320×240 LCD driven through the BCM framebuffer path |
| Storage | ATA disk (PIO); read-only FAT32 reader + an in-place write to one pre-allocated file |
| Input | Apple click-wheel + buttons + hold switch (polled) |
| Boot | Direct — our image *is* the OSOS in the firmware partition; no chainloader, no boot menu |
| Recovery | Boot ROM's Select + Play disk mode, which runs before any image loads |

## Performance — real-time on a 2006 SoC

The PP5022 is a pair of ~80 MHz ARM7TDMI cores with **no FPU, no hardware
divide**, a small unified cache, and a **PIO** disk (no DMA to the drive,
~170 KB/s). Decoding FLAC in real time *and* driving a smooth, animated,
antialiased UI on that budget took deliberate work — the interesting part
of the project is how little the hardware gives you.

- **Clock + cache first.** Enabling the PP5022 unified cache and holding an
  80 MHz boost across the whole open/decode path is the line between
  stuttering and real-time FLAC.
- **Measure the boot, don't guess at it.** "Boot takes ten seconds" had
  nowhere to aim, so the boot path got instrumented per phase and the
  numbers got their own screen. A cold boot measured **8.4 s**, of which
  **5.1 s was a single FLAC seek**; removing that should leave roughly 3 s,
  but the post-fix total has deliberately not been written down here because
  nobody has yet read it off the device. Boot Details reports it live, on
  every boot rather than on the one day someone times it — which is the
  point of the screen.
- **A `#define` that cost five seconds.** `DR_FLAC_NO_CRC` was set purely to
  save per-frame decode cost — but `dr_flac` guards its *binary-search seek*
  behind that same flag, because landing on an arbitrary byte means proving
  a candidate frame header is real rather than audio that happens to look
  like a sync code, and the CRC is the proof. These files also carry no
  SEEKTABLE, so with both paths gone every seek fell through to decoding
  from the start of the track: 5.1 s of that 8.4 s boot was one seek.
  Seeking is O(log n) now, at a cost of ~14 KB of text and some per-frame
  decode margin — which is exactly why Boot Details reports the decode
  margin instead of assuming it.
- **No divides in the hot path.** The gamma-correct text blend runs entirely
  in integers off pre-baked sRGB↔linear LUTs (never touches `<math.h>`), and
  the per-pixel alpha composite replaces three soft-divides with an exact
  `floor(x/255)` add-shift — the divide-less ARM7 never pays for a divide
  while painting glyphs.
- **Draw only what changed.** The marquee scrolls through a tiny partial
  present (just the title band), not a full-frame blit, and clips per pixel to
  its row — so continuous animation costs almost nothing.
- **Instant library.** The song database is built on the host into a single
  index the firmware loads in *one read*; Songs / Albums / Genres open with no
  per-file tag scan at boot, and per-genre counts are precomputed. Records bind
  to files by a hash, not a directory-walking string compare.
- **Streaming without skips.** A read-ahead disk buffer does bursty reads so
  the drive head parks between them (anti-skip), feeding a lock-free SPSC PCM
  ring drained by the DMA-completion ISR — audio never waits on the UI. Bulk
  ATA reads land straight in the caller's buffer, with a one-sector bounce only
  for unaligned tails.
- **Album art that never stalls audio.** Covers are pre-converted on the host
  to raw RGB565 sidecars (no on-device JPEG decode); the list-chip cache loads
  at most one thumbnail per main-loop pass so scrolling can't starve the audio
  DMA, and the 28 px chip is an exact-size file — a 1:1 copy, no resample.
- **No allocator in the render path.** The Nunito glyph atlases are `const`
  `.rodata` resolved at link time — no FreeType, no malloc, no init step.
- **Idle costs something, so spend less of it.** At idle the CPU drops to
  30 MHz and halts, and the drive spins down after 20 s — including while
  paused. At stop, the codec is powered down and the audio clocks are gated.

## Status

Working on real hardware today: direct boot as the OSOS image, LCD present,
click-wheel input, backlight, WM8758B sound, DMA continuous playback,
ATA + FAT32 read, **streaming FLAC playback off the iPod's own disk**,
**settings that persist to disk**, and **resume-on-boot**. The menu UI,
browser and Now Playing screens render on device via the freestanding text
renderer. There is no serial cable in the loop — on-device state is
confirmed through an on-screen framebuffer console and the Boot Details page.

Not there yet, and honestly labelled: **Playlists** — the M3U8 reader is
merged and unit-tested but wired to nothing, and writing playlists needs
FAT32 cluster allocation, which doesn't exist. **Search**, **Podcasts /
Audiobooks / Composers**, and codecs beyond FLAC are all unimplemented.
Panel sleep at idle is written but switched off (it wedged the LCD white).
Library sync is manual: build the index and convert art on the host, then
copy.

Not yet verified on the device: gapless hand-over, the 500 mA charge-current
change (needs an inline USB current meter), and seek performance outside the
boot path.

See [`STATUS.md`](STATUS.md) for the running list of what works, what's
pending, and what to pick up next, and [`PLAN.md`](PLAN.md) for the phased
roadmap.

---

## Building

Requires `meson`, `ninja`, `pkg-config`, a C11 host compiler, and (for
the device build) `arm-none-eabi-gcc` with binutils + newlib. `libsdl2-dev`
backs the host HAL build. On Arch: `pacman -S arm-none-eabi-gcc
arm-none-eabi-binutils arm-none-eabi-newlib meson ninja pkgconf sdl2`.

```bash
cd core

# Device firmware — ARMv4T bare-metal ELF + flat binary
make hw            # → build-hw/core.elf, build-hw/core.bin
make ipod          # → build-hw/core.ipod (transport-wrapped image)

# Host build + unit tests (freestanding drivers/codecs, MMIO golden traces)
make sim           # configures + builds the host TEST target — see below
meson test -C build-sim              # 36 tests

# Static checks against the linked ARM image: crt0/linker layout, the
# header↔docs address consistency check, name-hash parity across its three
# implementations, resume-matcher parity, and the size budget.
make verify-hw

# The same tests under AddressSanitizer + UndefinedBehaviorSanitizer. The
# FAT32 reader and the codec container parsers consume whatever is on a
# user's disk, so this is the build that matters for them.
meson setup build-asan -Dtarget=sim \
    -Db_sanitize=address,undefined -Doptimization=0 -Db_lto=false
meson test -C build-asan
```

> **`make sim` builds the tests, not a simulator.** The name is aspirational:
> the sim target compiles `codecs/` and `tests/` only. `core/hal/sim/sim_hal.c`
> exists and builds into a static library, but **no executable links it** —
> there is no runnable SDL2 emulator you can point at a music folder. What
> `make sim` gives you is the host test suite: the same freestanding driver,
> codec and text-renderer sources the device links, exercised against a
> recording mock MMIO bus. That is genuinely useful — it is the only automated
> check of the hardware register grammar — but it is not a simulator, and the
> device remains the only place the UI can be seen.

The host (`sim`) target compiles the same freestanding driver, codec, and
text-renderer sources the device links, plus the MMIO golden-trace tests
that assert each hardware driver's exact register grammar against a
recording mock bus — the automated safety net for code that otherwise
needs a logic analyzer to verify. CI builds every job **from nothing** on
every push (pinned and rolling ARM toolchains, plus the suite under
ASan + UBSan), because a stale build directory once reported a green suite
from month-old objects.

Two host tools exist purely for the type work, and both are worth knowing
about because they are the reason the spacing numbers are measured rather
than eyeballed:

```bash
# Render the REAL firmware text stack on the host — this links core/ui/text.c
# and the shipped atlases unmodified, so what it draws is what the panel draws.
cc -Icore/ui -o /tmp/text_preview tools/text_preview.c core/ui/text.c
/tmp/text_preview out.ppm 4

# Measure glyph spacing objectively out of the baked atlases, using the
# device's own pen arithmetic (tracking + kerning + 26.6 advance), reading
# ink-to-ink daylight from the alpha bitmaps rather than from bboxes.
tools/.venv/bin/python3 tools/text_metrics.py --worst 20
```

## Flashing

`core` is installed **over** Apple's firmware, as the OSOS image in the
iPod's firmware partition. There is no bootloader to install and nothing to
copy to the music partition — a `core.ipod` sitting on the FAT32 data
partition does nothing at all, because nothing loads it.

```bash
make ipod
# Put the iPod in disk mode, then, with raw block-device access:
ipodpatcher <disk> -wf build-hw/core.ipod     # write our OSOS image

# Always verify the write before booting it:
ipodpatcher <disk> -rfb readback.bin
cmp readback.bin build-hw/core.bin            # byte-identical, or don't boot
```

Back up the firmware partition first (`ipodpatcher <disk> -r
bootpartition-backup.bin`); restoring it (`-w`) puts Apple's firmware back.

**Recovery, if a build doesn't boot:** hold **Select + Play** at power-on to
reach Apple's disk mode. This lives in the boot ROM and runs before any
image is loaded, so it works with a black screen, a bad OSOS, or no
bootloader at all — it has been exercised on this device in exactly that
state. Then reflash, or restore the backup. The firmware also offers a
Disk Mode entry under Settings for convenience, but the ROM combo is the
floor and nothing we ship can remove it.

---

## Repo layout

```
core/                     bare-metal firmware + host test build
├── boot/                 crt0 (SDRAM remap, COP wake) + linker script
├── kernel/               cooperative scheduler, IRQ, timer, clock, PCM ring,
│                         panic/fault handlers, settings persistence, and the
│                         player UI (main.c)
├── hal/
│   ├── hal.h             hardware contract
│   ├── hw/               ARM drivers — LCD, ATA, I²C, I²S, WM8758B, DMA,
│   │                     click-wheel, backlight, battery, power, piezo, UART
│   └── sim/              host HAL backend (SDL2)
├── fs/                   from-scratch read-only FAT32 reader (LFN → UTF-8)
│                         + an M3U8 playlist reader (parse-only, not yet wired)
├── lib/                  freestanding mem.c (memcpy/memset)
├── codecs/               dr_flac + dr_mp3 (freestanding), static arena,
│                         read-ahead disk source, FLAC metadata reader
├── ui/                   AA text renderer + Nunito atlases, palette, art cache,
│                         settings model, per-screen renderers
├── player/               playback engine — queue, transport, gapless hand-over
├── cli/                  Go host CLI — `.ipod` image pack/unpack (and the
│                         image header format); install/flash are stubs
├── docs/hw/              hardware reference the drivers were written against
├── docs/design/          design notes (settings persistence)
├── cross/                Meson cross file (arm-none-eabi)
└── tests/                host unit + MMIO golden-trace tests, static-check scripts

design_reference/         UI design source — palette, chrome, icon paths
docs/screens/             interface screenshots + demo GIFs (this README)
tools/                    host tooling — atlas + glyphmap generator, album-art
                          converter, library-index builder, CORECFG.DAT
                          creator, text preview + spacing metrics, font sources
```

See [`core/README.md`](core/README.md) for firmware-side build detail and
[`tools/README.md`](tools/README.md) for the host toolchain.

---

## License

Apache-2.0, first-party. The firmware contains no copied Rockbox code and
vendors no GPL code: its drivers are written from hardware facts (register
maps, datasheets, the FAT32 spec), and where those facts were
cross-referenced against existing sources — including Rockbox — only the
facts themselves were taken (register addresses, bit values, init
sequences), not code bodies. Vendored decoders are permissively licensed
and unrelated to Rockbox — `dr_flac` / `dr_mp3` (public domain / MIT-0).
The Nunito font is under the SIL Open Font License 1.1 (`tools/fonts-src/`).
