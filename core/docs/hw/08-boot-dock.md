# 08 — Boot, firmware partition, dock connector, recovery

This doc covers the parts of the system the firmware doesn't usually
own at runtime, but absolutely needs to understand at install time:

- The iPod's boot ROM expectations (firmware partition format, image
  header, checksum).
- The state we inherit from the boot ROM when control reaches our
  code.
- The dock connector pinout (specifically the UART, which is our only
  out-of-band debug channel).
- Recovery: how to un-brick when our firmware bricks itself.

## The firmware partition

Partition 0 of the iPod's disk is the firmware partition (Windows-
formatted iPods make it visible as the small "system" partition;
Mac-formatted iPods hide it). The boot ROM reads it on every power-on
and dispatches to a registered image.

### Partition signature

The first 512 bytes contain the Apple copyright banner. The exact
string Rockbox checks for at offset `0x0` is the "STOP" / Apple
copyright preamble (a literal byte sequence verified by ipodpatcher
at `utils/ipodpatcher/ipodpatcher.c` line 1499). The boot ROM **will
not load a partition that doesn't have this preamble** — so any
custom firmware install must preserve it byte-exact.

### Directory header

At offset `0x100`: the marker string `]ih[` (the bytes spell `[hi]`
reversed). Then a 16-bit little-endian directory format version at
offset `0x10A`. Versions 2 and 3 are recognized by the iPod 5G; we
only ever see version 2 or 3 in the wild for this device.

The directory itself begins at offset `(LE32 at 0x104) + 0x200`.

### Image directory entry — 40 bytes

| Offset | Size | Field         | Purpose |
|--------|------|---------------|---------|
| 0      | 4    | Container ID  | `!ATA` or `DNAN` (reverse: 'NAND', 'ATA!') |
| 4      | 4    | Image type    | `soso` (OSOS), `crsr` (RSRC), `dpua` (AUPD), `kbso` (OSBK), `ebih` (HIBE) |
| 8      | 4    | Image ID      | LE u32 |
| 12     | 4    | `devOffset`   | Byte offset within firmware partition (LE) |
| 16     | 4    | `len`         | Image length in bytes (LE) |
| 20     | 4    | `addr`        | DRAM load address (LE) |
| 24     | 4    | `entryOffset` | Entry point within image; 0 = use start |
| 28     | 4    | `chksum`      | 32-bit additive checksum (LE) |
| 32     | 4    | `vers`        | Firmware version variant |
| 36     | 4    | `loadAddr`    | Secondary load address (LE) |

(Source: `utils/ipodpatcher/ipodpatcher.c` 1567–1582.)

The image type tags are stored byte-reversed (so `'soso'` on disk =
`'OSOS'` logically; `'crsr'` = `'RSRC'`; `'dpua'` = `'AUPD'` audio-
update; `'kbso'` = `'OSBK'` backup OS; `'ebih'` = `'HIBE'` Apple
hibernation image).

### Checksum algorithm

```c
u32 chksum = 0;                     // NOT seeded with MODEL_NUM — see below
for (size_t i = 0; i < len; i++)
    chksum += image[i];             // simple additive, 32-bit wrap
```

**Measured on the device, 2026-07-27.** This file previously said the sum
was seeded with `MODEL_NUM` (0x05 for the Video). It is not. Two
independent checks on a real 5.5G firmware partition:

- Apple's own untouched `RSRC` entry: recomputing the plain sum over its
  body gives `0x18319bab`, exactly the `chksum` in its directory entry.
  A `MODEL_NUM` seed would be off by 5.
- Our own OSOS image written by `ipodpatcher -wf`: plain sum
  `0x01589d64`, matching its entry byte-for-byte.

Also corrected: an image's body starts at **`devOffset + 0x800`** within
the firmware partition, not `+0x200`. Located by searching the partition
dump for the first 64 bytes of the image we had just written, then
confirmed by checksumming an image we had never touched at the same
delta.

For `.ipod`-format files (used by ipodpatcher to ship a packaged
image): the 32-bit checksum is stored big-endian as the first 4
bytes of the file, followed by the model name (`ipvd` for Video,
`nano` for Nano).

