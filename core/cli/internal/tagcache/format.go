// Package tagcache builds and reads TCDB, a binary music-index format.
//
// STATUS: TCDB is a host-side format that the firmware does not read.
//
// The index the device actually loads is CIDX (`Music/CORELIB.IDX`),
// written by tools/build_index.py and parsed by
// core/kernel/main.c:library_load_index. The two formats have nothing in
// common: CIDX is magic "CIDX", a 12-byte header and fixed 256-byte
// records keyed by FNV-1a hashes of normalized folder/file names; TCDB
// is magic "TCDB", a 156-byte header and variable-length sections keyed
// by host absolute path. Copying a .tcdb onto an iPod does nothing —
// the firmware ignores it and falls back to a directory scan.
//
// This package is kept as host-side tooling and as a worked design for
// a richer index than CIDX (deduped string table, uniq tables with
// per-dimension group lists, embedded album art, per-artist photos).
// See README.md in this directory for the full comparison and for what
// it would take to make the two formats meet.
//
// Nothing here is load-bearing for a build or a flash. Do not describe
// it as something the device consumes.
//
// File layout (all integers little-endian, the iPod ARM is LE):
//
//	+-------- Header (156 bytes) --------+
//	|  magic[4]      = "TCDB"             |
//	|  version u32   = 2                  |
//	|  song_count u32                     |
//	|  n_artists u32   (uniq)             |
//	|  n_albums u32                       |
//	|  n_genres u32                       |
//	|  n_composers u32                    |
//	|  songs_off u64                      |
//	|  artist_idx_off u64                 |  array of u32 string-offsets
//	|  album_idx_off u64                  |
//	|  genre_idx_off u64                  |
//	|  composer_idx_off u64               |
//	|  artist_groups_off u64              |  per-group song-list arrays
//	|  album_groups_off u64               |
//	|  genre_groups_off u64               |
//	|  composer_groups_off u64            |
//	|  strings_off, strings_len u64       |
//	|  art_off, art_len u64               |
//	|  artist_art_idx_off u64      (v2)   |
//	|  artist_art_blob_off u64     (v2)   |
//	|  artist_art_blob_len u64     (v2)   |
//	+-------- Song records ---------------+
//	|  song_count * SongRecord (40 B ea)  |
//	+-------- Uniq tables ---------------+
//	|  n_artists   * u32 string offsets   |  sorted (case-insensitive)
//	|  n_albums    * u32                  |
//	|  n_genres    * u32                  |
//	|  n_composers * u32                  |
//	+-------- Per-group song lists -------+
//	|  for each dimension (a/b/g/c):      |
//	|    n_X * u32 offsets (relative      |
//	|        to the dimension's _off)     |
//	|    [variable groups, each:          |
//	|       u32 count + count*u32 indices]|
//	+-------- String table ---------------+
//	|  null-terminated UTF-8 strings,     |
//	|  deduped. String 0 is "" (empty).   |
//	+-------- Art blob -------------------+
//	|  raw JPEG bytes, concatenated.      |
//	|  Per-song art_off/art_len point     |
//	|  into here (relative to art_off).   |
//	+-------- Artist art index -----------+
//	|  n_artists * (u64 off, u64 len)     |
//	|  one pair per artist; (0, 0) when   |
//	|  no fetched artist art.             |
//	+-------- Artist art blob ------------+
//	|  raw JPEG bytes, concatenated.      |
//	|  offsets above are relative to its  |
//	|  start. May be empty if no artist   |
//	|  photos were fetched at build time. |
//	+-------------------------------------+
//
// Songs are sorted alphabetically by title (case-insensitive). Uniq
// tables are sorted alphabetically. Per-group song-list orders match
// the global song order, so a reader can present a drilldown as a slice
// of the song array rather than a filter over it.
//
// Songs are keyed by their host absolute path (SongInfo.Path). That is
// one of several reasons this format could not be handed to the device
// as-is: the firmware locates files purely by FNV-1a hashes of
// normalized folder and file names, and never sees a host path.
//
// Case-insensitive ordering is ASCII-only — we use strings.ToLower, NOT
// a Unicode collator. Two strings that differ only by Unicode-case
// (e.g. Turkish dotless i) are treated as distinct. For an iPod-class
// music library this hasn't surfaced, but don't promise Unicode-correct
// collation here.
//
// Versioning: bump `Version` whenever the binary layout changes in a
// non-additive way. Read rejects mismatched versions rather than trying
// to interpret them.
package tagcache

