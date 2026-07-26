package tagcache

import (
	"bytes"
	"math"
	"strings"
	"testing"
)

// Byte offsets of each header field, mirroring the Header struct layout.
// Declared explicitly (rather than derived) so a silent struct reorder
// shows up as a failing test rather than a silently-relocated poke.
const (
	offMagic             = 0
	offVersion           = 4
	offSongCount         = 8
	offNArtists          = 12
	offNAlbums           = 16
	offNGenres           = 20
	offNComposers        = 24
	offSongsOff          = 28
	offArtistIdxOff      = 36
	offArtistGroupsOff   = 68
	offStringsOff        = 100
	offStringsLen        = 108
	offArtOff            = 116
	offArtLen            = 124
	offArtistArtIdxOff   = 132
	offArtistArtBlobOff  = 140
	offArtistArtBlobLen  = 148
	headerFieldsEndCheck = HeaderSize
)

// validFile returns a well-formed .tcdb with two songs, embedded song
// art, and one artist photo — enough that every section in the format
// is non-empty and therefore corruptible.
func validFile(t *testing.T) []byte {
	t.Helper()
	m := Build([]SongInfo{
		{Path: "/m/alpha.mp3", Title: "Alpha", Artist: "Artist A", Album: "Album A",
			Genre: "Rock", Composer: "Composer A", ArtBytes: []byte("SONGARTBYTES")},
		{Path: "/m/beta.mp3", Title: "Beta", Artist: "Artist B", Album: "Album B",
			Genre: "Jazz", Composer: "Composer B"},
	})
	m.ArtistArt = [][]byte{[]byte("ARTISTPHOTO"), nil}
	var buf bytes.Buffer
	if err := m.Write(&buf); err != nil {
		t.Fatalf("write valid model: %v", err)
	}
	return buf.Bytes()
}

func TestReadValidFile(t *testing.T) {
	m, err := Read(validFile(t))
	if err != nil {
		t.Fatalf("Read(valid) = %v, want nil", err)
	}
	if len(m.Songs) != 2 {
		t.Fatalf("songs = %d, want 2", len(m.Songs))
	}
	if string(m.Songs[0].ArtBytes) != "SONGARTBYTES" {
		t.Errorf("song art = %q", m.Songs[0].ArtBytes)
	}
	if string(m.ArtistArt[0]) != "ARTISTPHOTO" {
		t.Errorf("artist art = %q", m.ArtistArt[0])
	}
}

