package cli

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/firmware"
	"github.com/spf13/cobra"
)

func newFirmwareCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "firmware",
		Short: "Low-level firmware image operations (.ipod packaging, partition format)",
		Long: `Operations on the iPod firmware-partition image format and the .ipod
transport format used by ipodpatcher-style installers.

These are building blocks; everyday flashing goes through "core flash"
and "core install", which call into this package internally.`,
	}
	cmd.AddCommand(newFirmwarePackCmd())
	cmd.AddCommand(newFirmwareUnpackCmd())
	return cmd
}

// claimOutputPath enforces the no-clobber rule for a command's output.
//
// Without --force we create the path with O_EXCL, which is the only
// atomic "this name was not already taken" primitive we have; the
// zero-byte placeholder it leaves behind is renamed over by
// writeFileAtomic, or removed if anything later fails. Reports whether
// a placeholder was created so the caller knows what to clean up.
func claimOutputPath(path string, force bool) (created bool, err error) {
	if force {
		return false, nil
	}
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o644)
	if err != nil {
		if os.IsExist(err) {
			return false, fmt.Errorf("%s already exists; pass --force to overwrite", path)
		}
		return false, fmt.Errorf("create %s: %w", path, err)
	}
	if err := f.Close(); err != nil {
		_ = os.Remove(path)
		return false, fmt.Errorf("create %s: %w", path, err)
	}
	return true, nil
}

// writeFileAtomic produces `path` such that it is either the previous
// content or the complete new content — never a truncated prefix.
//
// This matters because "make ipod" runs `firmware pack --force` on every
// build. Opening the destination with O_TRUNC destroys the last known-good
// core.ipod before the first byte of the replacement is written, and the
// 8-byte header goes out ahead of the image, so a failure mid-write leaves
// a file that still parses as a valid .ipod header with a truncated image
// behind it. That is exactly the artifact you do not want to flash.
//
// So: write to a temp file in the destination directory, fsync it, read it
// back and hand it to `verify`, and only then rename it into place. Rename
// within a directory is atomic, so a reader either sees the old file or the
// new one. Any failure removes the temp file (and the O_EXCL placeholder,
// if we made one) and leaves the previous output untouched.
func writeFileAtomic(path string, force bool, write func(io.Writer) error, verify func([]byte) error) error {
	claimed, err := claimOutputPath(path, force)
	if err != nil {
		return err
	}
	cleanupClaim := func() {
		if claimed {
			_ = os.Remove(path)
		}
	}

	dir := filepath.Dir(path)
	tmp, err := os.CreateTemp(dir, "."+filepath.Base(path)+".tmp-*")
	if err != nil {
		cleanupClaim()
		return fmt.Errorf("create temp file in %s: %w", dir, err)
	}
	tmpName := tmp.Name()
	fail := func(err error) error {
		_ = tmp.Close()
		_ = os.Remove(tmpName)
		cleanupClaim()
		return err
	}

	if err := write(tmp); err != nil {
		return fail(err)
	}
	// fsync before trusting the bytes. Without it the read-back below can
	// be served entirely from the page cache and "verify" data the kernel
	// later fails to write out.
	if err := tmp.Sync(); err != nil {
		return fail(fmt.Errorf("fsync %s: %w", tmpName, err))
	}
	// Check Close explicitly: on many filesystems this is where a deferred
	// write finally reports ENOSPC or EIO. A deferred, unchecked
	// `defer f.Close()` turns that into a silent success and hands the
	// user a truncated image to flash.
	if err := tmp.Close(); err != nil {
		return fail(fmt.Errorf("close %s: %w", tmpName, err))
	}
	// os.CreateTemp makes the file 0600; the direct-write path this
	// replaced produced 0644, and the output is an artifact meant to be
	// read and copied around, not a secret.
	if err := os.Chmod(tmpName, 0o644); err != nil {
		return fail(fmt.Errorf("chmod %s: %w", tmpName, err))
	}
	if verify != nil {
		data, err := os.ReadFile(tmpName)
		if err != nil {
			return fail(fmt.Errorf("read back %s: %w", tmpName, err))
		}
		if err := verify(data); err != nil {
			return fail(err)
		}
	}
	if err := os.Rename(tmpName, path); err != nil {
		return fail(fmt.Errorf("rename %s -> %s: %w", tmpName, path, err))
	}
	syncDir(dir)
	return nil
}

