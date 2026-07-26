package tagcache

import (
	"bytes"
	"encoding/binary"
	"fmt"
)

// Read parses a .tcdb file from `data` and returns the in-memory model.
//
// Read is total: no input, however corrupt, may panic. Every offset and
// length in the file is attacker/corruption-controlled, so each one is
// validated against the file length and against the length of the
// section it claims to live inside before it is used to slice `data`.
// A malformed file is an error, never a crash.
func Read(data []byte) (*Model, error) {
	if len(data) < HeaderSize {
		return nil, fmt.Errorf("tagcache: file too small (%d bytes; header needs %d)", len(data), HeaderSize)
	}
	var hdr Header
	if err := binary.Read(bytes.NewReader(data[:HeaderSize]), LE, &hdr); err != nil {
		return nil, fmt.Errorf("tagcache: read header: %w", err)
	}
	if hdr.Magic != Magic {
		return nil, fmt.Errorf("tagcache: bad magic %q (want %q)", hdr.Magic[:], Magic[:])
	}
	if hdr.Version != Version {
		return nil, fmt.Errorf("tagcache: unsupported version %d (want %d)", hdr.Version, Version)
	}
	// Bounds-check every advertised section against file length. Width
	// sums are u64 to defend against overflow on a malicious header.
	//
	// These checks also bound every count in the header: a section check
	// of `off + n*stride <= len(data)` implies `n <= len(data)/stride`,
	// so the per-count `make([]T, n)` calls below can't be driven to a
	// huge allocation by a corrupt count field.
	end := uint64(len(data))
	checks := []struct {
		off, length uint64
		name        string
	}{
		{hdr.SongsOff, uint64(hdr.SongCount) * SongRecordSize, "songs"},
		{hdr.ArtistIdxOff, uint64(hdr.NArtists) * 4, "artist_idx"},
		{hdr.AlbumIdxOff, uint64(hdr.NAlbums) * 4, "album_idx"},
		{hdr.GenreIdxOff, uint64(hdr.NGenres) * 4, "genre_idx"},
		{hdr.ComposerIdxOff, uint64(hdr.NComposers) * 4, "composer_idx"},
		{hdr.StringsOff, hdr.StringsLen, "strings"},
		{hdr.ArtOff, hdr.ArtLen, "art"},
		{hdr.ArtistArtIdxOff, uint64(hdr.NArtists) * 16, "artist_art_idx"},
		{hdr.ArtistArtBlobOff, hdr.ArtistArtBlobLen, "artist_art_blob"},
	}
	for _, c := range checks {
		if c.off+c.length < c.off /* overflow */ || c.off+c.length > end {
			return nil, fmt.Errorf("tagcache: section %s out of bounds (off=%d len=%d file=%d)",
				c.name, c.off, c.length, end)
		}
	}
	// Per-group blocks have a fixed header (n*4 offsets) but the body
	// extent depends on per-group counts read at decode time. The
	// offset table is checked here; the per-group bodies it points at
	// are checked in readGroups, which returns an error rather than
	// slicing out of range.
	for _, c := range []struct {
		off  uint64
		n    uint32
		name string
	}{
		{hdr.ArtistGroupsOff, hdr.NArtists, "artist_groups"},
		{hdr.AlbumGroupsOff, hdr.NAlbums, "album_groups"},
		{hdr.GenreGroupsOff, hdr.NGenres, "genre_groups"},
		{hdr.ComposerGroupsOff, hdr.NComposers, "composer_groups"},
	} {
		if c.off+uint64(c.n)*4 < c.off || c.off+uint64(c.n)*4 > end {
			return nil, fmt.Errorf("tagcache: section %s offset table out of bounds", c.name)
		}
	}

	strings := func(off uint32) string {
		if uint64(off) >= hdr.StringsLen {
			return ""
		}
		base := data[hdr.StringsOff:][:hdr.StringsLen]
		end := off
		for int(end) < len(base) && base[end] != 0 {
			end++
		}
		return string(base[off:end])
	}

	// Songs.
	songs := make([]SongInfo, hdr.SongCount)
	songArtist := make([]int32, hdr.SongCount)
	songAlbum := make([]int32, hdr.SongCount)
	songGenre := make([]int32, hdr.SongCount)
	songComposer := make([]int32, hdr.SongCount)
	for i := uint32(0); i < hdr.SongCount; i++ {
		off := hdr.SongsOff + uint64(i)*SongRecordSize
		var rec SongRecord
		if err := binary.Read(bytes.NewReader(data[off:off+SongRecordSize]), LE, &rec); err != nil {
			return nil, fmt.Errorf("read song[%d]: %w", i, err)
		}
		songs[i].Title = strings(rec.TitleOff)
		songs[i].Path = strings(rec.PathOff)
		songArtist[i] = rec.ArtistIdx
		songAlbum[i] = rec.AlbumIdx
		songGenre[i] = rec.GenreIdx
		songComposer[i] = rec.ComposerIdx
		if rec.ArtLen > 0 {
			// The record's art extent is relative to the art blob, so
			// validate it against the blob's length, not the file's:
			// an in-file-but-out-of-blob extent would hand the caller
			// bytes from an unrelated section.
			if rec.ArtOff+rec.ArtLen < rec.ArtOff || rec.ArtOff+rec.ArtLen > hdr.ArtLen {
				return nil, fmt.Errorf(
					"tagcache: song[%d] art extent (off=%d len=%d) outside art blob (len=%d)",
					i, rec.ArtOff, rec.ArtLen, hdr.ArtLen)
			}
			start := hdr.ArtOff + rec.ArtOff
			songs[i].ArtBytes = append([]byte(nil), data[start:start+rec.ArtLen]...)
		}
	}

	// Uniq tables. The n*4 extent was bounds-checked above.
	readUniq := func(off uint64, n uint32) []string {
		out := make([]string, n)
		for i := uint32(0); i < n; i++ {
			s := LE.Uint32(data[off+uint64(i)*4:])
			out[i] = strings(s)
		}
		return out
	}
	uniqArtists := readUniq(hdr.ArtistIdxOff, hdr.NArtists)
	uniqAlbums := readUniq(hdr.AlbumIdxOff, hdr.NAlbums)
	uniqGenres := readUniq(hdr.GenreIdxOff, hdr.NGenres)
	uniqComposers := readUniq(hdr.ComposerIdxOff, hdr.NComposers)

	// Now that uniq strings are recovered, fill SongInfo Artist/Album/etc
	// so the round-trip test sees the same string data on both sides.
	// A song's uniq index is a raw int32 off disk: reject anything that
	// isn't either MissingTag or a live row of the corresponding table.
	fill := func(idx []int32, uniq []string, dim string, set func(*SongInfo, string)) error {
		for i := range songs {
			if idx[i] < 0 {
				continue
			}
			if int(idx[i]) >= len(uniq) {
				return fmt.Errorf("tagcache: song[%d] %s index %d out of range (%d entries)",
					i, dim, idx[i], len(uniq))
			}
			set(&songs[i], uniq[idx[i]])
		}
		return nil
	}
	for _, f := range []struct {
		idx  []int32
		uniq []string
		dim  string
		set  func(*SongInfo, string)
	}{
		{songArtist, uniqArtists, "artist", func(s *SongInfo, v string) { s.Artist = v }},
		{songAlbum, uniqAlbums, "album", func(s *SongInfo, v string) { s.Album = v }},
		{songGenre, uniqGenres, "genre", func(s *SongInfo, v string) { s.Genre = v }},
		{songComposer, uniqComposers, "composer", func(s *SongInfo, v string) { s.Composer = v }},
	} {
		if err := fill(f.idx, f.uniq, f.dim, f.set); err != nil {
			return nil, err
		}
	}

	// Per-group song lists.
	//
	// Group bodies have no length recorded in the header — the block's
	// extent is implied by the next section's offset — so the strongest
	// invariant we can enforce is "stays inside the file". Both the
	// group's own offset (relative to the block start) and its element
	// count are file-controlled, so both are checked before use.
	readGroups := func(off uint64, n uint32, name string) ([][]uint32, error) {
		out := make([][]uint32, n)
		offsets := make([]uint32, n)
		for i := uint32(0); i < n; i++ {
			offsets[i] = LE.Uint32(data[off+uint64(i)*4:])
		}
		for i := uint32(0); i < n; i++ {
			base := off + uint64(offsets[i])
			if base+4 > end {
				return nil, fmt.Errorf(
					"tagcache: %s group %d: count word at offset %d past end of file (%d bytes)",
					name, i, base, end)
			}
			count := LE.Uint32(data[base:])
			body := base + 4
			if uint64(count)*4 > end-body {
				return nil, fmt.Errorf(
					"tagcache: %s group %d: count %d overruns file (%d bytes after offset %d)",
					name, i, count, end-body, body)
			}
			ids := make([]uint32, count)
			for j := uint32(0); j < count; j++ {
				ids[j] = LE.Uint32(data[body+uint64(j)*4:])
			}
			out[i] = ids
		}
		return out, nil
	}

	// Artist art: pull each (off, len) pair, slice into the blob.
	// (0, 0) entries stay nil; a caller with no artist art falls back to
	// the artist's first album art.
	artistArt := make([][]byte, hdr.NArtists)
	for i := uint32(0); i < hdr.NArtists; i++ {
		off := hdr.ArtistArtIdxOff + uint64(i)*16
		artOff := LE.Uint64(data[off:])
		artLen := LE.Uint64(data[off+8:])
		if artLen == 0 {
			continue
		}
		if artOff+artLen < artOff || artOff+artLen > hdr.ArtistArtBlobLen {
			return nil, fmt.Errorf(
				"tagcache: artist[%d] art extent (off=%d len=%d) outside artist-art blob (len=%d)",
				i, artOff, artLen, hdr.ArtistArtBlobLen)
		}
		start := hdr.ArtistArtBlobOff + artOff
		artistArt[i] = append([]byte(nil), data[start:start+artLen]...)
	}

	artistGroups, err := readGroups(hdr.ArtistGroupsOff, hdr.NArtists, "artist_groups")
	if err != nil {
		return nil, err
	}
	albumGroups, err := readGroups(hdr.AlbumGroupsOff, hdr.NAlbums, "album_groups")
	if err != nil {
		return nil, err
	}
	genreGroups, err := readGroups(hdr.GenreGroupsOff, hdr.NGenres, "genre_groups")
	if err != nil {
		return nil, err
	}
	composerGroups, err := readGroups(hdr.ComposerGroupsOff, hdr.NComposers, "composer_groups")
	if err != nil {
		return nil, err
	}

	return &Model{
		Songs:           songs,
		UniqArtists:     uniqArtists,
		UniqAlbums:      uniqAlbums,
		UniqGenres:      uniqGenres,
		UniqComposers:   uniqComposers,
		SongArtistIdx:   songArtist,
		SongAlbumIdx:    songAlbum,
		SongGenreIdx:    songGenre,
		SongComposerIdx: songComposer,
		ArtistGroups:    artistGroups,
		AlbumGroups:     albumGroups,
		GenreGroups:     genreGroups,
		ComposerGroups:  composerGroups,
		ArtistArt:       artistArt,
	}, nil
}
