package tagcache

import (
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"
)

// TestScanRecursive verifies the scanner descends into artist/album
// subdirectories — the layout every real music library uses — and
// returns the union of all .flac/.mp3 files. Empty files are fine:
// readTags fails silently and the basename-as-title fallback kicks in,
// which is enough to confirm the file was discovered.
func TestScanRecursive(t *testing.T) {
	root := t.TempDir()
	want := []string{
		filepath.Join(root, "ArtistA", "Album1", "01 Track.mp3"),
		filepath.Join(root, "ArtistA", "Album1", "02 Track.flac"),
		filepath.Join(root, "ArtistB", "Album1", "Disc 1", "deep.mp3"),
		filepath.Join(root, "loose.MP3"), // case-insensitive ext
	}
	for _, p := range want {
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			t.Fatalf("mkdir: %v", err)
		}
		if err := os.WriteFile(p, nil, 0o644); err != nil {
			t.Fatalf("write %s: %v", p, err)
		}
	}
	// Drop a non-audio sibling to make sure it's filtered out.
	if err := os.WriteFile(filepath.Join(root, "ArtistA", "cover.jpg"), nil, 0o644); err != nil {
		t.Fatalf("write cover: %v", err)
	}

	songs, err := Scan(root)
	if err != nil {
		t.Fatalf("Scan: %v", err)
	}
	if len(songs) != len(want) {
		t.Fatalf("want %d songs, got %d: %v", len(want), len(songs), songs)
	}
	got := make([]string, len(songs))
	for i, s := range songs {
		got[i] = s.Path
	}
	sort.Strings(got)
	sort.Strings(want)
	for i := range want {
		if got[i] != want[i] {
			t.Errorf("song[%d] path: want %q got %q", i, want[i], got[i])
		}
	}
}

// TestPrimaryArtist locks the rules used to fold "Zach Bryan, The
// Lumineers"-style combo entries into a single primary. Real
// libraries credit features inside the ARTIST tag, and the Artists
// list reads as noise without this collapse.
func TestPrimaryArtist(t *testing.T) {
	cases := []struct{ in, want string }{
		{"", ""},
		{"  ", ""},
		{"Daniel Caesar", "Daniel Caesar"},
		{"Zach Bryan, The Lumineers", "Zach Bryan"},
		{"Skrillex, Justin Bieber & Diplo", "Skrillex"},
		{"A & B", "A"},
		{"Daft Punk feat. Pharrell", "Daft Punk"},
		{"Daft Punk Feat. Pharrell", "Daft Punk"},
		{"Daft Punk FEAT. Pharrell", "Daft Punk"},
		{"Travis Scott featuring Drake", "Travis Scott"},
		{"Drake ft. Future", "Drake"},
		{"Drake ft Future", "Drake"},
		/* "feat" embedded in a name (no leading space) shouldn't trigger. */
		{"Featherwait", "Featherwait"},
		/* Bare comma (no space) — common in some ID3 tagging tools. */
		{"Justin Bieber,Kehlani", "Justin Bieber"},
		{"Justin Bieber,Post Malone,Clever", "Justin Bieber"},
		/* Whitespace around the result is trimmed. */
		{"  Aphex Twin  ", "Aphex Twin"},
	}
	for _, c := range cases {
		got := primaryArtist(c.in)
		if got != c.want {
			t.Errorf("primaryArtist(%q) = %q, want %q", c.in, got, c.want)
		}
	}
}

// TestDedupeSongs locks the same-track-different-format collapse:
// real iPod libraries often hold parallel FLAC + MP3 copies of an
// album in sibling folders, and we want one row per logical track
// with the lossless copy winning.
func TestDedupeSongs(t *testing.T) {
	in := []SongInfo{
		{Path: "/m/Aphex Twin/Drukqs/01.mp3", Title: "Avril 14th", Artist: "Aphex Twin", Album: "Drukqs"},
		{Path: "/m/FLAC/Aphex Twin/Drukqs/01.flac", Title: "Avril 14th", Artist: "Aphex Twin", Album: "Drukqs"},
		/* Different-album dupes don't collapse. */
		{Path: "/m/Aphex Twin/Selected/01.flac", Title: "Avril 14th", Artist: "Aphex Twin", Album: "Selected"},
		/* Two untagged files keep both rows (empty key would otherwise lump them). */
		{Path: "/m/random/a.mp3", Title: "a"},
		{Path: "/m/random/b.mp3", Title: "b"},
	}
	out := dedupeSongs(append([]SongInfo(nil), in...))
	if len(out) != 4 {
		t.Fatalf("want 4 rows after dedup, got %d: %+v", len(out), out)
	}
	/* The Drukqs row should be the .flac copy (lossless wins). */
	for _, s := range out {
		if s.Title == "Avril 14th" && s.Album == "Drukqs" {
			if !strings.HasSuffix(strings.ToLower(s.Path), ".flac") {
				t.Errorf("Drukqs/Avril 14th: lossless should win, got %s", s.Path)
			}
		}
	}
}

// TestScanRejectsFile guards the precondition: passing a regular file
// instead of a directory must error rather than silently scanning the
// file's parent.
func TestScanRejectsFile(t *testing.T) {
	f := filepath.Join(t.TempDir(), "x.mp3")
	if err := os.WriteFile(f, nil, 0o644); err != nil {
		t.Fatalf("write: %v", err)
	}
	if _, err := Scan(f); err == nil {
		t.Fatal("Scan on a regular file: want error, got nil")
	}
}
