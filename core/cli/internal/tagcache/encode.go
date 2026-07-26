package tagcache

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"io"
	"sort"
	"strings"
)

// Build assembles the in-memory model (deduped strings, uniq tables,
// per-group song lists, art blob) from a song slice. The returned
// model can then be written to disk via Write.
//
// Tag values that compare case-insensitively equal share a uniq slot
// (e.g. "Aphex Twin" and "aphex twin" collapse to one artist). The
// canonical spelling is deterministic: after sorting songs by title,
// the first spelling encountered in title order wins.
//
// (An earlier version of this comment contrasted the tie-break with
// `tagcache.c::build_unique_index` on the firmware side. There is no
// such file and no such function: the device builds its lists from
// CIDX in core/kernel/main.c. Nothing on the device re-derives these
// uniq tables, so the only consumer of this ordering is Read.)
func Build(songs []SongInfo) *Model {
	// Sort by title (case-insensitive) up front. The format docstring
	// promises the on-disk song array is title-sorted; doing it here
	// (instead of trusting the caller) keeps direct callers and the
	// round-trip test from accidentally violating the contract.
	sort.SliceStable(songs, func(i, j int) bool {
		return strings.ToLower(songs[i].Title) < strings.ToLower(songs[j].Title)
	})
	m := &Model{
		Songs: songs,
	}

	m.UniqArtists,   m.SongArtistIdx   = uniqIndex(songs, func(s *SongInfo) string { return s.Artist })
	m.UniqAlbums,    m.SongAlbumIdx    = uniqIndex(songs, func(s *SongInfo) string { return s.Album })
	m.UniqGenres,    m.SongGenreIdx    = uniqIndex(songs, func(s *SongInfo) string { return s.Genre })
	m.UniqComposers, m.SongComposerIdx = uniqIndex(songs, func(s *SongInfo) string { return s.Composer })

	m.ArtistGroups   = groupBy(len(m.UniqArtists),   m.SongArtistIdx)
	m.AlbumGroups    = groupBy(len(m.UniqAlbums),    m.SongAlbumIdx)
	m.GenreGroups    = groupBy(len(m.UniqGenres),    m.SongGenreIdx)
	m.ComposerGroups = groupBy(len(m.UniqComposers), m.SongComposerIdx)

	return m
}

// Model is the in-memory representation matching the on-disk layout.
// Exposed so tests can verify Build's dedup + grouping without going
// through a write/read cycle.
type Model struct {
	Songs []SongInfo

	// Uniq tables, sorted alphabetically (case-insensitive).
	UniqArtists   []string
	UniqAlbums    []string
	UniqGenres    []string
	UniqComposers []string

	// Per-song -> uniq lookup. -1 when the song had no tag for this
	// dimension.
	SongArtistIdx   []int32
	SongAlbumIdx    []int32
	SongGenreIdx    []int32
	SongComposerIdx []int32

	// Per-group song lists. ArtistGroups[a] is the slice of song
	// indices whose Artist matches UniqArtists[a], in global song
	// order. Empty slice when no songs match.
	ArtistGroups   [][]uint32
	AlbumGroups    [][]uint32
	GenreGroups    [][]uint32
	ComposerGroups [][]uint32

	// Optional per-artist photo bytes (JPEG, fetched offline by
	// `core tagcache build --fetch-art`). Length is either 0 (none
	// fetched at all — common path) or len(UniqArtists). Per-entry
	// nil/empty means "no photo for this artist".
	//
	// These images are third-party works under third-party terms; see
	// the licensing notes in internal/artistart. They are for local use
	// and must not be bundled into a release artifact.
	ArtistArt [][]byte
}

// uniqIndex collects the case-insensitive-distinct values of one tag
// dimension across `songs`, sorts them alphabetically, and returns
// the canonical spellings plus a per-song lookup table. Songs whose
// tag is empty get -1.
func uniqIndex(songs []SongInfo, get func(*SongInfo) string) ([]string, []int32) {
	type entry struct{ canonical string }
	seen := make(map[string]*entry)        // key = lowercased value
	var values []string                    // dedup output, pre-sort
	for i := range songs {
		v := strings.TrimSpace(get(&songs[i]))
		if v == "" {
			continue
		}
		key := strings.ToLower(v)
		if _, ok := seen[key]; !ok {
			seen[key] = &entry{canonical: v}
			values = append(values, v)
		}
	}
	sort.Slice(values, func(i, j int) bool {
		return strings.ToLower(values[i]) < strings.ToLower(values[j])
	})
	// Build position-by-key map after sort so per-song lookup picks
	// the post-sort index.
	pos := make(map[string]int32, len(values))
	for i, v := range values {
		pos[strings.ToLower(v)] = int32(i)
	}
	idx := make([]int32, len(songs))
	for i := range songs {
		v := strings.TrimSpace(get(&songs[i]))
		if v == "" {
			idx[i] = MissingTag
		} else {
			idx[i] = pos[strings.ToLower(v)]
		}
	}
	return values, idx
}

