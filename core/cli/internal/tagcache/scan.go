package tagcache

import (
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/dhowden/tag"
)

// SongInfo holds the per-file metadata captured during a scan, before
// dedup + serialization. The strings are owner copies; ArtBytes is a
// borrowed slice from the source library, valid until the next call.
type SongInfo struct {
	Path     string
	Title    string
	Artist   string
	Album    string
	Genre    string
	Composer string
	ArtBytes []byte
}

// Scan walks `dir` recursively for .flac/.mp3 files (case-insensitive)
// and parses tags via dhowden/tag. Files we can't open or parse are
// silently skipped — the user gets a row with the basename-as-title
// instead of an outright failure. Real-world libraries are nearly always
// nested by artist/album, so recursion is the default; there is no flat
// mode.
//
// Note what SongInfo does *not* carry: duration, track number, disc
// number, and the normalized-name hashes the firmware uses to locate a
// file. CIDX (the format the device actually reads) requires all four,
// so this scanner could not produce a device-loadable index even if the
// encoder targeted CIDX. See README.md in this directory.
//
// Subtrees that error mid-walk (permission denied, vanished while we
// were iterating) are skipped rather than failing the whole scan: the
// user gets the music we *could* read, and the worst case is fewer
// songs than expected — never a half-built tagcache. Symlinks to
// directories are not followed (filepath.WalkDir's default), which
// also avoids cycle hazards on libraries built out of symlinks.
//
// Returned songs are sorted by title (case-insensitive) so that the
// global song-index order matches what the firmware would produce.
func Scan(dir string) ([]SongInfo, error) {
	if fi, err := os.Stat(dir); err != nil {
		return nil, fmt.Errorf("tagcache: stat %s: %w", dir, err)
	} else if !fi.IsDir() {
		return nil, fmt.Errorf("tagcache: %s is not a directory", dir)
	}
	var songs []SongInfo
	walkErr := filepath.WalkDir(dir, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			if d != nil && d.IsDir() {
				return filepath.SkipDir
			}
			return nil
		}
		if d.IsDir() {
			return nil
		}
		ext := strings.ToLower(filepath.Ext(d.Name()))
		if ext != ".flac" && ext != ".mp3" {
			return nil
		}
		s := SongInfo{Path: path, Title: strings.TrimSuffix(d.Name(), filepath.Ext(d.Name()))}
		readTags(&s, path)
		songs = append(songs, s)
		return nil
	})
	if walkErr != nil {
		return nil, fmt.Errorf("tagcache: walk %s: %w", dir, walkErr)
	}
	songs = dedupeSongs(songs)
	sort.SliceStable(songs, func(i, j int) bool {
		return strings.ToLower(songs[i].Title) < strings.ToLower(songs[j].Title)
	})
	return songs, nil
}

// dedupeSongs collapses duplicate scans of the same logical track —
// users commonly keep both .flac and .mp3 copies of an album in
// parallel folders (e.g. Music/FLAC/X/Disc 1/01.flac alongside
// Music/FLAC/mp3/X/Disc 1/01.mp3), and the raw scan picks both up.
// Same goes for the same file scanned twice via different symlinks.
//
// Group key is (title, artist, album), case-insensitive. When two
// scans collide, prefer FLAC over MP3 (lossless beats lossy when
// both are on disk). Songs with empty title/artist/album never
// collapse with each other because the empty-fields key would lump
// every untagged file into one slot — disambiguate by full path
// in that case so we err on the side of preserving everything.
func dedupeSongs(songs []SongInfo) []SongInfo {
	type seenEntry struct{ idx int }
	seen := make(map[string]seenEntry, len(songs))
	out := songs[:0]
	for _, s := range songs {
		title := strings.ToLower(strings.TrimSpace(s.Title))
		artist := strings.ToLower(strings.TrimSpace(s.Artist))
		album := strings.ToLower(strings.TrimSpace(s.Album))
		var key string
		if title == "" || artist == "" || album == "" {
			/* Untagged or partially-tagged files: don't risk
			 * collapsing unrelated tracks. Disambiguate by path. */
			key = "PATH:" + s.Path
		} else {
			key = title + "\x00" + artist + "\x00" + album
		}
		if prev, ok := seen[key]; ok {
			if isPreferredOver(s.Path, out[prev.idx].Path) {
				out[prev.idx] = s
			}
			continue
		}
		seen[key] = seenEntry{idx: len(out)}
		out = append(out, s)
	}
	return out
}

