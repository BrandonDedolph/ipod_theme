package cli

import (
	"bytes"
	"errors"
	"io"
	"math/rand"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/firmware"
)

// runCore drives the real command tree the way a user would, so the
// tests cover flag wiring and not just the helper functions.
func runCore(t *testing.T, args ...string) (stdout, stderr string, err error) {
	t.Helper()
	root := Root()
	var outBuf, errBuf bytes.Buffer
	root.SetArgs(args)
	root.SetOut(&outBuf)
	root.SetErr(&errBuf)
	err = root.Execute()
	return outBuf.String(), errBuf.String(), err
}

// plausibleImage returns deterministic bytes that pass validatePackImage:
// large enough, not uniform, and starting with an ARM branch.
func plausibleImage(n int) []byte {
	b := make([]byte, n)
	r := rand.New(rand.NewSource(1))
	r.Read(b)
	// 0xEA00000E, little-endian: b <somewhere>.
	b[0], b[1], b[2], b[3] = 0x0E, 0x00, 0x00, 0xEA
	return b
}

func writeTemp(t *testing.T, dir, name string, data []byte) string {
	t.Helper()
	p := filepath.Join(dir, name)
	if err := os.WriteFile(p, data, 0o644); err != nil {
		t.Fatalf("write %s: %v", p, err)
	}
	return p
}

func TestFirmwarePackRoundTrip(t *testing.T) {
	dir := t.TempDir()
	image := plausibleImage(8192)
	in := writeTemp(t, dir, "core.bin", image)
	out := filepath.Join(dir, "core.ipod")

	if _, _, err := runCore(t, "firmware", "pack", in, "--out", out); err != nil {
		t.Fatalf("pack: %v", err)
	}
	packed, err := os.ReadFile(out)
	if err != nil {
		t.Fatalf("read packed: %v", err)
	}
	if len(packed) != len(image)+firmware.IPodFileHeaderSize {
		t.Fatalf("packed size = %d, want %d", len(packed), len(image)+firmware.IPodFileHeaderSize)
	}
	name, got, err := firmware.ReadIPodFile(bytes.NewReader(packed))
	if err != nil {
		t.Fatalf("ReadIPodFile: %v", err)
	}
	if name != firmware.ModelNameIPodVideo {
		t.Errorf("model = %q", string(name[:]))
	}
	if !bytes.Equal(got, image) {
		t.Error("round-tripped image differs from input")
	}

	back := filepath.Join(dir, "back.bin")
	if _, _, err := runCore(t, "firmware", "unpack", out, "--out", back); err != nil {
		t.Fatalf("unpack: %v", err)
	}
	got2, err := os.ReadFile(back)
	if err != nil {
		t.Fatalf("read unpacked: %v", err)
	}
	if !bytes.Equal(got2, image) {
		t.Error("unpacked image differs from input")
	}
}

// TestFirmwarePackOverwriteGuard covers the O_EXCL guard, which is the
// only user-facing safety behavior in the tool today.
func TestFirmwarePackOverwriteGuard(t *testing.T) {
	dir := t.TempDir()
	in := writeTemp(t, dir, "core.bin", plausibleImage(8192))
	out := writeTemp(t, dir, "core.ipod", []byte("PRE-EXISTING"))

	_, _, err := runCore(t, "firmware", "pack", in, "--out", out)
	if err == nil {
		t.Fatal("pack over an existing file succeeded; want a refusal")
	}
	if !strings.Contains(err.Error(), "already exists") {
		t.Errorf("error = %q, want it to mention the file already exists", err)
	}
	if b, _ := os.ReadFile(out); string(b) != "PRE-EXISTING" {
		t.Errorf("existing file was modified: %q", b)
	}

	if _, _, err := runCore(t, "firmware", "pack", in, "--out", out, "--force"); err != nil {
		t.Fatalf("pack --force: %v", err)
	}
	if b, _ := os.ReadFile(out); string(b) == "PRE-EXISTING" {
		t.Error("--force did not overwrite")
	}
}

func TestFirmwareUnpackOverwriteGuard(t *testing.T) {
	dir := t.TempDir()
	in := writeTemp(t, dir, "core.bin", plausibleImage(8192))
	packed := filepath.Join(dir, "core.ipod")
	if _, _, err := runCore(t, "firmware", "pack", in, "--out", packed); err != nil {
		t.Fatalf("pack: %v", err)
	}
	out := writeTemp(t, dir, "back.bin", []byte("PRE-EXISTING"))

	_, _, err := runCore(t, "firmware", "unpack", packed, "--out", out)
	if err == nil || !strings.Contains(err.Error(), "already exists") {
		t.Fatalf("unpack over an existing file: err = %v, want a refusal", err)
	}
	if b, _ := os.ReadFile(out); string(b) != "PRE-EXISTING" {
		t.Errorf("existing file was modified: %q", b)
	}
}