import "encoding/binary"

// Magic is the four-byte file signature.
var Magic = [4]byte{'T', 'C', 'D', 'B'}

// Version is the on-disk format version. Bumped on incompatible layout
// changes; Read refuses to load mismatched versions. (There is no
// firmware-side reader — see the package status note.)
//
// v2 added the artist-art index + blob (fetched from Deezer /
// MusicBrainz / Wikidata / Commons by `core tagcache build
// --fetch-art`). v1 readers can't safely interpret v2 because the
// header grew from 132 to 156 bytes; nothing writes v1 any more.
const Version uint32 = 2

// HeaderSize is the byte size of the fixed header. The file's first
// byte after the header is the start of the song-record array.
//
// Layout (little-endian, no padding — encoding/binary packs verbatim):
//
//	[0..4)     magic                  4 B
//	[4..28)    6 * u32 (version + 5 counts) 24 B
//	[28..36)   songs_off u64           8 B
//	[36..68)   4 * u64 idx offsets    32 B
//	[68..100)  4 * u64 group offsets  32 B
//	[100..116) strings_off, _len u64  16 B
//	[116..132) art_off,     _len u64  16 B
//	[132..156) artist_art_idx_off u64,
//	           artist_art_blob_off u64,
//	           artist_art_blob_len u64    24 B
const HeaderSize = 156

// SongRecordSize is the on-disk size of one song record. Fixed so the
// reader can index by global song idx in O(1).
const SongRecordSize = 40

// MissingTag is the sentinel index value (-1) used in song records when
// the file's tag for a given dimension is absent.
const MissingTag = int32(-1)

// LE is the byte order used throughout the file.
var LE = binary.LittleEndian

// Header mirrors the on-disk layout of the fixed header. Fields are
// populated by Builder.Write and verified by Reader.
type Header struct {
	Magic      [4]byte
	Version    uint32
	SongCount  uint32
	NArtists   uint32
	NAlbums    uint32
	NGenres    uint32
	NComposers uint32
	SongsOff   uint64

	ArtistIdxOff   uint64
	AlbumIdxOff    uint64
	GenreIdxOff    uint64
	ComposerIdxOff uint64

	ArtistGroupsOff   uint64
	AlbumGroupsOff    uint64
	GenreGroupsOff    uint64
	ComposerGroupsOff uint64

	StringsOff uint64
	StringsLen uint64
	ArtOff     uint64
	ArtLen     uint64

	/* v2: artist art (one image per uniq artist, fetched offline). */
	ArtistArtIdxOff  uint64 /* offset of n_artists * (u64 off, u64 len) */
	ArtistArtBlobOff uint64 /* offset of the artist-art JPEG concatenation */
	ArtistArtBlobLen uint64 /* total bytes in the artist-art blob; 0 means none fetched */
}

// SongRecord mirrors the on-disk layout of one entry in the song-record
// array. ArtistIdx/AlbumIdx/GenreIdx/ComposerIdx are MissingTag (-1)
// when the file had no tag for that dimension. ArtOff/ArtLen are zero
// when the file had no embedded picture; otherwise ArtOff is relative
// to the file's ArtOff.
type SongRecord struct {
	TitleOff    uint32
	PathOff     uint32
	ArtistIdx   int32
	AlbumIdx    int32
	GenreIdx    int32
	ComposerIdx int32
	ArtOff      uint64
	ArtLen      uint64
}
