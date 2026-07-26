package tagcache

import (
	"bytes"
	"encoding/binary"
	"reflect"
	"testing"
)

// TestRoundTrip exercises Build → Write → Read on a hand-crafted song
// list covering: tagged + untagged tracks, two artists, two albums,
// two genres, one composer with multiple tracks, and one shared cover
// art (so the dedup path is exercised).
func TestRoundTrip(t *testing.T) {
	jpegA := []byte{0xff, 0xd8, 0xff, 0xe0, 1, 2, 3} // pretend-JPEG
	jpegB := []byte{0xff, 0xd8, 0xff, 0xe1, 9, 9}

	in := []SongInfo{
		{Path: "/m/track1.flac", Title: "Apple", Artist: "Aphex Twin", Album: "Drukqs", Genre: "IDM", Composer: "Aphex Twin", ArtBytes: jpegA},
		{Path: "/m/track2.flac", Title: "Banana", Artist: "Aphex Twin", Album: "Drukqs", Genre: "IDM", Composer: "Aphex Twin", ArtBytes: jpegA},
		{Path: "/m/track3.flac", Title: "Cherry", Artist: "Brian Eno", Album: "Ambient", Genre: "Ambient", Composer: "", ArtBytes: jpegB},
		{Path: "/m/track4.flac", Title: "Durian", Artist: "", Album: "", Genre: "", Composer: "", ArtBytes: nil},
	}
	// Sort like Scan would.
	want := Build(append([]SongInfo(nil), in...))

	var buf bytes.Buffer
	if err := want.Write(&buf); err != nil {
		t.Fatalf("write: %v", err)
	}
	got, err := Read(buf.Bytes())
	if err != nil {
		t.Fatalf("read: %v", err)
	}

	checkStrings := func(label string, w, g []string) {
		t.Helper()
		if !reflect.DeepEqual(w, g) {
			t.Errorf("%s mismatch:\n want %v\n  got %v", label, w, g)
		}
	}
	checkStrings("uniq_artists", want.UniqArtists, got.UniqArtists)
	checkStrings("uniq_albums", want.UniqAlbums, got.UniqAlbums)
	checkStrings("uniq_genres", want.UniqGenres, got.UniqGenres)
	checkStrings("uniq_composers", want.UniqComposers, got.UniqComposers)

	if !reflect.DeepEqual(want.SongArtistIdx, got.SongArtistIdx) {
		t.Errorf("song_artist_idx mismatch")
	}
	if !reflect.DeepEqual(want.SongAlbumIdx, got.SongAlbumIdx) {
		t.Errorf("song_album_idx mismatch")
	}
	if !reflect.DeepEqual(want.SongGenreIdx, got.SongGenreIdx) {
		t.Errorf("song_genre_idx mismatch")
	}
	if !reflect.DeepEqual(want.SongComposerIdx, got.SongComposerIdx) {
		t.Errorf("song_composer_idx mismatch")
	}

	if !reflect.DeepEqual(want.ArtistGroups, got.ArtistGroups) {
		t.Errorf("artist_groups mismatch:\n want %v\n  got %v", want.ArtistGroups, got.ArtistGroups)
	}
	if !reflect.DeepEqual(want.AlbumGroups, got.AlbumGroups) {
		t.Errorf("album_groups mismatch")
	}
	if !reflect.DeepEqual(want.GenreGroups, got.GenreGroups) {
		t.Errorf("genre_groups mismatch")
	}
	if !reflect.DeepEqual(want.ComposerGroups, got.ComposerGroups) {
		t.Errorf("composer_groups mismatch")
	}

	for i := range want.Songs {
		if want.Songs[i].Title != got.Songs[i].Title {
			t.Errorf("song[%d] title: want %q got %q", i, want.Songs[i].Title, got.Songs[i].Title)
		}
		if want.Songs[i].Path != got.Songs[i].Path {
			t.Errorf("song[%d] path: want %q got %q", i, want.Songs[i].Path, got.Songs[i].Path)
		}
		if !bytes.Equal(want.Songs[i].ArtBytes, got.Songs[i].ArtBytes) {
			t.Errorf("song[%d] art bytes mismatch", i)
		}
	}

	// Spot-check dedup outcomes.
	if len(got.UniqArtists) != 2 {
		t.Errorf("expected 2 uniq artists, got %d (%v)", len(got.UniqArtists), got.UniqArtists)
	}
	if len(got.UniqComposers) != 1 {
		t.Errorf("expected 1 uniq composer, got %d (%v)", len(got.UniqComposers), got.UniqComposers)
	}
	// Aphex group should hold the two Aphex tracks (post-sort by title — Apple, Banana = global indices 0, 1).
	aphex := -1
	for i, a := range got.UniqArtists {
		if a == "Aphex Twin" {
			aphex = i
		}
	}
	if aphex < 0 || !reflect.DeepEqual(got.ArtistGroups[aphex], []uint32{0, 1}) {
		t.Errorf("aphex group: want [0 1] got %v (artists=%v)", got.ArtistGroups[aphex], got.UniqArtists)
	}

	// Art dedup: jpegA was used twice; the file should hold it only once,
	// so total art bytes len = len(jpegA) + len(jpegB).
	wantArtLen := len(jpegA) + len(jpegB)
	// Inspect via re-parse of the header.
	var hdr Header
	if err := binary.Read(bytes.NewReader(buf.Bytes()[:HeaderSize]), LE, &hdr); err != nil {
		t.Fatalf("re-read header: %v", err)
	}
	if int(hdr.ArtLen) != wantArtLen {
		t.Errorf("art blob len: want %d (deduped) got %d", wantArtLen, hdr.ArtLen)
	}
}

