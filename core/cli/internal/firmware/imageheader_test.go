package firmware

import (
	"bytes"
	"encoding/binary"
	"errors"
	"testing"
)

// fakePartitionHead builds the first 0x200 bytes of a firmware
// partition with a directory header, per core/docs/hw/08-boot-dock.md.
func fakePartitionHead(marker string, startPtr uint32, version uint16) []byte {
	b := make([]byte, 0x200)
	copy(b[DirectoryMarkerOffset:], marker)
	binary.LittleEndian.PutUint32(b[DirectoryStartPtrOffset:], startPtr)
	binary.LittleEndian.PutUint16(b[DirectoryVersionOffset:], version)
	return b
}

func TestReadDirectoryLocator(t *testing.T) {
	b := fakePartitionHead("]ih[", 0x4000, 3)
	loc, err := ReadDirectoryLocator(b)
	if err != nil {
		t.Fatalf("ReadDirectoryLocator: %v", err)
	}
	if loc.Version != 3 {
		t.Errorf("Version = %d, want 3", loc.Version)
	}
	// Directory start is the LE32 at 0x104 plus the 0x200 bias.
	if want := uint32(0x4000 + 0x200); loc.Start != want {
		t.Errorf("Start = %#x, want %#x", loc.Start, want)
	}
}

func TestReadDirectoryLocatorBadMarker(t *testing.T) {
	b := fakePartitionHead("hi!!", 0x4000, 2)
	if _, err := ReadDirectoryLocator(b); !errors.Is(err, ErrBadDirectoryMarker) {
		t.Fatalf("err = %v, want ErrBadDirectoryMarker", err)
	}
}

func TestReadDirectoryLocatorShort(t *testing.T) {
	if _, err := ReadDirectoryLocator(make([]byte, 0x100)); !errors.Is(err, ErrShortPartition) {
		t.Fatalf("err = %v, want ErrShortPartition", err)
	}
	if _, err := ReadDirectoryLocator(nil); !errors.Is(err, ErrShortPartition) {
		t.Fatalf("err = %v, want ErrShortPartition", err)
	}
}

func TestReadDirectoryLocatorUnknownVersion(t *testing.T) {
	// Version 7 is not something the 5G boot ROM recognizes, but the
	// locator is still returned so a human can see what was found.
	b := fakePartitionHead("]ih[", 0x1000, 7)
	loc, err := ReadDirectoryLocator(b)
	if !errors.Is(err, ErrUnknownDirectoryVersion) {
		t.Fatalf("err = %v, want ErrUnknownDirectoryVersion", err)
	}
	if loc.Version != 7 || loc.Start != 0x1200 {
		t.Errorf("locator = %+v, want version 7 start 0x1200", loc)
	}
}

func TestReadDirectoryLocatorAcceptsKnownVersions(t *testing.T) {
	for _, v := range KnownDirectoryVersions {
		if _, err := ReadDirectoryLocator(fakePartitionHead("]ih[", 0, v)); err != nil {
			t.Errorf("version %d rejected: %v", v, err)
		}
	}
}

func TestDirectoryEntryRoundtrip(t *testing.T) {
	original := DirectoryEntry{
		ContainerID: [4]byte{'!', 'A', 'T', 'A'},
		ImageType:   [4]byte{'s', 'o', 's', 'o'}, // "OSOS"
		ImageID:     0xDEADBEEF,
		DevOffset:   0x10000,
		Length:      0x80000,
		LoadAddr:    0x10000000,
		EntryOffset: 0x40,
		Checksum:    0x12345678,
		Version:     2,
		LoadAddr2:   0x10000040,
	}

	var buf bytes.Buffer
	if err := WriteDirectoryEntry(&buf, original); err != nil {
		t.Fatalf("WriteDirectoryEntry: %v", err)
	}
	if got, want := buf.Len(), 40; got != want {
		t.Fatalf("encoded length = %d, want %d", got, want)
	}

	decoded, err := ReadDirectoryEntry(&buf)
	if err != nil {
		t.Fatalf("ReadDirectoryEntry: %v", err)
	}
	if decoded != original {
		t.Errorf("roundtrip mismatch:\n got %#v\nwant %#v", decoded, original)
	}
}

func TestDirectoryEntry_LogicalImageType(t *testing.T) {
	// On-disk "soso" reversed = "osos" (lowercase to match the on-disk bytes).
	e := DirectoryEntry{ImageType: [4]byte{'s', 'o', 's', 'o'}}
	if got := e.LogicalImageType(); got != "osos" {
		t.Errorf("LogicalImageType = %q, want %q", got, "osos")
	}
}

func TestDirectoryEntry_IsOSOS(t *testing.T) {
	osos := DirectoryEntry{ImageType: [4]byte{'s', 'o', 's', 'o'}}
	rsrc := DirectoryEntry{ImageType: [4]byte{'c', 'r', 's', 'r'}}
	if !osos.IsOSOS() {
		t.Error("OSOS entry should report IsOSOS()")
	}
	if rsrc.IsOSOS() {
		t.Error("RSRC entry should not report IsOSOS()")
	}
}

func TestDirectoryMarker(t *testing.T) {
	good := [4]byte{']', 'i', 'h', '['}
	bad := [4]byte{'h', 'i', '!', '!'}

	if err := CheckDirectoryMarker(good); err != nil {
		t.Errorf("CheckDirectoryMarker(good) returned %v", err)
	}
	if err := CheckDirectoryMarker(bad); err == nil {
		t.Error("CheckDirectoryMarker(bad) returned nil, want error")
	}
}

func TestReadDirectoryEntry_Truncated(t *testing.T) {
	// Only 20 bytes — too short.
	_, err := ReadDirectoryEntry(bytes.NewReader(make([]byte, 20)))
	if err == nil {
		t.Error("expected error on truncated read, got nil")
	}
}
