package cli

import (
	"errors"

	"github.com/spf13/cobra"
)

// SAFETY CHECKLIST FOR THE WRITE PATH — READ BEFORE IMPLEMENTING
//
// install, flash and recover are stubs today, so none of the guards
// below exist yet. They are written down here because the cost of
// getting this wrong is someone else's 80 GB disk, and because the
// documented flow (detect -> confirm -> elevate -> write) has no step
// that proves the block device being written is actually an iPod.
//
// Any code that writes to a block device MUST do all of the following.
// A reviewer should reject a write path missing any one of them.
//
//	(a) PROVE THE TARGET IS AN IPOD — three independent checks, all
//	    required, before a single byte is written:
//	      1. USB VID/PID matches Apple (0x05AC) and a known iPod Video
//	         product ID (see internal/ipod).
//	      2. The Apple firmware-partition preamble is present and
//	         byte-exact at the start of partition 0, and the directory
//	         marker "]ih[" is at offset 0x100 (see
//	         core/docs/hw/08-boot-dock.md and internal/firmware). A
//	         device that enumerates right but has no preamble is not a
//	         device to write to.
//	      3. Disk size is consistent with the detected model. A 2 TB
//	         "iPod" is a USB enclosure someone left plugged in.
//	    VID/PID alone is not sufficient: the OS block-device path can
//	    be reassigned between enumeration and open.
//
//	(b) BACK UP FIRST — dump the entire existing firmware partition to
//	    a timestamped file (e.g. ~/.local/share/core/backups/
//	    fwpart-<serial>-<RFC3339>.bin) and fsync it before the first
//	    write. The Apple preamble must be preserved byte-exact; the
//	    boot ROM will not load a partition without it, and the only
//	    copy of a given device's may be the one on that device.
//
//	(c) VERIFY BY READ-BACK — after writing, re-read the written extent
//	    and compare a checksum against the source. Report failure
//	    loudly and point at the backup from (b). A write that is not
//	    read back is a write that was not verified.
//
//	(d) --dry-run AND A TYPED CONFIRMATION — --dry-run prints the exact
//	    device, extents and byte counts and writes nothing. Without it,
//	    require the user to type the device path (not "y") to proceed,
//	    unless --yes was passed. --yes exists today and skips a prompt
//	    nobody has written; it must not become a way to skip (a)–(c).
//
// The global --device flag selects the target when more than one iPod
// is connected (internal/ipod.ErrMultipleDevices); read it with
// deviceFlag(cmd).

func newInstallCmd() *cobra.Command {
	var (
		yes      bool
		bootonly bool
		dryRun   bool
	)

	cmd := &cobra.Command{
		Use:   "install",
		Short: "First-time install onto a stock iPod (not yet implemented)",
		Long: `Installs the bootloader and our firmware on a stock iPod (Apple OS or
Rockbox).

NOT YET IMPLEMENTED — needs hardware in the loop.

The intended flow:
  1. Detect the connected iPod via USB (model, capacity, firmware mode);
     --device selects one when several are connected.
  2. Prove the target really is an iPod: VID/PID, the Apple firmware-
     partition preamble, and a disk size consistent with the model.
  3. Dump the existing firmware partition to a timestamped backup.
  4. Prompt for confirmation (skip with --yes) or print and exit with
     --dry-run.
  5. Prompt for OS-level elevation (sudo / pkexec / UAC) since writing
     the firmware partition needs raw block-device access.
  6. Write our bootloader to the firmware partition, preserving the
     Apple preamble byte-exact.
  7. Read back and verify by checksum.
  8. Mount the data partition and write /core.ipod + /.core/ assets.
  9. Unmount cleanly; tell the user to disconnect and reboot.

Steps 2, 3 and 7 are not optional — see the safety checklist in
internal/cli/install.go.

Recovery is documented in RECOVERY.md (Select+Play boots into Apple
disk mode regardless of firmware state, so this is reversible).`,
		RunE: func(cmd *cobra.Command, args []string) error {
			_ = yes
			_ = bootonly
			_ = dryRun
			_ = deviceFlag(cmd)
			return errors.New("not yet implemented — needs hardware in the loop")
		},
	}

	cmd.Flags().BoolVarP(&yes, "yes", "y", false,
		"Skip confirmation prompts (does not skip target verification, backup or read-back)")
	cmd.Flags().BoolVar(&bootonly, "bootloader-only", false,
		"Install only the bootloader (skip firmware + assets)")
	cmd.Flags().BoolVar(&dryRun, "dry-run", false,
		"Print exactly what would be written and exit without writing")
	return cmd
}