// syncDir best-effort fsyncs a directory so the rename itself is durable.
// Failure is ignored: opening a directory for read is not portable (it
// fails on Windows), and a non-durable-but-correct rename is still an
// improvement over a truncating write.
func syncDir(dir string) {
	d, err := os.Open(dir)
	if err != nil {
		return
	}
	_ = d.Sync()
	_ = d.Close()
}

// Sanity bounds on the raw image handed to "firmware pack".
//
// These are not the firmware partition budget — the real limit is the
// size of partition 0 on the target device, which is only knowable with
// the device attached, and must be re-checked by any future install
// path. They exist to catch the failure that actually happens: objcopy
// producing an empty or truncated core.bin, which today packs into a
// perfectly "valid" core.ipod and makes `make ipod` report success.
const (
	// minPackImageBytes is far below any real build (our own image is
	// ~220 KB, a stock Rockbox build ~760 KB) but above the 0-and-change
	// bytes a failed objcopy leaves behind.
	minPackImageBytes = 4096
	// maxPackImageBytes is a generous ceiling; Apple's own OSOS images
	// are a few MB.
	maxPackImageBytes = 32 << 20
)

// validatePackImage rejects images that cannot plausibly be firmware.
// Structural oddities that are merely suspicious are written to `warn`
// rather than failing the build.
func validatePackImage(path string, image []byte, warn io.Writer) error {
	switch {
	case len(image) == 0:
		return fmt.Errorf("%s is empty; refusing to pack a 0-byte firmware image "+
			"(the usual cause is objcopy failing silently)", path)
	case len(image) < minPackImageBytes:
		return fmt.Errorf("%s is only %d bytes, below the %d-byte minimum for a plausible "+
			"firmware image; refusing to pack what looks like a truncated build",
			path, len(image), minPackImageBytes)
	case len(image) > maxPackImageBytes:
		return fmt.Errorf("%s is %d bytes, above the %d-byte sanity ceiling for a firmware "+
			"image; refusing to pack (is this really core.bin?)",
			path, len(image), maxPackImageBytes)
	}
	if b, uniform := uniformByte(image); uniform {
		return fmt.Errorf("%s is %d bytes of nothing but %#02x; refusing to pack a blank image",
			path, len(image), b)
	}
	// The reset vector of our image is a branch, but that is not a
	// property of .ipod images in general: a stock rockbox.ipod starts
	// with 0xE321F0D3 (MSR CPSR_c, ...), not a branch. So a non-branch
	// first word is worth mentioning and nothing more.
	if !looksLikeARMBranch(image) {
		fmt.Fprintf(warn, "warning: %s does not begin with an ARM branch instruction "+
			"(first word %#08x); packing anyway\n", path, firstWordLE(image))
	}
	return nil
}

// uniformByte reports whether every byte of b is identical (the shape of
// an all-zero or erased-flash 0xFF buffer).
func uniformByte(b []byte) (byte, bool) {
	if len(b) == 0 {
		return 0, false
	}
	for _, x := range b[1:] {
		if x != b[0] {
			return 0, false
		}
	}
	return b[0], true
}

func firstWordLE(image []byte) uint32 {
	if len(image) < 4 {
		return 0
	}
	return uint32(image[0]) | uint32(image[1])<<8 | uint32(image[2])<<16 | uint32(image[3])<<24
}

// looksLikeARMBranch reports whether the first word decodes as an ARM
// B/BL: bits 27:25 == 0b101, with any condition code other than 0b1111
// (which is the unconditional-instruction space, not a branch).
func looksLikeARMBranch(image []byte) bool {
	w := firstWordLE(image)
	if len(image) < 4 {
		return false
	}
	return (w&0x0E000000) == 0x0A000000 && (w>>28) != 0xF
}

