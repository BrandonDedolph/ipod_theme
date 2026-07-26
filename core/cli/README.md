# `core` — host CLI

Single Go binary intended to handle install, update, recovery, dev
iteration, the simulator, and the test suite. One executable per
platform; users download `core-{os}-{arch}` from GitHub Releases and run
it.

Most of that is still ahead: today the binary packs and unpacks firmware
images, builds a host-side music index, and shells out to `make`. See
[Status](#status) for exactly what works.

## Build

```bash
cd core/cli
go build -o core ./cmd/core
./core --help
```

Cross-compile for other platforms:

```bash
GOOS=darwin  GOARCH=amd64 go build -o core-darwin-amd64  ./cmd/core
GOOS=darwin  GOARCH=arm64 go build -o core-darwin-arm64  ./cmd/core
GOOS=linux   GOARCH=amd64 go build -o core-linux-amd64   ./cmd/core
GOOS=linux   GOARCH=arm64 go build -o core-linux-arm64   ./cmd/core
GOOS=windows GOARCH=amd64 go build -o core-windows.exe   ./cmd/core
```

## Test

```bash
cd core/cli
go test ./...
```

## Layout

```
cmd/core/         main package (just calls cli.Root().Execute())
internal/
  cli/            cobra command tree (one file per subcommand)
  firmware/       .ipod transport format + firmware-partition helpers
                  (checksum, directory entry, directory locator)
  ipod/           USB device detection — type system only, no working
                  enumeration on any platform (see Status)
  tagcache/       TCDB, a host-side music index the firmware does NOT
                  read; see internal/tagcache/README.md
  artistart/      artist-photo fetcher for --fetch-art (opt-in,
                  local-only, third-party licensing — read the package doc)
  version/        build version info, stamped via -ldflags at release time
```

## Status

Read this before assuming a command does something.

**Load-bearing today** — these work and are used:

| Command | Notes |
|---|---|
| `firmware pack` | Wraps a raw image in the .ipod format. `core/Makefile`'s `ipod` target runs it on every build, so this is the one command a broken change actually breaks. Validates its input, writes atomically, verifies by read-back. |
| `firmware unpack` | Extracts + checksum-verifies an image. `--ignore-checksum` for recovery. |
| `tagcache build` / `dump` | Builds and decodes TCDB — a **host-side** index. The device does not read it (it reads CIDX, from `tools/build_index.py`). See `internal/tagcache/README.md`. |
| `build hw` / `build sim` | Thin wrapper over `make -C core <target>`. |

**Not implemented** — these return `not yet implemented`:
`flash`, `install`, `update`, `recover`, `info`, `debug`, `sim`,
`test`, `release`. That is 9 of 12 top-level commands. They are wired
with help text and flags describing the intended behavior; none of them
touch a device.

**Device detection does not exist on any platform.**
`internal/ipod/detect_stub.go` carries the build tag
`!corehas_real_detect`, which nothing ever sets, so the stub is compiled
in everywhere and `Detect()` returns an error unconditionally. The
Generation / Mode / Device types are real; the USB enumeration behind
them is not.

**There is no signing or verification code.** `internal/release` does
not exist, there is no release key, and there is no embedded public key.
The `--verify` (on `update`) and `--sign` (on `release`) flags — both of
which defaulted to `true` — have been removed; a flag that claims to
verify a signature while verifying nothing is worse than no flag. They
come back with the code.

**`internal/firmware/imageheader.go` is incomplete scaffolding** with no
non-test callers. It implements the 40-byte directory entry, the
directory marker, and the directory locator (`]ih[` at `0x100`, LE16
version at `0x10A`, directory start = `LE32@0x104 + 0x200`) per
`core/docs/hw/08-boot-dock.md`. It does **not** handle the Apple
partition preamble, which any install path must preserve byte-exact.

**The write path has a safety checklist and no code.** Before anything
writes to a block device, read the checklist at the top of
`internal/cli/install.go`: prove the target is an iPod three ways, back
up the firmware partition first, verify by read-back, and support
`--dry-run` plus a typed confirmation. The global `--device` flag exists
to disambiguate when several iPods are connected
(`ipod.ErrMultipleDevices`).

## Design notes

- **Cobra** for the command tree. We considered urfave/cli too — cobra
  won on better help-text rendering and richer flag types.
- **Minimal deps** — cobra + pflag + mousetrap (cobra's transitive
  Windows-mode dep), plus `dhowden/tag` for the tagcache scanner.
  Anything more goes through a code review.
- **Output files are written atomically.** `writeFileAtomic` in
  `internal/cli/firmware.go` writes to a temp file, fsyncs, verifies by
  read-back, and renames. `--force` must never destroy the previous
  artifact before the replacement is complete on disk — `make ipod`
  passes `--force` on every build, and a truncated `core.ipod` still
  parses as a valid header.
- **Artist art fetched by `--fetch-art` is local-only.** The images are
  third-party works under third-party terms (Deezer press imagery is not
  freely licensed; Wikimedia Commons images are usually CC BY-SA and
  require attribution). Provenance is recorded per image in the cache.
  They must never be bundled into a release artifact.
- The CLI would be the only thing that talks to GitHub. Shipping
  anything unsigned or fetching anything insecure would be a security
  blunder in this kind of tool, so signature verification is a hard
  prerequisite for `core update` — and until it is written, the flags
  that imply it stay out of the help text.