func TestFirmwarePackRejectsImplausibleInput(t *testing.T) {
	uniform := make([]byte, 8192) // all zero
	tests := []struct {
		name    string
		image   []byte
		wantErr string
	}{
		{"empty", nil, "is empty"},
		{"truncated", plausibleImage(64)[:64], "below the"},
		{"all zeroes", uniform, "nothing but"},
		{"all 0xFF", bytes.Repeat([]byte{0xFF}, 8192), "nothing but"},
		{"oversized", make([]byte, maxPackImageBytes+1), "above the"},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			dir := t.TempDir()
			in := writeTemp(t, dir, "core.bin", tc.image)
			out := filepath.Join(dir, "core.ipod")
			_, _, err := runCore(t, "firmware", "pack", in, "--out", out)
			if err == nil {
				t.Fatal("pack succeeded; want a refusal")
			}
			if !strings.Contains(err.Error(), tc.wantErr) {
				t.Errorf("error = %q, want it to contain %q", err, tc.wantErr)
			}
			if _, statErr := os.Stat(out); statErr == nil {
				t.Error("a rejected pack still created the output file")
			}
		})
	}
}

func TestFirmwarePackWarnsOnNonBranchEntry(t *testing.T) {
	dir := t.TempDir()
	image := plausibleImage(8192)
	// 0xE321F0D3 — MSR CPSR_c, the first word of a stock rockbox.ipod.
	image[0], image[1], image[2], image[3] = 0xD3, 0xF0, 0x21, 0xE3
	in := writeTemp(t, dir, "core.bin", image)
	out := filepath.Join(dir, "core.ipod")

	_, stderr, err := runCore(t, "firmware", "pack", in, "--out", out)
	if err != nil {
		t.Fatalf("pack: %v (a non-branch entry must warn, not fail)", err)
	}
	if !strings.Contains(stderr, "warning") {
		t.Errorf("stderr = %q, want a warning about the entry word", stderr)
	}
}

// TestWriteFileAtomicKeepsPreviousOnFailure is the regression test for
// the truncating --force write: a failure part-way through must leave
// the previous file byte-identical and no temp files behind.
func TestWriteFileAtomicKeepsPreviousOnFailure(t *testing.T) {
	dir := t.TempDir()
	out := writeTemp(t, dir, "core.ipod", []byte("PREVIOUS GOOD IMAGE"))

	boom := errors.New("simulated ENOSPC")
	err := writeFileAtomic(out, true, func(w io.Writer) error {
		// Write a valid-looking header, then fail — exactly the shape
		// of the hazard: header on disk, image never written.
		if _, werr := w.Write([]byte("\x00\x00\x00\x00ipvd")); werr != nil {
			return werr
		}
		return boom
	}, nil)
	if !errors.Is(err, boom) {
		t.Fatalf("err = %v, want %v", err, boom)
	}
	if b, _ := os.ReadFile(out); string(b) != "PREVIOUS GOOD IMAGE" {
		t.Errorf("previous file was clobbered: %q", b)
	}
	assertNoTempFiles(t, dir)
}

func TestWriteFileAtomicKeepsPreviousOnVerifyFailure(t *testing.T) {
	dir := t.TempDir()
	out := writeTemp(t, dir, "core.ipod", []byte("PREVIOUS GOOD IMAGE"))

	err := writeFileAtomic(out, true,
		func(w io.Writer) error { _, err := w.Write([]byte("new bytes")); return err },
		func([]byte) error { return errors.New("read-back verification failed") })
	if err == nil {
		t.Fatal("verify failure did not surface as an error")
	}
	if b, _ := os.ReadFile(out); string(b) != "PREVIOUS GOOD IMAGE" {
		t.Errorf("previous file was clobbered: %q", b)
	}
	assertNoTempFiles(t, dir)
}

// TestWriteFileAtomicRemovesClaimOnFailure: without --force we take the
// output name with O_EXCL before writing. If the write then fails, the
// zero-byte placeholder must not survive.
func TestWriteFileAtomicRemovesClaimOnFailure(t *testing.T) {
	dir := t.TempDir()
	out := filepath.Join(dir, "core.ipod")

	err := writeFileAtomic(out, false,
		func(io.Writer) error { return errors.New("boom") }, nil)
	if err == nil {
		t.Fatal("want an error")
	}
	if _, statErr := os.Stat(out); statErr == nil {
		t.Error("zero-byte placeholder survived a failed write")
	}
	assertNoTempFiles(t, dir)
}