// groupBy inverts a per-song -> group lookup into a per-group -> songs
// list. Songs with -1 (no tag) are skipped; group ordering preserves
// the global song order.
func groupBy(nGroups int, songIdx []int32) [][]uint32 {
	out := make([][]uint32, nGroups)
	for s, g := range songIdx {
		if g < 0 {
			continue
		}
		out[g] = append(out[g], uint32(s))
	}
	return out
}

// Write serializes the model to w in the .tcdb format. The layout
// follows the comments in format.go; see the package docstring for
// the byte map.
func (m *Model) Write(w io.Writer) error {
	// String table is built first via a deduper. Strings live in the
	// last section of the file; offsets returned by intern() are
	// relative to strings_off, which we patch in once we know it.
	st := newStringTable()

	// Reserve string offsets for every per-song / uniq-table string.
	// (Doing this up-front lets us fill song records and uniq tables
	// in one pass.)
	titleOffs    := make([]uint32, len(m.Songs))
	pathOffs     := make([]uint32, len(m.Songs))
	for i, s := range m.Songs {
		titleOffs[i] = st.intern(s.Title)
		pathOffs[i]  = st.intern(s.Path)
	}
	artistStrOffs   := internAll(st, m.UniqArtists)
	albumStrOffs    := internAll(st, m.UniqAlbums)
	genreStrOffs    := internAll(st, m.UniqGenres)
	composerStrOffs := internAll(st, m.UniqComposers)

	// Art blob: concatenate every song's ArtBytes; record per-song
	// offset+len (relative to art_off). De-dup by content hash to
	// keep the file small when one album shares cover art across N
	// tracks.
	var artBlob bytes.Buffer
	type artRef struct{ off, length uint64 }
	artRefs := make([]artRef, len(m.Songs))
	artDedup := make(map[string]artRef)
	for i, s := range m.Songs {
		if len(s.ArtBytes) == 0 {
			continue
		}
		key := string(s.ArtBytes) // hash via Go map; cheap on these sizes
		if r, ok := artDedup[key]; ok {
			artRefs[i] = r
			continue
		}
		r := artRef{off: uint64(artBlob.Len()), length: uint64(len(s.ArtBytes))}
		artBlob.Write(s.ArtBytes)
		artDedup[key] = r
		artRefs[i] = r
	}

	// Compute section offsets.
	songsOff := uint64(HeaderSize)
	songsLen := uint64(len(m.Songs)) * SongRecordSize

	artistIdxOff   := songsOff       + songsLen
	albumIdxOff    := artistIdxOff   + uint64(len(m.UniqArtists))   * 4
	genreIdxOff    := albumIdxOff    + uint64(len(m.UniqAlbums))    * 4
	composerIdxOff := genreIdxOff    + uint64(len(m.UniqGenres))    * 4
	uniqEnd        := composerIdxOff + uint64(len(m.UniqComposers)) * 4

	// Per-group song lists: each dimension is a flat block starting
	// with N u32 offsets (relative to that dimension's _off), followed
	// by [u32 count, u32 indices...] tuples.
	artistGroupsBytes   := encodeGroups(m.ArtistGroups)
	albumGroupsBytes    := encodeGroups(m.AlbumGroups)
	genreGroupsBytes    := encodeGroups(m.GenreGroups)
	composerGroupsBytes := encodeGroups(m.ComposerGroups)

	artistGroupsOff   := uniqEnd
	albumGroupsOff    := artistGroupsOff   + uint64(len(artistGroupsBytes))
	genreGroupsOff    := albumGroupsOff    + uint64(len(albumGroupsBytes))
	composerGroupsOff := genreGroupsOff    + uint64(len(genreGroupsBytes))
	groupsEnd         := composerGroupsOff + uint64(len(composerGroupsBytes))

	stringsOff := groupsEnd
	stringsBytes := st.bytes()
	stringsLen := uint64(len(stringsBytes))

	artOff := stringsOff + stringsLen
	artLen := uint64(artBlob.Len())

	// Artist art: per-artist (off, len) pairs, then a concatenated
	// blob. Always emit the index (even when empty) so the on-disk
	// shape is uniform; bound the index to the smaller of UniqArtists
	// vs ArtistArt so a partial set is tolerated.
	var artistArtBlob bytes.Buffer
	type artistArtEntry struct{ off, length uint64 }
	artistArtEntries := make([]artistArtEntry, len(m.UniqArtists))
	for i := 0; i < len(m.UniqArtists) && i < len(m.ArtistArt); i++ {
		b := m.ArtistArt[i]
		if len(b) == 0 {
			continue
		}
		artistArtEntries[i] = artistArtEntry{
			off:    uint64(artistArtBlob.Len()),
			length: uint64(len(b)),
		}
		artistArtBlob.Write(b)
	}
	artistArtIdxOff  := artOff + artLen
	artistArtBlobOff := artistArtIdxOff + uint64(len(m.UniqArtists))*16
	artistArtBlobLen := uint64(artistArtBlob.Len())

	// Header.
	hdr := Header{
		Magic:             Magic,
		Version:           Version,
		SongCount:         uint32(len(m.Songs)),
		NArtists:          uint32(len(m.UniqArtists)),
		NAlbums:           uint32(len(m.UniqAlbums)),
		NGenres:           uint32(len(m.UniqGenres)),
		NComposers:        uint32(len(m.UniqComposers)),
		SongsOff:          songsOff,
		ArtistIdxOff:      artistIdxOff,
		AlbumIdxOff:       albumIdxOff,
		GenreIdxOff:       genreIdxOff,
		ComposerIdxOff:    composerIdxOff,
		ArtistGroupsOff:   artistGroupsOff,
		AlbumGroupsOff:    albumGroupsOff,
		GenreGroupsOff:    genreGroupsOff,
		ComposerGroupsOff: composerGroupsOff,
		StringsOff:        stringsOff,
		StringsLen:        stringsLen,
		ArtOff:            artOff,
		ArtLen:            artLen,
		ArtistArtIdxOff:   artistArtIdxOff,
		ArtistArtBlobOff:  artistArtBlobOff,
		ArtistArtBlobLen:  artistArtBlobLen,
	}

	// Emit. Order matters: header, songs, uniq tables, group blocks,
	// strings, art.
	if err := binary.Write(w, LE, &hdr); err != nil {
		return fmt.Errorf("write header: %w", err)
	}
	for i := range m.Songs {
		rec := SongRecord{
			TitleOff:    titleOffs[i],
			PathOff:     pathOffs[i],
			ArtistIdx:   m.SongArtistIdx[i],
			AlbumIdx:    m.SongAlbumIdx[i],
			GenreIdx:    m.SongGenreIdx[i],
			ComposerIdx: m.SongComposerIdx[i],
			ArtOff:      artRefs[i].off,
			ArtLen:      artRefs[i].length,
		}
		if err := binary.Write(w, LE, &rec); err != nil {
			return fmt.Errorf("write song[%d]: %w", i, err)
		}
	}
	if err := writeU32s(w, artistStrOffs); err != nil {
		return err
	}
	if err := writeU32s(w, albumStrOffs); err != nil {
		return err
	}
	if err := writeU32s(w, genreStrOffs); err != nil {
		return err
	}
	if err := writeU32s(w, composerStrOffs); err != nil {
		return err
	}
	if _, err := w.Write(artistGroupsBytes); err != nil {
		return err
	}
	if _, err := w.Write(albumGroupsBytes); err != nil {
		return err
	}
	if _, err := w.Write(genreGroupsBytes); err != nil {
		return err
	}
	if _, err := w.Write(composerGroupsBytes); err != nil {
		return err
	}
	if _, err := w.Write(stringsBytes); err != nil {
		return err
	}
	if _, err := w.Write(artBlob.Bytes()); err != nil {
		return err
	}
	for _, e := range artistArtEntries {
		if err := binary.Write(w, LE, e.off); err != nil {
			return err
		}
		if err := binary.Write(w, LE, e.length); err != nil {
			return err
		}
	}
	if _, err := w.Write(artistArtBlob.Bytes()); err != nil {
		return err
	}
	return nil
}

