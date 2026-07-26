package artistart

import (
	"context"
	"crypto/sha1"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
)

// ErrNoCacheDir is returned by DefaultCacheDir when the platform gives
// us no user cache directory to work in.
var ErrNoCacheDir = errors.New("artistart: no user cache directory available")

// DefaultCacheDir returns ~/.cache/core/artist-art (or the platform
// equivalent), creating the directory tree if necessary.
//
// It returns an error rather than an empty path when the platform has
// no cache dir. It used to return ("", nil), which CachedFetch then
// joined onto — producing relative paths and scattering 40-hex-char
// .jpg/.missing files across the user's working directory. Callers that
// want to continue without a cache should pass "" to CachedFetch
// explicitly, which disables all cache reads and writes.
func DefaultCacheDir() (string, error) {
	root, err := os.UserCacheDir()
	if err != nil {
		return "", fmt.Errorf("%w: %v", ErrNoCacheDir, err)
	}
	dir := filepath.Join(root, "core", "artist-art")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", fmt.Errorf("create %s: %w", dir, err)
	}
	return dir, nil
}

// CachedFetch reads cached bytes for `artistName` from `cacheDir`, or
// runs the full Fetch+Process chain and writes the result on a miss.
// The cache key is a SHA1 of the lowercase trimmed artist name —
// avoids filesystem-illegal characters and case-collisions ("Beach
// House" vs "beach house" share a slot, which matches the tagcache's
// case-insensitive dedup).
//
// An empty cacheDir disables caching entirely: nothing is read and
// nothing is written, and every call goes to the network.
//
// On miss-and-not-found, writes a sentinel ".missing" file so future
// rebuilds skip the network round-trip for artists no source knows
// about. Sentinel TTL is the user's responsibility — delete the cache
// dir to force a refresh.
//
// Each cached image gets a ".json" provenance sidecar recording the
// source URL and license terms; see Source.
//
// Returns:
//
//   - bytes, true, nil  on hit (real image, served from cache)
//   - bytes, false, nil on miss-then-fetch (real image, network fetched)
//   - nil, *, ErrNotFound on miss-or-cached-miss (no usable image)
//   - nil, *, err on any other failure
func CachedFetch(ctx context.Context, f *Fetcher, cacheDir, artistName string,
	maxDim, quality int) ([]byte, bool, error) {
	caching := cacheDir != ""
	key := cacheKey(artistName)
	var imgPath, missPath string
	if caching {
		imgPath = filepath.Join(cacheDir, key+".jpg")
		missPath = filepath.Join(cacheDir, key+".missing")

		if b, err := os.ReadFile(imgPath); err == nil {
			return b, true, nil
		} else if !errors.Is(err, fs.ErrNotExist) {
			return nil, false, fmt.Errorf("read cache: %w", err)
		}
		if _, err := os.Stat(missPath); err == nil {
			return nil, true, ErrNotFound
		}
	}

	raw, src, err := f.Fetch(ctx, artistName)
	if err != nil {
		if errors.Is(err, ErrNotFound) {
			/* Persist the negative result; rebuilds shouldn't pay
			 * the network cost for artists the chain can't resolve. */
			if caching {
				_ = os.WriteFile(missPath, nil, 0o644)
			}
			return nil, false, ErrNotFound
		}
		return nil, false, err
	}
	processed, err := Process(raw, maxDim, quality)
	if err != nil {
		return nil, false, fmt.Errorf("process: %w", err)
	}
	if caching {
		if err := os.WriteFile(imgPath, processed, 0o644); err != nil {
			return nil, false, fmt.Errorf("write cache: %w", err)
		}
		if err := writeProvenance(cacheDir, key, artistName, src); err != nil {
			return nil, false, err
		}
	}
	return processed, false, nil
}

// sidecar is the on-disk shape of a provenance record: the Source plus
// the artist name it was fetched for, so a cache directory full of
// hashed filenames is still auditable by a human.
type sidecar struct {
	Source
	Artist string `json:"artist"`
}

func writeProvenance(cacheDir, key, artistName string, src Source) error {
	rec := sidecar{Source: src, Artist: artistName}
	b, err := json.MarshalIndent(rec, "", "  ")
	if err != nil {
		return fmt.Errorf("encode provenance: %w", err)
	}
	path := filepath.Join(cacheDir, key+".json")
	if err := os.WriteFile(path, append(b, '\n'), 0o644); err != nil {
		return fmt.Errorf("write provenance %s: %w", path, err)
	}
	return nil
}

// CachedProvenance returns the recorded provenance for a cached artist
// image, plus the artist name the sidecar was written for. Returns
// fs.ErrNotExist when there is no sidecar (either the artist was never
// fetched, or the image predates provenance recording).
func CachedProvenance(cacheDir, artistName string) (Source, error) {
	if cacheDir == "" {
		return Source{}, ErrNoCacheDir
	}
	path := filepath.Join(cacheDir, cacheKey(artistName)+".json")
	b, err := os.ReadFile(path)
	if err != nil {
		return Source{}, err
	}
	var rec sidecar
	if err := json.Unmarshal(b, &rec); err != nil {
		return Source{}, fmt.Errorf("decode provenance %s: %w", path, err)
	}
	return rec.Source, nil
}

func cacheKey(s string) string {
	h := sha1.Sum([]byte(strings.ToLower(strings.TrimSpace(s))))
	return hex.EncodeToString(h[:])
}
