// Firmware-partition directory support.
//
// STATUS: INCOMPLETE SCAFFOLDING. Nothing outside this package's tests
// calls any of it, and it does not yet cover everything a partition
// write would need. Specifically:
//
//   - IMPLEMENTED: the 40-byte directory entry codec, the directory
//     marker at 0x100, and the directory locator (format version at
//     0x10A, first entry at LE32@0x104 + 0x200) per
//     core/docs/hw/08-boot-dock.md.
//
//   - NOT IMPLEMENTED: the Apple partition preamble. The first 512
//     bytes of partition 0 carry the Apple copyright banner, the boot
//     ROM refuses to load a partition without it, and any install path
//     must preserve it byte-exact. This package neither parses nor
//     validates it, and cannot supply one — the only copy of a given
//     device's preamble is the one on that device, which is why the
//     install safety checklist (internal/cli/install.go) requires a
//     full partition backup before any write.
//
// Do not describe this file as "done" until the preamble is handled and
// something outside the tests uses it.

package firmware

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
)

// DirectoryEntry is one row of the iPod firmware-partition image
// directory — 40 bytes. See core/docs/hw/08-boot-dock.md for the full
// layout.
type DirectoryEntry struct {
	ContainerID [4]byte // "!ATA" or "DNAN"
	ImageType   [4]byte // "soso" (OSOS), "crsr" (RSRC), etc. — stored byte-reversed of the logical name
	ImageID     uint32
	DevOffset   uint32 // bytes from start of firmware partition
	Length      uint32 // image length in bytes
	LoadAddr    uint32 // DRAM load address
	EntryOffset uint32 // entry point within the image (0 = start)
	Checksum    uint32 // additive checksum (model + sum-of-bytes)
	Version     uint32
	LoadAddr2   uint32 // secondary load address
}

// LogicalImageType returns the byte-reversed image-type tag.
//
// On disk the four bytes spell e.g. "soso"; reversed, "osos" (which
// Apple/iPodLinux docs conventionally write uppercase as "OSOS"). The
// returned string is lowercase to match the on-disk encoding exactly.
func (e *DirectoryEntry) LogicalImageType() string {
	return string([]byte{e.ImageType[3], e.ImageType[2], e.ImageType[1], e.ImageType[0]})
}

// IsOSOS reports whether this entry is the main OS image — the one
// our bootloader takes over.
func (e *DirectoryEntry) IsOSOS() bool {
	// "soso" reversed = "OSOS"
	return e.ImageType == [4]byte{'s', 'o', 's', 'o'}
}

// ReadDirectoryEntry decodes one 40-byte entry from r.
//
// Multi-byte fields are little-endian per the iPod boot ROM expectation.
func ReadDirectoryEntry(r io.Reader) (DirectoryEntry, error) {
	var raw [40]byte
	if _, err := io.ReadFull(r, raw[:]); err != nil {
		return DirectoryEntry{}, err
	}

	var e DirectoryEntry
	copy(e.ContainerID[:], raw[0:4])
	copy(e.ImageType[:], raw[4:8])
	e.ImageID = binary.LittleEndian.Uint32(raw[8:12])
	e.DevOffset = binary.LittleEndian.Uint32(raw[12:16])
	e.Length = binary.LittleEndian.Uint32(raw[16:20])
	e.LoadAddr = binary.LittleEndian.Uint32(raw[20:24])
	e.EntryOffset = binary.LittleEndian.Uint32(raw[24:28])
	e.Checksum = binary.LittleEndian.Uint32(raw[28:32])
	e.Version = binary.LittleEndian.Uint32(raw[32:36])
	e.LoadAddr2 = binary.LittleEndian.Uint32(raw[36:40])
	return e, nil
}

// WriteDirectoryEntry encodes e into w.
func WriteDirectoryEntry(w io.Writer, e DirectoryEntry) error {
	var raw [40]byte
	copy(raw[0:4], e.ContainerID[:])
	copy(raw[4:8], e.ImageType[:])
	binary.LittleEndian.PutUint32(raw[8:12], e.ImageID)
	binary.LittleEndian.PutUint32(raw[12:16], e.DevOffset)
	binary.LittleEndian.PutUint32(raw[16:20], e.Length)
	binary.LittleEndian.PutUint32(raw[20:24], e.LoadAddr)
	binary.LittleEndian.PutUint32(raw[24:28], e.EntryOffset)
	binary.LittleEndian.PutUint32(raw[28:32], e.Checksum)
	binary.LittleEndian.PutUint32(raw[32:36], e.Version)
	binary.LittleEndian.PutUint32(raw[36:40], e.LoadAddr2)
	_, err := w.Write(raw[:])
	return err
}

