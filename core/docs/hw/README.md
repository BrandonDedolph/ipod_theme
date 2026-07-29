# iPod Video 5G/5.5G — Hardware Reference

A consolidated hardware reference for the iPod Video 5G and 5.5G,
written for someone who needs to drive every register and replicate
every sequence from scratch. Compiled from public hardware
documentation — the PortalPlayer PP502x datasheets, the Wolfson WM8758
datasheet, the FAT32 spec, and the iPodLinux wiki — and cross-referenced
against community projects for **hardware facts only** (register
addresses, bit fields, init sequences), never their source code. This
keeps the reference and the firmware built on it cleanroom and
Apache-2.0.

> **Status:** originally a Phase 0 deliverable; now a live reference that
> the firmware in `core/` has been built and booted against on real
> hardware. It still stands on its own — even if no firmware were ever
> built on top of it, the iPod 5G modding community would have the cleanest
> single reference covering this device — but the register grammars here
> are no longer only *documented*, they are asserted: `check_hw_consistency.py`
> cross-checks `hal/hw/pp5022.h` against these files on every
> `make verify-hw`, and the MMIO golden-trace tests assert each driver's
> exact register sequence.

## How we actually boot (read this first)

Our firmware **is** the OSOS image in the iPod's firmware partition. There
is no chainloader, no ipodloader2, and no boot menu on the device: the Apple
boot ROM loads our image and enters `core/boot/crt0.S` directly. Two
consequences run through several of the docs below:

- **We inherit the boot ROM's state, not a loader's.** SDRAM is still at its
  native `0x10000000` and has *not* been remapped to `0x0` — crt0.S performs
  the MMAP0 remap itself, from an IRAM stub, and everything up to that point
  is position-independent. (01-soc-pp5022.md, 08-boot-dock.md.)
- **Nothing has warmed the display for us.** Passages in 02-lcd.md describing
  the "chainload handoff" (BCM already powered, bootstrapped, idle at frame
  one) document what ipodloader2 *used to* leave behind. The driver no longer
  relies on any of it: it probes, and `bcm_init()` power-cycles a wedged BCM.

Install is `ipodpatcher <n> -wf core.ipod`, verified by reading the partition
back. Recovery is the boot ROM's own disk mode — **hold Select + Play at
power-on** — which runs before any firmware image is loaded and therefore
works no matter how broken the image we wrote is. That unconditional escape
hatch is the load-bearing safety net for everything here: never write the
boot ROM, never disturb the partition signature. (08-boot-dock.md.)

## Table of contents

| File | Subsystem |
|---|---|
| [01-soc-pp5022.md](01-soc-pp5022.md) | PortalPlayer PP5022 SoC: memory map, clocks, dual-core, IRQs, cache |
| [02-lcd.md](02-lcd.md)               | Broadcom BCM video coprocessor + LCD panel (5G / 5.5G) |
| [03-clickwheel.md](03-clickwheel.md) | Touch wheel + buttons + hold switch |
| [04-ata.md](04-ata.md)               | Integrated PIO/UDMA ATA controller, HDD power management |
| [05-audio.md](05-audio.md)           | I²S transport, DMA chain, Wolfson WM8758 DAC |
| [06-power.md](06-power.md)           | PCF50605 PMIC, battery curve, charge detection, sleep |
| [07-usb.md](07-usb.md)               | ARC USBOTG controller, MSC stack, exclusive-storage handoff |
| [08-boot-dock.md](08-boot-dock.md)   | Firmware partition format, bootloader handoff, dock UART, recovery |
| [09-i2c.md](09-i2c.md)               | On-SoC I²C controller (WM8758 codec control bus) |

## Conventions

- Register addresses are in MMIO space at `0x60000000`–`0x700FFFFF`
  unless otherwise noted.
- Bit and byte numbering follows the Rockbox source convention: bit 0
  is the LSB, byte 0 is the lowest-address byte.
- "PP5022" and "PP502x" are used somewhat interchangeably — the iPod
  Video uses the PP5022 specifically, but most of the platform-shared
  code lives in `firmware/target/arm/pp/` and applies to PP5020/5022/5024
  uniformly.
- 5G and 5.5G differ in panel gamma, max storage size, and a few
  GPIO assignments — small enough that Rockbox covers both from one build
  with runtime branches, and small enough that we do too.

## Source map

These files in the Rockbox tree (commit current as of phase-0 research)
are the load-bearing references. Citations in each subsystem doc point
back to specific functions and line ranges.

```
firmware/target/arm/pp/                 Shared PortalPlayer platform code
firmware/target/arm/ipod/               Shared iPod-specific code
firmware/target/arm/ipod/video/         iPod Video 5G/5.5G specific
firmware/export/pp5020.h                PP5020/5022 register definitions
firmware/export/config/ipodvideo.h      Video target compile config
firmware/drivers/ata.c                  Generic ATA driver layer
firmware/drivers/audio/wm8758.c         Wolfson DAC driver
firmware/drivers/pcf50605.c             PMIC driver
firmware/usbstack/                      USB device stack + MSC class
firmware/target/arm/usb-drv-arc.c       ARC USBOTG controller driver
bootloader/ipod.c                       Bootloader entry & dispatch
utils/ipodpatcher/ipodpatcher.c         Firmware-partition installer
```

## What's intentionally out of scope here

- **FireWire** — the 5G/5.5G has a FireWire dock pinout but Rockbox
  doesn't drive it. Not researched.
- **Video encoder / TV out** — the BCM video coprocessor can produce
  PAL/NTSC composite, but we won't use it. Documented at a surface
  level in 02-lcd.md only.
- **Apple firmware ("OF") internals** — covered only where the boot
  ROM's expectations affect us (image format, partition signature). We
  *replace* the Apple OS rather than dual-booting it, so its runtime
  behaviour is not something we need to model.

## Caveats

- Some registers below are documented only in source comments. Where
  the Rockbox source says `/* unknown */`, this doc says so explicitly.
- Magic constants for the BCM video coprocessor's bootstrap (in 02-lcd.md)
  came from iPodLinux reverse-engineering and have never been validated
  against an Apple datasheet. Treat them as "known-working incantations,"
  not "specified values."
- Battery curves (in 06-power.md) are calibrated to the original 2005
  cell chemistry. Users with replacement cells from 2024+ may see
  drift at the bottom 10% of the curve.