func assertNoTempFiles(t *testing.T, dir string) {
	t.Helper()
	ents, err := os.ReadDir(dir)
	if err != nil {
		t.Fatalf("readdir: %v", err)
	}
	for _, e := range ents {
		if strings.Contains(e.Name(), ".tmp-") {
			t.Errorf("temp file left behind: %s", e.Name())
		}
	}
}

func TestFirmwareUnpackChecksumMismatch(t *testing.T) {
	dir := t.TempDir()
	image := plausibleImage(8192)
	in := writeTemp(t, dir, "core.bin", image)
	packed := filepath.Join(dir, "core.ipod")
	if _, _, err := runCore(t, "firmware", "pack", in, "--out", packed); err != nil {
		t.Fatalf("pack: %v", err)
	}
	// Flip a payload byte so the stored checksum no longer matches.
	b, err := os.ReadFile(packed)
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	b[firmware.IPodFileHeaderSize+16] ^= 0xFF
	if err := os.WriteFile(packed, b, 0o644); err != nil {
		t.Fatalf("write: %v", err)
	}

	out := filepath.Join(dir, "back.bin")
	_, _, err = runCore(t, "firmware", "unpack", packed, "--out", out)
	if !errors.Is(err, firmware.ErrIPodChecksumMismatch) {
		t.Fatalf("unpack of a corrupt image: err = %v, want a checksum mismatch", err)
	}
	if _, statErr := os.Stat(out); statErr == nil {
		t.Error("unpack wrote an output file despite the checksum mismatch")
	}

	// --ignore-checksum is the recovery path: it must write the bytes
	// and say loudly that they are unverified.
	_, stderr, err := runCore(t, "firmware", "unpack", packed, "--out", out, "--ignore-checksum")
	if err != nil {
		t.Fatalf("unpack --ignore-checksum: %v", err)
	}
	if !strings.Contains(stderr, "WARNING") {
		t.Errorf("stderr = %q, want a loud warning", stderr)
	}
	got, err := os.ReadFile(out)
	if err != nil {
		t.Fatalf("read recovered image: %v", err)
	}
	if len(got) != len(image) {
		t.Errorf("recovered %d bytes, want %d", len(got), len(image))
	}
	if bytes.Equal(got, image) {
		t.Error("recovered image should carry the corruption we introduced")
	}
}

func TestFirmwareUnpackShortFile(t *testing.T) {
	dir := t.TempDir()
	short := writeTemp(t, dir, "short.ipod", []byte{1, 2, 3})
	out := filepath.Join(dir, "back.bin")
	_, _, err := runCore(t, "firmware", "unpack", short, "--out", out, "--ignore-checksum")
	if !errors.Is(err, firmware.ErrShortIPodFile) {
		t.Fatalf("err = %v, want ErrShortIPodFile", err)
	}
	if _, statErr := os.Stat(out); statErr == nil {
		t.Error("unpack of a truncated file still created the output")
	}
}

func TestPackRequiresOut(t *testing.T) {
	dir := t.TempDir()
	in := writeTemp(t, dir, "core.bin", plausibleImage(8192))
	if _, _, err := runCore(t, "firmware", "pack", in); err == nil {
		t.Fatal("pack without --out succeeded; want an error")
	}
}

func TestLooksLikeARMBranch(t *testing.T) {
	tests := []struct {
		name string
		word uint32
		want bool
	}{
		{"b forward (our reset vector)", 0xEA00000E, true},
		{"bl", 0xEB000010, true},
		{"conditional b", 0x1A000004, true},
		{"msr (rockbox reset vector)", 0xE321F0D3, false},
		{"ldr pc", 0xE59FF014, false},
		{"unconditional space", 0xFA000000, false},
		{"zero", 0x00000000, false},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			b := []byte{byte(tc.word), byte(tc.word >> 8), byte(tc.word >> 16), byte(tc.word >> 24)}
			if got := looksLikeARMBranch(b); got != tc.want {
				t.Errorf("looksLikeARMBranch(%#08x) = %v, want %v", tc.word, got, tc.want)
			}
		})
	}
	if looksLikeARMBranch([]byte{0xEA, 0x00}) {
		t.Error("a sub-word image must not decode as a branch")
	}
}

func TestUniformByte(t *testing.T) {
	if _, ok := uniformByte(nil); ok {
		t.Error("empty slice reported uniform")
	}
	if b, ok := uniformByte(bytes.Repeat([]byte{0xFF}, 16)); !ok || b != 0xFF {
		t.Errorf("uniformByte(0xFF...) = %#02x, %v", b, ok)
	}
	if _, ok := uniformByte([]byte{1, 1, 1, 2}); ok {
		t.Error("mixed slice reported uniform")
	}
}
