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
> the silicon. *(The screenshots and GIF below are faithful renders of the
> on-device UI — the panel is hard to photograph cleanly.)*

The whole bare-metal stack is proven end-to-end on an actual iPod 5.5G:
boot + memory remap → clock/PLL → timer/IRQ → LCD (BCM framebuffer
present) → I²C/WM8758B/I²S first sound → DMA playback → ATA PIO reader →
FAT32 → streaming FLAC decode → audio out the headphone jack.

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
  (`-DCORE_FREESTANDING`) and fed by a read-ahead disk source into an
  SPSC PCM ring drained by the DMA-completion ISR. Streaming, not
  preload — a full-length track plays off the disk. **The device is
  FLAC-only today:** `dr_mp3` is built and linked, but MP3 is disabled
  (`CORE_ENABLE_MP3 0` in `core/kernel/main.c`) and `.mp3` files are
  hidden from the browser entirely — its float synthesis filter can't
  hit real time on this FPU-less CPU, so the ring starves and playback
  stutters. Re-enabling it needs a fixed-point or second-core decoder.
- **Real type.** `core/ui/text.c` is a libc-free, gamma-correct
  antialiased text renderer that draws pre-rasterized Nunito glyph
  atlases straight into the RGB565 framebuffer — no FreeType, no malloc,
  all `.rodata`. It decodes UTF-8 and covers Latin-1 + smart punctuation,
  so accented names and curly quotes render true.
- **A real library UI.** Browse by Album / Artist / Genre / Song off a
  host-built index (`CORELIB.IDX`) that loads in one read — album-art
  chips, a 120×120 now-playing cover, a scrolling marquee for long
  titles, a warm-light **Linen** theme and a warm-dark **Onyx** one,
  plus settings (tone/balance, backlight, click profiles), volume and
  lock overlays, and a battery gauge that warns red when low.

## Screens

A tour of what's on the device. *(Faithful renders of the on-device UI.)*

### Browse your whole library

Main menu → Music → browse by **Artist / Album / Song / Genre**, all off a
host-built index that loads in one read. Two-line rows carry album-art
chips; long titles scroll a marquee.

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

Playback (shuffle / repeat), Sound (volume / bass / treble / balance via
the WM8758B EQ), a theme picker, backlight, **seven** piezo click
profiles, and an About dashboard.

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
    <td></td>
  </tr>
</table>

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
| Storage | ATA disk (PIO), read-only FAT32 reader |
| Input | Apple click-wheel + buttons + hold switch (polled) |
| Chainload | [ipodloader2](https://github.com/crozone/ipodloader2) loads our `.ipod` image |

## Performance — real-time on a 2006 SoC

The PP5022 is a pair of ~80 MHz ARM7TDMI cores with **no FPU, no hardware
divide**, a small unified cache, and a **PIO** disk (no DMA to the drive,
~170 KB/s). Decoding FLAC in real time *and* driving a smooth, animated,
antialiased UI on that budget took deliberate work — the interesting part
of the project is how little the hardware gives you.

- **Clock + cache first.** Enabling the PP5022 unified cache and holding an
  80 MHz boost across the whole open/decode path is the line between
  stuttering and real-time FLAC.
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

## Status

Working on real hardware today: boot and bring-up, LCD present, click-wheel
input, backlight, WM8758B first sound, DMA continuous playback, ATA + FAT32
read, and **streaming FLAC playback off the iPod's own disk**. The
menu UI and Now Playing screens (embedded album art, progress) render on
device via the freestanding text renderer. There is no serial cable in the
loop — on-device state is confirmed through an on-screen framebuffer console.

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
meson test -C build-sim

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
needs a logic analyzer to verify.

## Flashing

`core` is chainloaded, not installed over Apple's firmware. Install
[ipodloader2](https://github.com/crozone/ipodloader2) once, then copy the
built image to the FAT32 data partition and reboot:

```bash
make ipod
cp build-hw/core.ipod /path/to/ipod/          # FAT32 root
# eject, then boot — ipodloader2 chainloads core.ipod
```

---

## Repo layout

```
core/                     bare-metal firmware + host test build
├── boot/                 crt0, image header, linker script
├── kernel/               cooperative scheduler, IRQ, timer, clock, PCM ring
├── hal/
│   ├── hal.h             hardware contract
│   ├── hw/               ARM drivers — LCD, ATA, I²C, I²S, WM8758B, DMA,
│   │                     click-wheel, backlight, UART
│   └── sim/              host HAL backend (SDL2)
├── fs/                   from-scratch read-only FAT32 reader (LFN → UTF-8)
├── lib/                  freestanding mem.c (memcpy/memset)
├── codecs/               dr_flac + dr_mp3 (freestanding), static arena,
│                         read-ahead disk source, FLAC metadata reader
├── ui/                   AA text renderer + Nunito atlases, palette, art cache
├── cli/                  Go host CLI (.ipod firmware pack/unpack)
├── docs/hw/              hardware reference the drivers were written against
├── cross/                Meson cross file (arm-none-eabi)
└── tests/                host unit + MMIO golden-trace tests

design_reference/         UI design source — palette, chrome, icon paths
docs/screens/             interface screenshots + demo GIF (this README)
tools/                    host tooling — atlas + glyphmap generator, album-art
                          converter, library-index builder, font sources
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