// TestReadCorrupt is the regression table for the class of bug where a
// file-controlled offset or length was used to slice the mmap'd bytes
// without validation. Before the bounds checks landed, the
// "artist group offset past EOF" case panicked with
//
//	panic: runtime error: slice bounds out of range [65695:180]
//
// Every case here must return an error; none may panic.
func TestReadCorrupt(t *testing.T) {
	u32 := func(b []byte, at int, v uint32) { LE.PutUint32(b[at:], v) }
	u64 := func(b []byte, at int, v uint64) { LE.PutUint64(b[at:], v) }

	tests := []struct {
		name    string
		build   func(t *testing.T) []byte
		wantErr string // substring; "" means any error is fine
	}{
		{
			name:    "empty input",
			build:   func(*testing.T) []byte { return nil },
			wantErr: "file too small",
		},
		{
			name:    "truncated header",
			build:   func(t *testing.T) []byte { return validFile(t)[:HeaderSize-1] },
			wantErr: "file too small",
		},
		{
			name: "bad magic",
			build: func(t *testing.T) []byte {
				b := validFile(t)
				copy(b[offMagic:], "XXXX")
				return b
			},
			wantErr: "bad magic",
		},
		{
			name: "unsupported version",
			build: func(t *testing.T) []byte {
				b := validFile(t)
				u32(b, offVersion, Version+1)
				return b
			},
			wantErr: "unsupported version",
		},
		{
			name: "songs offset past EOF",
			build: func(t *testing.T) []byte {
				b := validFile(t)
				u64(b, offSongsOff, 1<<40)
				return b
			},
			wantErr: "section songs out of bounds",
		},
		{
			name: "song count overflows file",
			build: func(t *testing.T) []byte {
				b := validFile(t)
				u32(b, offSongCount, math.MaxUint32)
				return b
			},
			wantErr: "section songs out of bounds",
		},
		{
			name: "strings length overflows u64",
			build: func(t *testing.T) []byte {
				b := validFile(t)
				u64(b, offStringsLen, math.MaxUint64)
				return b
			},
			wantErr: "section strings out of bounds",
		},
		{
			name: "strings offset past EOF",
			build: func(t *testing.T) []byte {
				b := validFile(t)
				u64(b, offStringsOff, math.MaxUint64-8)
				return b
			},
			wantErr: "section strings out of bounds",
		},
		{
			name: "artist count overflows idx table",
			build: func(t *testing.T) []byte {
				b := validFile(t)
				u32(b, offNArtists, math.MaxUint32)
				return b
			},
			wantErr: "out of bounds",
		},
		{
			name: "artist idx table offset past EOF",
			build: func(t *testing.T) []byte {
				b := validFile(t)
				u64(b, offArtistIdxOff, uint64(len(b))+1)
				return b
			},
			wantErr: "section artist_idx out of bounds",
		},
		{
			// The reproduced panic: an artist-group offset of 0xFFFF in
			// a 180-byte file drove a slice at data[65695:].
			name: "artist group offset past EOF",
			build: func(t *testing.T) []byte {
				b := validFile(t)
				groupsOff := LE.Uint64(b[offArtistGroupsOff:])
				u32(b, int(groupsOff), 0xFFFF)
				return b
			},
			wantErr: "artist_groups group 0",
		},
		{
			name: "artist group count overruns file",
			build: func(t *testing.T) []byte {
				b := validFile(t)
				groupsOff := LE.Uint64(b[offArtistGroupsOff:])
				bodyOff := groupsOff + uint64(LE.Uint32(b[groupsOff:]))
				u32(b, int(bodyOff), math.MaxUint32)
				return b
			},
			wantErr: "overruns file",
		},
		{
			name: "group offset table past EOF",
			build: func(t *testing.T) []byte {
				b := validFile(t)
				u64(b, offArtistGroupsOff, uint64(len(b)))
				return b
			},
			wantErr: "artist_groups offset table out of bounds",
		},
		{
			name: "song art extent outside art blob",
			build: func(t *testing.T) []byte {
				b := validFile(t)
				// Song records start right after the header; ArtLen is
				// the last u64 of the 40-byte record.
				u64(b, HeaderSize+32, math.MaxUint64/2)
				return b
			},
			wantErr: "outside art blob",
		},
		{
			name: "song art offset outside art blob",
			build: func(t *testing.T) []byte {
				b := validFile(t)
				u64(b, HeaderSize+24, 1<<30) // rec.ArtOff
				return b
			},
			wantErr: "outside art blob",
		},
		{
			name: "art section offset past EOF",
			build: func(t *testing.T) []byte {
				b := validFile(t)
				u64(b, offArtOff, uint64(len(b))+16)
				return b
			},
			wantErr: "section art out of bounds",
		},
		{
			name: "art section length past EOF",
			build: func(t *testing.T) []byte {
				b := validFile(t)
				u64(b, offArtLen, uint64(len(b)))
				return b
			},
			wantErr: "section art out of bounds",
		},
		{
			name: "artist art extent outside blob",
			build: func(t *testing.T) []byte {
				b := validFile(t)
				idxOff := LE.Uint64(b[offArtistArtIdxOff:])
				u64(b, int(idxOff)+8, math.MaxUint64/2) // entry 0 length
				return b
			},
			wantErr: "outside artist-art blob",
		},
		{
			name: "artist art blob offset past EOF",
			build: func(t *testing.T) []byte {
				b := validFile(t)
				u64(b, offArtistArtBlobOff, uint64(len(b))+1)
				return b
			},
			wantErr: "section artist_art_blob out of bounds",
		},
		{
			name: "artist art blob length past EOF",
			build: func(t *testing.T) []byte {
				b := validFile(t)
				u64(b, offArtistArtBlobLen, uint64(len(b)))
				return b
			},
			wantErr: "section artist_art_blob out of bounds",
		},
		{
			name: "artist art idx offset past EOF",
			build: func(t *testing.T) []byte {
				b := validFile(t)
				u64(b, offArtistArtIdxOff, uint64(len(b)))
				return b
			},
			wantErr: "section artist_art_idx out of bounds",
		},
		{
			name: "song uniq index past end of table",
			build: func(t *testing.T) []byte {
				b := validFile(t)
				u32(b, HeaderSize+8, 9999) // rec.ArtistIdx
				return b
			},
			wantErr: "artist index 9999 out of range",
		},
		{
			name: "album count zeroed leaves stale song index",
			build: func(t *testing.T) []byte {
				b := validFile(t)
				u32(b, offNAlbums, 0)
				return b
			},
			wantErr: "album index",
		},
		{
			name: "genre count zeroed leaves stale song index",
			build: func(t *testing.T) []byte {
				b := validFile(t)
				u32(b, offNGenres, 0)
				return b
			},
			wantErr: "genre index",
		},
		{
			name: "composer count zeroed leaves stale song index",
			build: func(t *testing.T) []byte {
				b := validFile(t)
				u32(b, offNComposers, 0)
				return b
			},
			wantErr: "composer index",
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			data := tc.build(t)
			m, err := Read(data) // must not panic
			if err == nil {
				t.Fatalf("Read(corrupt) = %+v, nil; want an error", m)
			}
			if tc.wantErr != "" && !strings.Contains(err.Error(), tc.wantErr) {
				t.Fatalf("Read(corrupt) error = %q, want it to contain %q", err, tc.wantErr)
			}
		})
	}
}

// TestReadTruncatedAtEveryLength is the cheap totality check: chopping a
// valid file at any byte boundary must produce an error, never a panic.
func TestReadTruncatedAtEveryLength(t *testing.T) {
	full := validFile(t)
	for n := 0; n < len(full); n++ {
		if _, err := Read(full[:n]); err == nil {
			t.Errorf("Read(valid[:%d]) = nil error, want an error", n)
		}
	}
}

// TestReadHeaderFieldOffsets pins the byte offsets the corruption table
// pokes at, so a Header struct reorder can't quietly turn these tests
// into no-ops.
func TestReadHeaderFieldOffsets(t *testing.T) {
	b := validFile(t)
	if got := LE.Uint32(b[offVersion:]); got != Version {
		t.Errorf("version at offset %d = %d, want %d", offVersion, got, Version)
	}
	if got := LE.Uint32(b[offSongCount:]); got != 2 {
		t.Errorf("song count at offset %d = %d, want 2", offSongCount, got)
	}
	if got := LE.Uint64(b[offSongsOff:]); got != HeaderSize {
		t.Errorf("songs off at offset %d = %d, want %d", offSongsOff, got, HeaderSize)
	}
	if headerFieldsEndCheck != HeaderSize {
		t.Errorf("header size drifted")
	}
}