// encodeGroups serializes one dimension's per-group song lists into
// the flat block described in format.go: N u32 offsets followed by
// per-group [count, indices...] tuples. Offsets are relative to the
// start of the block.
func encodeGroups(groups [][]uint32) []byte {
	n := len(groups)
	headerSize := uint32(n) * 4
	// First pass: lay out each group's body offset, accumulating
	// the body's running size.
	offsets := make([]uint32, n)
	bodySize := uint32(0)
	for i, g := range groups {
		offsets[i] = headerSize + bodySize
		bodySize += 4 + uint32(len(g))*4
	}
	var buf bytes.Buffer
	buf.Grow(int(headerSize + bodySize))
	for _, off := range offsets {
		_ = binary.Write(&buf, LE, off)
	}
	for _, g := range groups {
		_ = binary.Write(&buf, LE, uint32(len(g)))
		for _, idx := range g {
			_ = binary.Write(&buf, LE, idx)
		}
	}
	return buf.Bytes()
}

func writeU32s(w io.Writer, vs []uint32) error {
	for _, v := range vs {
		if err := binary.Write(w, LE, v); err != nil {
			return err
		}
	}
	return nil
}

func internAll(st *stringTable, ss []string) []uint32 {
	out := make([]uint32, len(ss))
	for i, s := range ss {
		out[i] = st.intern(s)
	}
	return out
}
