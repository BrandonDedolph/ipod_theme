package firmware

import (
	"bytes"
	"encoding/binary"
	"os"
	"testing"
)

func TestChecksum_EmptyImage(t *testing.T) {
	// Empty image: sum is just the model seed.
	//
	// Note this test, and every other one in this file, only proves the
	// arithmetic is self-consistent — they assert 0x05 against 0x05. The
	// question of whether 0x05 is the *right* seed is answered only by
	// TestChecksumGoldenVector below.
	got := Checksum(ModelIPodVideo, nil)
	want := uint32(0x05)
	if got != want {
		t.Errorf("Checksum(empty) = %#x, want %#x", got, want)
	}
}

// TestChecksumGoldenVector re-derives the model seed from a real
// .ipod file rather than restating our own assumption.
//
// Point it at any genuine .ipod image:
//
//	CORE_GOLDEN_IPOD=/path/to/rockbox.ipod go test ./internal/firmware/
//
// The most useful vector is one this project did not produce — a
// Rockbox build for the iPod Video, say — because that makes it
// independent corroboration rather than a round trip through our own
// encoder. The file is read from wherever the developer has it; nothing
// third-party is vendored into the repo, which also keeps a GPL-2
// artifact out of an Apache-2.0 tree.
//
// Verified this way at the time of writing (see the provenance note on
// ModelIPodVideo in checksum.go): a Rockbox rockbox.ipod with a
// 777976-byte payload and this project's own core.ipod with a
// 225388-byte payload both yield a seed of exactly 5 for model "ipvd".
func TestChecksumGoldenVector(t *testing.T) {
	path := os.Getenv("CORE_GOLDEN_IPOD")
	if path == "" {
		t.Skip("set CORE_GOLDEN_IPOD=/path/to/a/real.ipod to check the model seed " +
			"against a golden vector (a third-party build, e.g. rockbox.ipod, is best)")
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read golden vector: %v", err)
	}
	if len(data) <= IPodFileHeaderSize {
		t.Fatalf("%s is too small to be a .ipod file (%d bytes)", path, len(data))
	}
	stored := binary.BigEndian.Uint32(data[0:4])
	var name ModelName
	copy(name[:], data[4:8])
	image := data[IPodFileHeaderSize:]

	want, ok := ModelNumForName(name)
	if !ok {
		t.Skipf("%s has model name %q, which we have no seed for", path, string(name[:]))
	}
	// Derive the seed the file implies and compare with our constant.
	derived := stored - Checksum(0, image)
	if ModelNum(derived) != want {
		t.Errorf("%s (model %q, %d payload bytes): file implies seed %#x, "+
			"but ModelNumForName says %#x — one of them is wrong, and the constant "+
			"is the one that has never been exercised on hardware",
			path, string(name[:]), len(image), derived, want)
	}
	if got := Checksum(want, image); got != stored {
		t.Errorf("%s: computed checksum %#08x, file stores %#08x", path, got, stored)
	}
}

func TestChecksum_KnownInput(t *testing.T) {
	// Hand-computed: 0x05 + 0x10 + 0x20 + 0x30 = 0x65.
	got := Checksum(ModelIPodVideo, []byte{0x10, 0x20, 0x30})
	want := uint32(0x65)
	if got != want {
		t.Errorf("Checksum([16,32,48]) = %#x, want %#x", got, want)
	}
}

func TestChecksum_LargeNoWrap(t *testing.T) {
	// 256 KB of 0xFF bytes: 256 * 1024 * 0xFF = 0x3FC0000.
	// Plus model seed (0x05) = 0x3FC0005. No wrap at 32-bit.
	data := bytes.Repeat([]byte{0xFF}, 256*1024)
	got := Checksum(ModelIPodVideo, data)
	want := uint32(0x3FC0005)
	if got != want {
		t.Errorf("Checksum(256K of 0xFF) = %#x, want %#x", got, want)
	}
}

func TestChecksum_Wraps32Bit(t *testing.T) {
	// Feed enough 0xFF bytes to overflow uint32 by 0xFF and confirm wrap to 0xFE.
	// Size chosen so size*0xFF == 0xFFFFFFFF + 0xFF.
	const size = (0xFFFF_FFFF / 0xFF) + 1
	data := bytes.Repeat([]byte{0xFF}, size)
	got := Checksum(0, data) // seed = 0; the wrap is the only thing under test
	want := uint32(0xFE)
	if got != want {
		t.Errorf("Checksum(wrap) = %#x, want %#x", got, want)
	}
}

func TestChecksum_DifferentModelSeeds(t *testing.T) {
	data := []byte{0x42}
	if Checksum(ModelIPodVideo, data) == Checksum(ModelIPodNano, data) {
		t.Error("seeds should produce different sums for non-empty data")
	}
}

// Sanity check: confirm the algorithm is order-independent at the byte
// level (additive). This guarantee lets us checksum streamed input
// without buffering the whole image.
func TestChecksum_OrderIndependent(t *testing.T) {
	a := []byte{0x11, 0x22, 0x33, 0x44}
	b := []byte{0x44, 0x33, 0x22, 0x11}
	if Checksum(ModelIPodVideo, a) != Checksum(ModelIPodVideo, b) {
		t.Error("byte-order shouldn't change the additive sum")
	}
}

func BenchmarkChecksum_1MB(b *testing.B) {
	data := bytes.Repeat([]byte{0xAB}, 1<<20)
	b.SetBytes(int64(len(data)))
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		_ = Checksum(ModelIPodVideo, data)
	}
}
