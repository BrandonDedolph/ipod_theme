// Package cli wires the cobra command tree.
package cli

import (
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/version"
	"github.com/spf13/cobra"
)

// Root returns the configured root command. Each subcommand lives in its
// own file in this package and is attached here so the layout mirrors the
// help-text grouping ('build / dev', 'install / update / recover',
// 'test / release', 'info / debug').
func Root() *cobra.Command {
	root := &cobra.Command{
		Use:           "core",
		Short:         "Custom iPod Video 5G/5.5G firmware — host CLI",
		Long:          longDescription,
		Version:       version.Full(),
		SilenceUsage:  true,
		SilenceErrors: true,
	}

	// Global device selector. internal/ipod already defines
	// ErrMultipleDevices ("specify which one") with no way for the user
	// to do the specifying; this is that way. Read it with deviceFlag.
	root.PersistentFlags().String("device", "",
		"Target a specific iPod: OS block-device path (/dev/sdX, /dev/diskN, "+
			"\\\\.\\PhysicalDriveN) or serial number. Required when more than one is connected.")

	root.AddCommand(
		newBuildCmd(),
		newSimCmd(),
		newFlashCmd(),
		newInstallCmd(),
		newUpdateCmd(),
		newRecoverCmd(),
		newInfoCmd(),
		newDebugCmd(),
		newTestCmd(),
		newReleaseCmd(),
		newTagcacheCmd(),
		newFirmwareCmd(),
	)

	return root
}

// deviceFlag returns the value of the global --device selector, or ""
// when the user didn't pass one. Defined here so no command grows its
// own private copy of the flag.
func deviceFlag(cmd *cobra.Command) string {
	v, err := cmd.Flags().GetString("device")
	if err != nil {
		return ""
	}
	return v
}

const longDescription = `core is the host-side CLI for the custom iPod Video firmware project.

Build and dev:
  core build hw       Cross-compile the firmware image (ARM)
  core build sim      Build the host simulator (SDL2)
  core sim            Launch the interactive simulator
  core flash          Push the latest build to a connected iPod
  core debug          Stream the dock-connector UART log

Install and update:
  core install        First-time install onto a stock iPod
  core update         Update an iPod already running our firmware
  core recover        Restore factory firmware (or reflash ours)
  core info           Detect and identify a connected iPod

Test and release:
  core test           Run the test suite (unit / sim / hw / battery / soak)
  core release        Build, sign, and package a release zip

Music index:
  core tagcache       Build and inspect the binary music index (.tcdb)

Firmware images:
  core firmware       Pack / unpack .ipod transport-format images

Run "core <command> --help" for command-specific options.
`