## Boot ROM → our code: handoff state

When the boot ROM jumps to the OSOS entry point, we inherit:

- **CPU mode**: ARM supervisor (CPSR = `0xD3`); IRQ + FIQ disabled.
- **Cores**: both CPU and COP are powered, but only CPU is running.
- **MMU**: disabled.
- **Cache**: disabled.
- **SDRAM**: mapped at its native address (`0x10000000`), NOT yet at
  `0x00000000`.
- **Stack**: undefined — we must set up our own immediately.
- **Vectors**: at default ARM vector address (`0x00000000`), pointing
  at boot ROM. Until we remap SDRAM and copy our own vector table
  there, we cannot handle exceptions cleanly.

Our `crt0.S` job:

1. Set up a temporary stack in IRAM.
2. Remap SDRAM to `0x00000000` (using IRAM-resident remap code so we
   don't pull the rug out from under ourselves — see
   `firmware/target/arm/pp/crt0-pp.S` lines 84–107).
3. Copy our `.data` from ROM image to SDRAM.
4. Zero `.bss`.
5. Initialize cache (see [01-soc-pp5022.md](01-soc-pp5022.md)).
6. Wake the COP (writes its control word, COP does its own remap).
7. Jump to `main()` on CPU.

## Dock connector: pinout

The 30-pin dock connector carries power, USB, FireWire (legacy), the
UART debug interface, line-level audio out, accessory ID, and a few
control lines. Not all of this is in the Rockbox source — much of
the pinout is reverse-engineered iPodLinux work.

| Pin   | Function (5G)                                   |
|-------|-------------------------------------------------|
| 1, 2  | GND                                             |
| 3, 4  | Audio L / R out (line level)                    |
| 5     | Audio GND                                       |
| 8, 9  | Video out                                       |
| 11    | Serial GND                                      |
| 13    | UART TX (`SER0_THR` from SoC)                   |
| 14    | UART RX (`SER0_RBR` to SoC)                     |
| 15    | Reserved                                        |
| 16    | USB GND                                         |
| 19, 20| Accessory power                                 |
| 21    | Accessory ID (resistor divider)                 |
| 23    | USB +5 V                                        |
| 25    | USB D−                                          |
| 27    | USB D+                                          |
| 26, 28, 29, 30 | FireWire data + power (only used when FireWire is the carrier; idle when over USB) |

> Pin assignments differ between iPod models and across some
> revisions. The 5G/5.5G layout above is what's documented in the
> iPodLinux pinout pages and what Rockbox's `serial-ipod-pp.c`
> assumes when it routes UART.
>
> Note: pin 25 carries USB D− on the iPod Video. Earlier docs
> sometimes describe pins 25–30 collectively as the "FireWire region"
> because on a FireWire-only iPod the same physical pins are
> repurposed; on the 5G the assignments above are exclusive.

## UART debug

UART is our primary out-of-band debug channel — when our firmware
panics and the LCD is dead, this is what we have left.

### SoC registers (8250-style at SER0)

The register file is the standard 8250 byte layout widened so each
register occupies a 32-bit word slot — uniform 4-byte stride, all
accesses word-wide. The classic 8250 sharing holds: RBR/THR/DLL share
`+0x00`, IER/DLM share `+0x04`, IIR/FCR share `+0x08`.

| Register   | Address      | Purpose |
|------------|--------------|---------|
| `SER0_RBR` | `0x70006000` | RX buffer (read) |
| `SER0_THR` | `0x70006000` | TX holding (write) |
| `SER0_DLL` | `0x70006000` | Divisor latch (low)  — when `LCR.bit7` set |
| `SER0_IER` | `0x70006004` | IRQ enable |
| `SER0_DLM` | `0x70006004` | Divisor latch (high) — when `LCR.bit7` set |
| `SER0_IIR` | `0x70006008` | IRQ identification (read) |
| `SER0_FCR` | `0x70006008` | FIFO control (write) |
| `SER0_LCR` | `0x7000600C` | Line control |
| `SER0_MCR` | `0x70006010` | Modem control |
| `SER0_LSR` | `0x70006014` | Line status: bit 0 = RX ready, bit 5 = TX empty |
| `SER0_MSR` | `0x70006018` | Modem status |