// isPreferredOver reports whether `cand` should replace `cur` when
// they hash to the same dedup key. Lossless-by-extension wins; on
// extension tie, the shallower path wins (matches the user's
// intent — short canonical paths are usually the "primary" copy).
func isPreferredOver(cand, cur string) bool {
	score := func(p string) int {
		ext := strings.ToLower(filepath.Ext(p))
		switch ext {
		case ".flac":
			return 100
		case ".mp3":
			return 50
		default:
			return 0
		}
	}
	sc := score(cand)
	scur := score(cur)
	if sc != scur {
		return sc > scur
	}
	/* Same extension: prefer shorter path. Avoids picking a deeply-
	 * nested copy over a top-level one. */
	return len(cand) < len(cur)
}

// readTags opens `path` and populates s with whatever tag data is
// present. On any error (open failed, format unrecognized) the
// already-populated default fields (Path, basename Title) stay as-is.
func readTags(s *SongInfo, path string) {
	f, err := os.Open(path)
	if err != nil {
		return
	}
	defer f.Close()
	m, err := tag.ReadFrom(f)
	if err != nil {
		return
	}
	if t := m.Title(); t != "" {
		s.Title = t
	}
	s.Artist = primaryArtist(m.Artist())
	s.Album = m.Album()
	s.Genre = m.Genre()
	s.Composer = m.Composer()
	if pic := m.Picture(); pic != nil && len(pic.Data) > 0 {
		// dhowden/tag returns the raw bytes verbatim; the firmware
		// expects JPEG today, so we stash whatever's there and let
		// the C-side stb_image fail gracefully on non-JPEG. Filtering
		// to image/jpeg here would be stricter but breaks the user's
		// PNG cover art when stb_image gains PNG support.
		s.ArtBytes = pic.Data
	}
}

// primaryArtist returns the canonical lead artist from a multi-artist
// tag, e.g.
//
//	"Zach Bryan, The Lumineers"        -> "Zach Bryan"
//	"Justin Bieber,Kehlani"            -> "Justin Bieber"   (no-space comma)
//	"Skrillex, Justin Bieber & Diplo"  -> "Skrillex"
//	"Daft Punk feat. Pharrell"         -> "Daft Punk"
//	"Daniel Caesar"                    -> "Daniel Caesar"
//
// Many libraries credit features and collaborations directly in the
// ARTIST tag, which produces a lot of noisy single-track entries in
// the Artists list (one per unique combination). Folding to the
// primary keeps browsing usable: a song featuring four artists shows
// up under the lead artist, and the full credit can live in the
// title or a future "credits" surface.
//
// Splits on the first occurrence of any common separator: ",",
// " & ", " feat. ", " ft. ", " featuring ". Case-insensitive for the
// word forms.
//
// This will mangle legitimately-comma-named artists ("Tyler, The
// Creator", "Earth, Wind & Fire") — there's no clean automatic way to
// distinguish artist-with-comma from primary,featured. The right long-
// term fix is reading the ALBUMARTIST / TPE2 tag instead, which by
// convention is the canonical lead. That's a follow-up.
func primaryArtist(raw string) string {
	s := strings.TrimSpace(raw)
	if s == "" {
		return s
	}
	earliest := -1
	if i := strings.IndexByte(s, ','); i >= 0 {
		earliest = i
	}
	if i := strings.Index(s, " & "); i >= 0 && (earliest < 0 || i < earliest) {
		earliest = i
	}
	lower := strings.ToLower(s)
	for _, sep := range []string{" feat. ", " feat ", " featuring ", " ft. ", " ft "} {
		if i := strings.Index(lower, sep); i >= 0 && (earliest < 0 || i < earliest) {
			earliest = i
		}
	}
	if earliest < 0 {
		return s
	}
	return strings.TrimSpace(s[:earliest])
}
