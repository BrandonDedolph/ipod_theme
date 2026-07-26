package cli

import (
	"errors"

	"github.com/spf13/cobra"
)

func newUpdateCmd() *cobra.Command {
	var (
		zip  string
		self bool
	)

	cmd := &cobra.Command{
		Use:   "update [release-zip]",
		Short: "Update an iPod already running our firmware (not yet implemented)",
		Long: `Updates the firmware on a connected iPod that's already running our
build. The iPod must be in update mode (USB connected, "Connected —
safe to update" screen visible).

NOT YET IMPLEMENTED. The intended flow is to fetch the latest release
from GitHub Releases, verify its signature, and apply it atomically
(write new files alongside old, then rename into place), keeping
/core.ipod.prev as a fallback the bootloader uses if the new image
fails to boot N times.

There is deliberately no --verify flag. It used to exist and defaulted
to true, which read as a security guarantee for code that does not
exist: there is no signing key, no embedded public key, and no
verification code anywhere in this module. The flag comes back when
the verification does, not before.

Use --self to update the host CLI itself (the 'core' binary) rather
than the iPod's firmware.`,
		Args: cobra.MaximumNArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			if len(args) == 1 {
				zip = args[0]
			}
			_ = zip
			if self {
				return errors.New("not yet implemented — self-update")
			}
			return errors.New("not yet implemented — needs hardware in the loop")
		},
	}

	cmd.Flags().BoolVar(&self, "self", false, "Update the host CLI binary instead of the iPod firmware")
	return cmd
}
