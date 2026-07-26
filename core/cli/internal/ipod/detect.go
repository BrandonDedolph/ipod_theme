// Package ipod handles USB device detection and probing.
//
// On all platforms the basic flow is the same: enumerate USB devices,
// look for the Apple vendor ID with an iPod-Video product ID, and
// query its mode (Apple disk mode / Apple OS / our firmware in
// update mode). Per-platform code lives in detect_<os>.go files.
package ipod

import "errors"

// AppleVendorID is the USB vendor ID Apple uses for iPods.
const AppleVendorID = 0x05AC

// Known iPod Video product IDs. The set is small; Apple used a
// different PID per model.
const (
	ProductIDIPodVideo30  = 0x1209 // 30 GB iPod Video (5G)
	ProductIDIPodVideo60  = 0x120A // 60/80 GB iPod Video (5.5G)
	ProductIDIPodVideoDFU = 0x1224 // recovery/DFU mode
)

// Generation identifies the iPod hardware generation.
type Generation int

const (
	GenUnknown Generation = iota
	Gen5_30               // 30 GB
	Gen5_5                // 60/80 GB ("5.5G")
)

func (g Generation) String() string {
	switch g {
	case Gen5_30:
		return "5G (30 GB)"
	case Gen5_5:
		return "5.5G (60/80 GB)"
	default:
		return "unknown"
	}
}

// Mode describes what firmware (and which boot mode) is currently on the device.
type Mode int

const (
	ModeUnknown   Mode = iota
	ModeAppleOS        // booted into Apple's firmware, USB sync mode
	ModeAppleDisk      // Apple disk mode (Select+Play recovery)
	ModeOurUpdate      // our firmware, in "safe to update" screen
	ModeRockbox        // Rockbox firmware
)

func (m Mode) String() string {
	switch m {
	case ModeAppleOS:
		return "apple-os"
	case ModeAppleDisk:
		return "apple-disk-mode"
	case ModeOurUpdate:
		return "core-update-mode"
	case ModeRockbox:
		return "rockbox"
	default:
		return "unknown"
	}
}

// Device describes a single connected iPod.
type Device struct {
	Generation  Generation
	Mode        Mode
	CapacityGB  int
	Serial      string
	BlockDevice string // OS-specific raw device path: /dev/sdX, /dev/diskN, \\.\PhysicalDriveN
	MountPoint  string // FAT-partition mount point, if mounted
}

// ErrNoDevice is returned when no iPod is connected.
var ErrNoDevice = errors.New("no iPod found")

// ErrMultipleDevices is returned when more than one iPod is connected
// and the caller didn't specify which to use.
var ErrMultipleDevices = errors.New("multiple iPods found; specify which one")

// Detect returns the connected iPod, if any. The actual implementation
// lives in detect_<os>.go.
func Detect() (Device, error) {
	return detect()
}