func newFirmwarePackCmd() *cobra.Command {
	var (
		out   string
		force bool
	)
	cmd := &cobra.Command{
		Use:   "pack <image.bin>",
		Short: "Wrap a raw firmware image in the .ipod transport format",
		Long: `Reads a flat firmware binary (typically produced by objcopy -O binary
from the hw-build ELF) and emits a .ipod-format file: a 4-byte big-endian
additive checksum, the 4-byte model name ("ipvd" for iPod Video), then
the image bytes.

The input is sanity-checked first (non-empty, plausible size, not a blank
buffer) so a failed objcopy can't produce a "valid" .ipod. The output is
written to a temp file, fsynced, read back and re-verified, and only then
renamed into place — an interrupted pack leaves the previous image intact
rather than a truncated one that still parses.

The output is what "core install" / "core update" write to the device.`,
		Args: cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			if out == "" {
				return errors.New("--out is required")
			}
			image, err := os.ReadFile(args[0])
			if err != nil {
				return fmt.Errorf("read %s: %w", args[0], err)
			}
			if err := validatePackImage(args[0], image, cmd.ErrOrStderr()); err != nil {
				return err
			}

			err = writeFileAtomic(out, force,
				func(w io.Writer) error {
					return firmware.WriteIPodFile(w,
						firmware.ModelIPodVideo, firmware.ModelNameIPodVideo, image)
				},
				func(data []byte) error {
					name, got, err := firmware.ReadIPodFile(bytes.NewReader(data))
					if err != nil {
						return fmt.Errorf("read-back verification of %s failed: %w", out, err)
					}
					if name != firmware.ModelNameIPodVideo {
						return fmt.Errorf("read-back verification of %s failed: model %q, want %q",
							out, string(name[:]), string(firmware.ModelNameIPodVideo[:]))
					}
					if !bytes.Equal(got, image) {
						return fmt.Errorf("read-back verification of %s failed: "+
							"%d image bytes on disk, %d written", out, len(got), len(image))
					}
					return nil
				})
			if err != nil {
				return err
			}
			fmt.Fprintf(cmd.OutOrStdout(),
				"wrote %s (%d image bytes + %d header bytes, checksum verified)\n",
				out, len(image), firmware.IPodFileHeaderSize)
			return nil
		},
	}
	cmd.Flags().StringVarP(&out, "out", "o", "", "Output .ipod path (required)")
	cmd.Flags().BoolVarP(&force, "force", "f", false,
		"Overwrite the output file if it already exists")
	return cmd
}

func newFirmwareUnpackCmd() *cobra.Command {
	var (
		out             string
		force           bool
		ignoreChecksum  bool
		ignoreModelName bool
	)
	cmd := &cobra.Command{
		Use:   "unpack <image.ipod>",
		Short: "Extract the raw image bytes from a .ipod file (verifying checksum)",
		Long: `Extracts the image payload from a .ipod-format file, verifying the
embedded additive checksum against the model seed.

ReadIPodFile deliberately returns the image bytes alongside a checksum
error so recovery tooling can inspect a damaged image. Pass
--ignore-checksum to actually write those bytes out; the command prints
a loud warning and the result must not be flashed to a device.`,
		Args: cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			if out == "" {
				return errors.New("--out is required")
			}
			f, err := os.Open(args[0])
			if err != nil {
				return fmt.Errorf("open %s: %w", args[0], err)
			}
			defer f.Close()

			name, image, err := firmware.ReadIPodFile(f)
			if err != nil {
				recoverable := (ignoreChecksum && errors.Is(err, firmware.ErrIPodChecksumMismatch)) ||
					(ignoreModelName && errors.Is(err, firmware.ErrUnknownModelName))
				if !recoverable || image == nil {
					return err
				}
				fmt.Fprintf(cmd.ErrOrStderr(),
					"WARNING: %v\n"+
						"WARNING: writing the image anyway because you asked; these bytes are\n"+
						"WARNING: UNVERIFIED and must not be flashed to a device.\n", err)
			}
			werr := writeFileAtomic(out, force,
				func(w io.Writer) error {
					if _, err := w.Write(image); err != nil {
						return fmt.Errorf("write %s: %w", out, err)
					}
					return nil
				},
				func(data []byte) error {
					if !bytes.Equal(data, image) {
						return fmt.Errorf("read-back verification of %s failed: "+
							"%d bytes on disk, %d written", out, len(data), len(image))
					}
					return nil
				})
			if werr != nil {
				return werr
			}
			fmt.Fprintf(cmd.OutOrStdout(),
				"unpacked %s (model=%q, %d image bytes) → %s\n",
				args[0], string(name[:]), len(image), out)
			return nil
		},
	}
	cmd.Flags().StringVarP(&out, "out", "o", "", "Output raw image path (required)")
	cmd.Flags().BoolVarP(&force, "force", "f", false,
		"Overwrite the output file if it already exists")
	cmd.Flags().BoolVar(&ignoreChecksum, "ignore-checksum", false,
		"Write the image even if the embedded checksum does not verify (recovery only)")
	cmd.Flags().BoolVar(&ignoreModelName, "ignore-model", false,
		"Write the image even if the embedded model name is unknown (recovery only)")
	return cmd
}