// DirectoryMarker is the magic bytes that mark the start of the image
// directory (at byte offset 0x100 of the firmware partition). It's the
// reversed ASCII of "[hi]".
var DirectoryMarker = [4]byte{']', 'i', 'h', '['}

// ErrBadDirectoryMarker is returned when the magic bytes don't match.
var ErrBadDirectoryMarker = errors.New("firmware partition: directory marker mismatch")

// CheckDirectoryMarker verifies the 4 bytes are the expected marker.
// Returns ErrBadDirectoryMarker if not.
func CheckDirectoryMarker(b [4]byte) error {
	if b != DirectoryMarker {
		return ErrBadDirectoryMarker
	}
	return nil
}

// Byte offsets within the firmware partition, per
// core/docs/hw/08-boot-dock.md.
const (
	// DirectoryMarkerOffset is where the "]ih[" marker lives.
	DirectoryMarkerOffset = 0x100
	// DirectoryStartPtrOffset holds an LE32 that, plus DirectoryStartBias,
	// gives the offset of the first directory entry.
	DirectoryStartPtrOffset = 0x104
	// DirectoryVersionOffset holds the LE16 directory format version.
	DirectoryVersionOffset = 0x10A
	// DirectoryStartBias is added to the LE32 at DirectoryStartPtrOffset.
	DirectoryStartBias = 0x200
	// directoryHeaderEnd is the first byte past the fields we read.
	directoryHeaderEnd = 0x10C
)

// KnownDirectoryVersions are the directory format versions the iPod 5G
// boot ROM recognizes. Only 2 and 3 are seen in the wild for this device.
var KnownDirectoryVersions = []uint16{2, 3}

// ErrShortPartition is returned when the supplied bytes don't reach the
// directory header.
var ErrShortPartition = errors.New("firmware partition: too short to contain a directory header")

// ErrUnknownDirectoryVersion is returned for a directory format version
// outside KnownDirectoryVersions.
var ErrUnknownDirectoryVersion = errors.New("firmware partition: unrecognized directory format version")

// DirectoryLocator is the result of parsing the directory header: where
// the image directory starts and which format version it is in.
type DirectoryLocator struct {
	// Version is the LE16 directory format version at 0x10A.
	Version uint16
	// Start is the absolute byte offset, within the firmware partition,
	// of the first 40-byte directory entry.
	Start uint32
}

// ReadDirectoryLocator parses the directory header out of the leading
// bytes of a firmware partition. `part` needs to contain at least the
// first directoryHeaderEnd bytes; passing the whole partition is fine.
//
// It verifies the marker, reads the LE32 at 0x104 and adds the 0x200
// bias to get the directory start, and reads the LE16 version at 0x10A.
// An unrecognized version is reported but the locator is still returned,
// so a caller inspecting an odd device can see what it found.
func ReadDirectoryLocator(part []byte) (DirectoryLocator, error) {
	if len(part) < directoryHeaderEnd {
		return DirectoryLocator{}, fmt.Errorf("%w (have %d bytes, need %d)",
			ErrShortPartition, len(part), directoryHeaderEnd)
	}
	var marker [4]byte
	copy(marker[:], part[DirectoryMarkerOffset:DirectoryMarkerOffset+4])
	if err := CheckDirectoryMarker(marker); err != nil {
		return DirectoryLocator{}, fmt.Errorf("%w (found %q at %#x)",
			err, string(marker[:]), DirectoryMarkerOffset)
	}
	start := binary.LittleEndian.Uint32(part[DirectoryStartPtrOffset : DirectoryStartPtrOffset+4])
	loc := DirectoryLocator{
		Version: binary.LittleEndian.Uint16(part[DirectoryVersionOffset : DirectoryVersionOffset+2]),
		Start:   start + DirectoryStartBias,
	}
	if start+DirectoryStartBias < start {
		return DirectoryLocator{}, fmt.Errorf(
			"firmware partition: directory start pointer %#x overflows when biased by %#x",
			start, DirectoryStartBias)
	}
	for _, v := range KnownDirectoryVersions {
		if loc.Version == v {
			return loc, nil
		}
	}
	return loc, fmt.Errorf("%w: %d (known: %v)",
		ErrUnknownDirectoryVersion, loc.Version, KnownDirectoryVersions)
}