*(Corrected 2026-06-10 against Rockbox `pp5020.h`: an earlier revision
byte-strided `SER0_IER`/`SER0_FCR` at `0x70006001`/`0x70006002`, which
contradicted both the word stride of the other registers and the 8250
offset-sharing convention — a transcription slip.)*

`DEV_SER0` (the SER0 bit in `DEV_EN`/`DEV_RS`) = `0x40` (bit 6);
`DEV_SER1` = `0x80` (bit 7). `GPO32_VAL` = `0x70000080`,
`GPO32_ENABLE` = `0x70000084` (verified against Rockbox `pp5020.h`,
2026-06-10).

### Baud rate

Reference clock = 24 MHz. Divisor = `24_000_000 / (16 * baud)`.

| Baud   | Divisor (decimal) | Divisor (hex) |
|--------|-------------------|---------------|
| 9600   | 156               | `0x9C`        |
| 19200  | 78                | `0x4E`        |
| 38400  | 39                | `0x27`        |
| 57600  | 26                | `0x1A`        |
| 115200 | 13                | `0x0D`        |

### Init sequence (default 115200 8-N-1)

Full iPod Video order (routing first, then power/reset, then line
setup; the reference also masks the SER0 IRQ at the controller via
`CPU_HI_INT_DIS` between enable and reset — only needed if `IER` is
going to be set):

```c
// GPIO routing first — see next section
routing_reg(0x7000008C) &= ~0x0C;      // route SER0 TX/RX to dock pins
GPO32_ENABLE            &= ~0x0C;      // release pads from GPO mode

DEV_EN |= DEV_SER0;                    // power UART (bit 6)
DEV_RS |= DEV_SER0;                    // assert reset
sleep(1);                              // one ~10 ms tick
DEV_RS &= ~DEV_SER0;                   // release

SER0_LCR = 0x80;                       // enable divisor latch access
SER0_DLM = 0x00;
SER0_LCR = 0x03;                       // disable latch, 8-N-1
SER0_IER = 0x01;                       // RX IRQ (for accessory RX)
SER0_FCR = 0x07;                       // enable + reset FIFOs
SER0_LCR = 0x80; SER0_DLL = 0x0D;      // 115200: divisor low byte,
SER0_LCR = 0x03;                       //   via a second DLAB window
```

All accesses are 32-bit word writes. The divisor's low byte is
programmed last through a second DLAB window in the reference;
writing DLL inside the first window is functionally equivalent.
A polled-TX-only driver can skip the `IER`/`FCR` lines (non-FIFO
8250 TX works via THR/THRE).

(Source: `firmware/target/arm/pp/uart-pp.c` — init block ~lines
67–87 + `set_bitrate`; re-verified 2026-06-10.)

### GPIO routing for SER0 on iPod Video

```c
outl(inl(0x7000008C) & ~0x0000000C, 0x7000008C);  // route SER0 TX/RX via GPIO bits 2-3
GPO32_ENABLE &= ~0x0000000C;                      // 0x70000084 — release pads from GPO mode
```

(The `0x7000008C` routing register has no symbolic name even in
Rockbox, which uses the raw literal.)

### Autobaud (Apple Accessory Protocol)

The dock UART also speaks Apple's Accessory Protocol (IAP) — packetized
with a sync byte (`0xFF`) and command frames. Rockbox implements
autobaud detection: by examining the first byte received (`0xFF`,
`0xFC`, `0xE0`, `0xFE`, etc.) the firmware can infer the host's baud
rate and switch among 9600 / 19200 / 38400 / 57600 / 115200. (See
`uart-pp.c` lines 185–270.)

For us: we just want raw debug at 115200. We don't need to talk IAP
unless we want accessory support, which is out of scope.

## Accessory detection

Pin 21 (accessory ID) is connected via a resistor divider to the
PCF50605 ADC. Different accessories use different resistors, giving
distinguishable ADC readings:

- USB power adapter — high resistance.
- Original Apple dock — specific value.
- Headphone remote — specific value.
- Line-out dock — specific value.

