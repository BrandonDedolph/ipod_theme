package cli

import (
	"errors"

	"github.com/spf13/cobra"
)

func newReleaseCmd() *cobra.Command {
	var ver string

	cmd := &cobra.Command{
		Use:   "release",
		Short: "Build and package a release zip (not yet implemented)",
		Long: `Builds the firmware, packages assets, and emits a release zip ready
for distribution via GitHub Releases.

NOT YET IMPLEMENTED.

Intended output:
  build/release/core-{os}-{arch}         — host CLI binaries
  build/release/core-firmware-vX.Y.Z.zip — firmware payload + assets
  build/release/checksums.txt

There is deliberately no --sign flag. It used to exist and defaulted to
true, promising artifact signing for which no release key, no signing
code and no verification path exists. It comes back when signing does.

When this lands: artist art fetched by "core tagcache build --fetch-art"
must never be included. Those images are third-party works under
third-party terms (see internal/artistart).
`,
		RunE: func(cmd *cobra.Command, args []string) error {
			_ = ver
			return errors.New("not yet implemented — pending hw build path")
		},
	}
	cmd.Flags().StringVar(&ver, "version", "", "Release version (defaults to git describe)")
	return cmd
}