// TestCaseCollision locks the case-folding-dedup contract: two artist
// strings differing only by case must collapse to a single uniq slot,
// and the canonical spelling is the first one in title-sort order.
func TestCaseCollision(t *testing.T) {
	in := []SongInfo{
		// "Apple" sorts before "Banana", so the first-encountered
		// spelling of the case-folded artist is "Aphex Twin".
		{Path: "/a.flac", Title: "Apple", Artist: "Aphex Twin"},
		{Path: "/b.flac", Title: "Banana", Artist: "aphex twin"},
	}
	m := Build(in)
	if len(m.UniqArtists) != 1 || m.UniqArtists[0] != "Aphex Twin" {
		t.Fatalf("uniq artists: want [Aphex Twin] got %v", m.UniqArtists)
	}
	if m.SongArtistIdx[0] != 0 || m.SongArtistIdx[1] != 0 {
		t.Errorf("both songs should map to artist 0; got %v", m.SongArtistIdx)
	}
	if !reflect.DeepEqual(m.ArtistGroups[0], []uint32{0, 1}) {
		t.Errorf("artist 0 group: want [0 1] got %v", m.ArtistGroups[0])
	}
}

// TestEmpty exercises the zero-song path. Encoder must produce a
// well-formed file and decoder must accept it without error.
func TestEmpty(t *testing.T) {
	m := Build(nil)
	var buf bytes.Buffer
	if err := m.Write(&buf); err != nil {
		t.Fatalf("write: %v", err)
	}
	got, err := Read(buf.Bytes())
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	if len(got.Songs) != 0 || len(got.UniqArtists) != 0 {
		t.Errorf("expected empty model, got %d songs / %d artists",
			len(got.Songs), len(got.UniqArtists))
	}
}

// TestArtistArtRoundtrip confirms per-artist photos survive a write/
// read cycle: present entries come back byte-equal, missing entries
// stay nil, and the index header bounds-check accepts the resulting
// file. The .tcdb format is the contract between the Go builder and
// the firmware C reader, so format-shape regressions here would
// silently break artist thumbnails on real hardware.
func TestArtistArtRoundtrip(t *testing.T) {
	in := []SongInfo{
		{Path: "/a.flac", Title: "Apple", Artist: "Aphex Twin"},
		{Path: "/b.flac", Title: "Banana", Artist: "Brian Eno"},
		{Path: "/c.flac", Title: "Cherry", Artist: "Caribou"},
	}
	m := Build(in)
	/* Sparse: art for artist 0 + 2, none for 1. The mid-gap is the
	 * interesting case — encoder must still emit an index entry for
	 * "Brian Eno" so per-artist offsets stay aligned. */
	jpegA := []byte{0xff, 0xd8, 0xff, 0xe0, 1, 2, 3, 4, 5}
	jpegC := []byte{0xff, 0xd8, 0xff, 0xe1, 9, 9}
	m.ArtistArt = make([][]byte, len(m.UniqArtists))
	for i, name := range m.UniqArtists {
		switch name {
		case "Aphex Twin":
			m.ArtistArt[i] = jpegA
		case "Caribou":
			m.ArtistArt[i] = jpegC
		}
	}

	var buf bytes.Buffer
	if err := m.Write(&buf); err != nil {
		t.Fatalf("write: %v", err)
	}
	got, err := Read(buf.Bytes())
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	if len(got.ArtistArt) != len(m.UniqArtists) {
		t.Fatalf("artist art len: want %d got %d", len(m.UniqArtists), len(got.ArtistArt))
	}
	for i, name := range got.UniqArtists {
		switch name {
		case "Aphex Twin":
			if !bytes.Equal(got.ArtistArt[i], jpegA) {
				t.Errorf("art[%s] mismatch: want %v got %v", name, jpegA, got.ArtistArt[i])
			}
		case "Brian Eno":
			if got.ArtistArt[i] != nil {
				t.Errorf("art[%s] should be nil, got %v", name, got.ArtistArt[i])
			}
		case "Caribou":
			if !bytes.Equal(got.ArtistArt[i], jpegC) {
				t.Errorf("art[%s] mismatch: want %v got %v", name, jpegC, got.ArtistArt[i])
			}
		}
	}
}

// TestNoArt covers the no-embedded-art path on every track. The art
// blob should be zero-length but still validly addressed.
func TestNoArt(t *testing.T) {
	in := []SongInfo{
		{Path: "/a.flac", Title: "A", Artist: "X", Album: "Y"},
		{Path: "/b.flac", Title: "B", Artist: "X", Album: "Y"},
	}
	m := Build(in)
	var buf bytes.Buffer
	if err := m.Write(&buf); err != nil {
		t.Fatalf("write: %v", err)
	}
	got, err := Read(buf.Bytes())
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	for i, s := range got.Songs {
		if len(s.ArtBytes) != 0 {
			t.Errorf("song[%d] expected no art, got %d bytes", i, len(s.ArtBytes))
		}
	}
}