The firmware reads the ADC channel, compares against a lookup table.
(Source: `firmware/target/arm/ipod/adc-ipod-pcf.c` and the bootloader
at `bootloader/ipod.c` 62–145.)

For us: nice-to-have; defer until everything else works.

## Disk Mode (Apple's fallback)

iPod's "disk mode" is a recovery state where the iPod is a plain USB
mass-storage device with no music UI. It's reached two ways:

### From the bootloader

Hold **Select + Play** at boot. The bootloader detects this, writes
a magic string to a known RAM location, and reboots. The boot ROM
recognizes the magic and enters disk mode instead of loading the OS:

```c
// PP5020 (5G Video):
memcpy((void *)0x4001FF00,
       "diskmode\0\0hotstuff\0\0\1", 21);
system_reboot();
```

(Source: `bootloader/ipod.c` 228–235.)

### From running firmware

Same trick: write the magic string, reboot.

### As recovery

Disk mode is in ROM and works regardless of what's installed in the
firmware partition — even if you've completely bricked the OSOS
image, Select+Play at boot still works. This is the recovery path.

## Diagnostic mode

A separate, less-documented mode reachable via Select+Play during
the *Apple* boot animation (not Rockbox/our boot). Provides low-level
hardware testing. Rockbox doesn't implement its own; we'd inherit
this from the Apple firmware staying installed alongside (which it
doesn't, since we replace OSOS).

For us: not needed. Disk mode covers recovery.

## Hold switch at boot

GPIO A bit 5 (active-low on Video). The bootloader checks this
immediately after CPU init:

```c
// firmware/bootloader/ipod.c lines 175–182
bool held = !(GPIOA_INPUT_VAL & 0x20);
if (held) {
    boot_apple_firmware();   // priority Apple OS
} else {
    boot_rockbox();          // (or our OS)
}
```

This convention lets users dual-boot easily: hold = Apple, release =
custom. Worth preserving in our bootloader (we use Rockbox's
unchanged for now anyway).

## Recovery checklist

If our firmware bricks the device, the recovery path is:

1. **Hold Select + Play at boot** to enter Apple disk mode (in ROM,
   always works).
2. iPod enumerates as USB MSC.
3. From a host, run our recovery tool:
   - `core recover` — restores the factory bootloader and Apple OS.
   - `core recover --reflash` — re-installs our firmware fresh.
4. If neither works (extreme cases — corrupted firmware partition):
   `ipodpatcher --restore <path-to-apple-firmware.bin>` from the
   iPodLinux project, plus an Apple firmware blob (which Apple no
   longer hosts; users keep copies).

The fact that Apple disk mode is in ROM is the load-bearing safety
net for everything we ship. **We must never write to the boot ROM**
or replace the partition signature. Both would brick beyond
software-recoverability.

## ipodpatcher — what we absorb into Go

Our Go CLI's `internal/bootloader` package re-implements the parts of
ipodpatcher we need:

1. Read firmware partition; parse image directory.
2. Locate OSOS image entry.
3. Insert our bootloader at the appropriate offset (preserving
   Apple's `entryOffset` so we can chain back to Apple).
4. Update the image directory entry (`len`, `entryOffset`).
5. Recompute checksum.
6. Write the partition back.

ipodpatcher is GPL-2.0, ~3000 LoC of C. We re-implement clean from
the spec above; we don't link the C source. The actual firmware
patches we write (the bootloader binary itself) are also our code,
embedded in the Go binary via `go:embed`.

## Source citations

| Topic                         | File |
|-------------------------------|------|
| Boot loader entry             | `bootloader/ipod.c` |
| CPU init / SDRAM remap        | `firmware/target/arm/pp/crt0-pp.S` |
| UART driver                   | `firmware/target/arm/pp/uart-pp.c` |
| Firmware partition format     | `utils/ipodpatcher/ipodpatcher.c` |
| Register definitions          | `firmware/export/pp5020.h` |
| GPIO / button definitions     | `firmware/export/pp5020.h` (GPIOx_* blocks) |
| ADC accessory readings        | `firmware/target/arm/ipod/adc-ipod-pcf.c` |
