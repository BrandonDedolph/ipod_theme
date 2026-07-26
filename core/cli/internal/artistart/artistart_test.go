package artistart

import (
	"bytes"
	"context"
	"errors"
	"image"
	"image/color"
	"image/jpeg"
	"io"
	"io/fs"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

// --- helpers -------------------------------------------------------

type rtFunc func(*http.Request) (*http.Response, error)

func (f rtFunc) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }

// testFetcher builds a Fetcher whose transport is `rt` and whose rate
// limiter is unpaced, so tests stay fast and never touch the network.
func testFetcher(rt rtFunc) *Fetcher {
	l := newLimiter()
	l.fallback = 0
	l.rules = nil
	return &Fetcher{HTTP: &http.Client{Transport: rt}, limit: l}
}

func jsonResponse(req *http.Request, code int, body string) *http.Response {
	return &http.Response{
		StatusCode: code,
		Body:       io.NopCloser(strings.NewReader(body)),
		Header:     http.Header{"Content-Type": []string{"application/json"}},
		Request:    req,
	}
}

func bytesResponse(req *http.Request, code int, body []byte, hdr http.Header) *http.Response {
	if hdr == nil {
		hdr = http.Header{}
	}
	return &http.Response{
		StatusCode: code,
		Body:       io.NopCloser(bytes.NewReader(body)),
		Header:     hdr,
		Request:    req,
	}
}

// testJPEG returns a decodable JPEG of the given size with a gradient,
// so downscaling has something to average.
func testJPEG(t *testing.T, w, h int) []byte {
	t.Helper()
	img := image.NewRGBA(image.Rect(0, 0, w, h))
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			img.SetRGBA(x, y, color.RGBA{R: uint8(x * 255 / w), G: uint8(y * 255 / h), B: 128, A: 255})
		}
	}
	var buf bytes.Buffer
	if err := jpeg.Encode(&buf, img, &jpeg.Options{Quality: 90}); err != nil {
		t.Fatalf("encode test jpeg: %v", err)
	}
	return buf.Bytes()
}

// deezerStub serves the Deezer search + image-download hops, and an
// empty MusicBrainz result for artists it doesn't know, so the fallback
// chain terminates in ErrNotFound.
func deezerStub(t *testing.T, known map[string][]byte, calls *int32) rtFunc {
	t.Helper()
	return func(req *http.Request) (*http.Response, error) {
		atomic.AddInt32(calls, 1)
		switch {
		case req.URL.Host == "api.deezer.com":
			name := req.URL.Query().Get("q")
			if _, ok := known[name]; !ok {
				return jsonResponse(req, 200, `{"data":[]}`), nil
			}
			return jsonResponse(req, 200,
				`{"data":[{"name":"`+name+`","picture_big":"https://e-cdns-images.dzcdn.net/`+name+`.jpg"}]}`), nil
		case strings.HasSuffix(req.URL.Host, "dzcdn.net"):
			name := strings.TrimSuffix(strings.TrimPrefix(req.URL.Path, "/"), ".jpg")
			img, ok := known[name]
			if !ok {
				return jsonResponse(req, 404, "not found"), nil
			}
			return bytesResponse(req, 200, img, nil), nil
		case req.URL.Host == "musicbrainz.org":
			return jsonResponse(req, 200, `{"artists":[]}`), nil
		}
		t.Errorf("unexpected request to %s", req.URL)
		return jsonResponse(req, 500, "unexpected"), nil
	}
}

// --- pure functions ------------------------------------------------

func TestFitBox(t *testing.T) {
	tests := []struct {
		name                   string
		srcW, srcH, maxW, maxH int
		wantW, wantH           int
	}{
		{"already fits", 64, 64, 128, 128, 64, 64},
		{"exact fit", 128, 128, 128, 128, 128, 128},
		{"square downscale", 500, 500, 128, 128, 128, 128},
		{"landscape", 2000, 1200, 128, 128, 128, 76},
		{"portrait", 1200, 2000, 128, 128, 76, 128},
		{"extreme landscape", 4000, 100, 128, 128, 128, 3},
		{"one axis over", 300, 64, 128, 128, 128, 27},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			w, h := fitBox(tc.srcW, tc.srcH, tc.maxW, tc.maxH)
			if w != tc.wantW || h != tc.wantH {
				t.Errorf("fitBox(%d,%d,%d,%d) = %d,%d; want %d,%d",
					tc.srcW, tc.srcH, tc.maxW, tc.maxH, w, h, tc.wantW, tc.wantH)
			}
			if w > tc.maxW || h > tc.maxH {
				t.Errorf("result %dx%d exceeds box %dx%d", w, h, tc.maxW, tc.maxH)
			}
		})
	}
}

func TestBoxDownscaleAveragesCells(t *testing.T) {
	// Left half pure red, right half pure blue. Downscaled to 2x1 the
	// two cells must come out as the two source colors; to 1x1 the mean.
	src := image.NewRGBA(image.Rect(0, 0, 4, 2))
	for y := 0; y < 2; y++ {
		for x := 0; x < 4; x++ {
			c := color.RGBA{R: 255, A: 255}
			if x >= 2 {
				c = color.RGBA{B: 255, A: 255}
			}
			src.SetRGBA(x, y, c)
		}
	}
	got := boxDownscale(src, 2, 1)
	if r, _, _, _ := got.At(0, 0).RGBA(); r>>8 != 255 {
		t.Errorf("left cell R = %d, want 255", r>>8)
	}
	if _, _, b, _ := got.At(1, 0).RGBA(); b>>8 != 255 {
		t.Errorf("right cell B = %d, want 255", b>>8)
	}
	one := boxDownscale(src, 1, 1)
	r, _, b, a := one.At(0, 0).RGBA()
	if r>>8 != 127 || b>>8 != 127 {
		t.Errorf("1x1 mean = R%d B%d, want ~127 each", r>>8, b>>8)
	}
	if a>>8 != 255 {
		t.Errorf("alpha = %d, want 255", a>>8)
	}
}

func TestBoxDownscaleUpscaleIsNearestNeighbor(t *testing.T) {
	// A dst larger than the src on an axis collapses to cell width 1
	// rather than dividing by zero.
	src := image.NewRGBA(image.Rect(0, 0, 2, 2))
	src.SetRGBA(0, 0, color.RGBA{R: 255, A: 255})
	got := boxDownscale(src, 4, 4)
	if got.Bounds().Dx() != 4 || got.Bounds().Dy() != 4 {
		t.Fatalf("bounds = %v", got.Bounds())
	}
	if r, _, _, _ := got.At(0, 0).RGBA(); r>>8 != 255 {
		t.Errorf("top-left R = %d, want 255", r>>8)
	}
}

func TestBoxDownscaleRespectsNonZeroOrigin(t *testing.T) {
	// image.Decode can hand back images whose Bounds().Min isn't (0,0).
	src := image.NewRGBA(image.Rect(10, 20, 14, 24))
	for y := 20; y < 24; y++ {
		for x := 10; x < 14; x++ {
			src.SetRGBA(x, y, color.RGBA{G: 200, A: 255})
		}
	}
	got := boxDownscale(src, 2, 2)
	if _, g, _, _ := got.At(0, 0).RGBA(); g>>8 != 200 {
		t.Errorf("G = %d, want 200 (offset bounds mishandled)", g>>8)
	}
}

func TestProcess(t *testing.T) {
	raw := testJPEG(t, 500, 300)
	out, err := Process(raw, 128, 85)
	if err != nil {
		t.Fatalf("Process: %v", err)
	}
	img, format, err := image.Decode(bytes.NewReader(out))
	if err != nil {
		t.Fatalf("decode result: %v", err)
	}
	if format != "jpeg" {
		t.Errorf("format = %q, want jpeg", format)
	}
	if w, h := img.Bounds().Dx(), img.Bounds().Dy(); w != 128 || h != 76 {
		t.Errorf("size = %dx%d, want 128x76", w, h)
	}
	if len(out) >= len(raw) {
		t.Errorf("processed image (%d B) not smaller than source (%d B)", len(out), len(raw))
	}
}

func TestProcessRejectsBadArgs(t *testing.T) {
	raw := testJPEG(t, 64, 64)
	for _, tc := range []struct {
		name             string
		maxDim, quality  int
		raw              []byte
		wantErrSubstring string
	}{
		{"zero maxDim", 0, 85, raw, "maxDim"},
		{"huge maxDim", 8192, 85, raw, "maxDim"},
		{"quality zero", 128, 0, raw, "quality"},
		{"quality over 100", 128, 101, raw, "quality"},
		{"not an image", 128, 85, []byte("definitely not a jpeg"), "decode"},
		{"empty input", 128, 85, nil, "decode"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if _, err := Process(tc.raw, tc.maxDim, tc.quality); err == nil ||
				!strings.Contains(err.Error(), tc.wantErrSubstring) {
				t.Errorf("Process = %v, want an error containing %q", err, tc.wantErrSubstring)
			}
		})
	}
}

func TestCacheKey(t *testing.T) {
	base := cacheKey("Beach House")
	for _, variant := range []string{"beach house", "  Beach House  ", "BEACH HOUSE"} {
		if got := cacheKey(variant); got != base {
			t.Errorf("cacheKey(%q) = %s, want %s (case/space-insensitive)", variant, got, base)
		}
	}
	if cacheKey("Beach House") == cacheKey("Beach Houses") {
		t.Error("distinct names collided")
	}
	if len(base) != 40 {
		t.Errorf("key length = %d, want 40 hex chars", len(base))
	}
	if strings.ContainsAny(base, `/\:*?"<>|`) {
		t.Errorf("key %q contains filesystem-illegal characters", base)
	}
}

func TestSourceFor(t *testing.T) {
	tests := []struct {
		url                 string
		wantProvider        string
		wantRedistributable bool
	}{
		{"https://e-cdns-images.dzcdn.net/images/artist/x/500x500-000000-80-0-0.jpg", ProviderDeezer, false},
		{"https://api.deezer.com/search/artist", ProviderDeezer, false},
		{"https://upload.wikimedia.org/wikipedia/commons/thumb/a/b/X.jpg", ProviderCommons, true},
		{"https://commons.wikimedia.org/w/api.php", ProviderCommons, true},
		{"https://example.com/photo.jpg", ProviderUnknown, false},
		{"", ProviderUnknown, false},
		{"://nonsense", ProviderUnknown, false},
	}
	for _, tc := range tests {
		got := SourceFor(tc.url)
		if got.Provider != tc.wantProvider {
			t.Errorf("SourceFor(%q).Provider = %q, want %q", tc.url, got.Provider, tc.wantProvider)
		}
		if got.Redistributable != tc.wantRedistributable {
			t.Errorf("SourceFor(%q).Redistributable = %v, want %v",
				tc.url, got.Redistributable, tc.wantRedistributable)
		}
		if got.License == "" {
			t.Errorf("SourceFor(%q) has no license statement", tc.url)
		}
		if got.URL != tc.url {
			t.Errorf("SourceFor(%q).URL = %q", tc.url, got.URL)
		}
	}
	// Deezer imagery is the primary source and is explicitly not
	// redistributable; guard that so nobody flips it casually.
	if SourceFor("https://e-cdns-images.dzcdn.net/x.jpg").Redistributable {
		t.Error("Deezer imagery must not be marked redistributable")
	}
}

// --- CachedFetch matrix --------------------------------------------

func TestCachedFetchMissThenHit(t *testing.T) {
	dir := t.TempDir()
	img := testJPEG(t, 500, 500)
	var calls int32
	f := testFetcher(deezerStub(t, map[string][]byte{"Aphex Twin": img}, &calls))

	got, cached, err := CachedFetch(context.Background(), f, dir, "Aphex Twin", 128, 85)
	if err != nil {
		t.Fatalf("first CachedFetch: %v", err)
	}
	if cached {
		t.Error("first fetch reported a cache hit")
	}
	if len(got) == 0 {
		t.Fatal("no bytes returned")
	}
	afterMiss := atomic.LoadInt32(&calls)
	if afterMiss == 0 {
		t.Error("first fetch made no network calls")
	}

	// Provenance sidecar must exist and classify the source.
	src, err := CachedProvenance(dir, "Aphex Twin")
	if err != nil {
		t.Fatalf("CachedProvenance: %v", err)
	}
	if src.Provider != ProviderDeezer {
		t.Errorf("provider = %q, want %q", src.Provider, ProviderDeezer)
	}
	if src.URL == "" || src.License == "" {
		t.Errorf("incomplete provenance: %+v", src)
	}
	if src.FetchedAt.IsZero() {
		t.Error("provenance has no fetch timestamp")
	}

	// Second call: served from cache, no further network traffic.
	got2, cached2, err := CachedFetch(context.Background(), f, dir, "aphex twin", 128, 85)
	if err != nil {
		t.Fatalf("second CachedFetch: %v", err)
	}
	if !cached2 {
		t.Error("second fetch was not reported as cached")
	}
	if !bytes.Equal(got, got2) {
		t.Error("cached bytes differ from fetched bytes")
	}
	if atomic.LoadInt32(&calls) != afterMiss {
		t.Errorf("cache hit still made network calls (%d -> %d)", afterMiss, atomic.LoadInt32(&calls))
	}
}

func TestCachedFetchNotFoundWritesSentinel(t *testing.T) {
	dir := t.TempDir()
	var calls int32
	f := testFetcher(deezerStub(t, map[string][]byte{}, &calls))

	_, _, err := CachedFetch(context.Background(), f, dir, "Nobody At All", 128, 85)
	if !errors.Is(err, ErrNotFound) {
		t.Fatalf("err = %v, want ErrNotFound", err)
	}
	sentinel := filepath.Join(dir, cacheKey("Nobody At All")+".missing")
	if _, err := os.Stat(sentinel); err != nil {
		t.Fatalf("sentinel not written: %v", err)
	}
	afterMiss := atomic.LoadInt32(&calls)

	_, cached, err := CachedFetch(context.Background(), f, dir, "Nobody At All", 128, 85)
	if !errors.Is(err, ErrNotFound) {
		t.Fatalf("second err = %v, want ErrNotFound", err)
	}
	if !cached {
		t.Error("cached-miss was not reported as cached")
	}
	if atomic.LoadInt32(&calls) != afterMiss {
		t.Error("cached miss still hit the network")
	}
	// A not-found artist must not leave a provenance sidecar.
	if _, err := CachedProvenance(dir, "Nobody At All"); !errors.Is(err, fs.ErrNotExist) {
		t.Errorf("CachedProvenance for a miss = %v, want fs.ErrNotExist", err)
	}
}

// TestCachedFetchNoCacheDirWritesNothing pins the fix for art being
// silently written into the current working directory when the platform
// has no user cache dir.
func TestCachedFetchNoCacheDirWritesNothing(t *testing.T) {
	cwd := t.TempDir()
	orig, err := os.Getwd()
	if err != nil {
		t.Fatalf("getwd: %v", err)
	}
	if err := os.Chdir(cwd); err != nil {
		t.Fatalf("chdir: %v", err)
	}
	t.Cleanup(func() { _ = os.Chdir(orig) })

	img := testJPEG(t, 300, 300)
	var calls int32
	f := testFetcher(deezerStub(t, map[string][]byte{"Aphex Twin": img}, &calls))

	got, cached, err := CachedFetch(context.Background(), f, "", "Aphex Twin", 128, 85)
	if err != nil {
		t.Fatalf("CachedFetch with caching disabled: %v", err)
	}
	if cached {
		t.Error("reported a cache hit with caching disabled")
	}
	if len(got) == 0 {
		t.Error("no bytes returned")
	}
	// And the not-found path must not drop a sentinel either.
	if _, _, err := CachedFetch(context.Background(), f, "", "Nobody At All", 128, 85); !errors.Is(err, ErrNotFound) {
		t.Fatalf("err = %v, want ErrNotFound", err)
	}
	ents, err := os.ReadDir(cwd)
	if err != nil {
		t.Fatalf("readdir: %v", err)
	}
	if len(ents) != 0 {
		names := make([]string, 0, len(ents))
		for _, e := range ents {
			names = append(names, e.Name())
		}
		t.Errorf("caching-disabled fetch littered the working directory: %v", names)
	}
}

func TestCachedProvenanceNoCacheDir(t *testing.T) {
	if _, err := CachedProvenance("", "X"); !errors.Is(err, ErrNoCacheDir) {
		t.Errorf("err = %v, want ErrNoCacheDir", err)
	}
}

// --- rate limiting and retries -------------------------------------

func TestLimiterIntervalFor(t *testing.T) {
	l := newLimiter()
	tests := []struct {
		host string
		want time.Duration
	}{
		{"musicbrainz.org", 1100 * time.Millisecond},
		{"api.deezer.com", 150 * time.Millisecond},
		{"e-cdns-images.dzcdn.net", 150 * time.Millisecond},
		{"upload.wikimedia.org", 250 * time.Millisecond},
		{"commons.wikimedia.org", 250 * time.Millisecond},
		{"example.com", defaultHostInterval},
		{"notmusicbrainz.org", defaultHostInterval},
	}
	for _, tc := range tests {
		if got := l.intervalFor(tc.host); got != tc.want {
			t.Errorf("intervalFor(%q) = %v, want %v", tc.host, got, tc.want)
		}
	}
}

func TestLimiterSpacesRequests(t *testing.T) {
	l := newLimiter()
	l.rules = nil
	l.fallback = 20 * time.Millisecond
	ctx := context.Background()
	start := time.Now()
	for i := 0; i < 4; i++ {
		if err := l.wait(ctx, "example.com"); err != nil {
			t.Fatalf("wait: %v", err)
		}
	}
	// Slots are reserved back-to-back: 4 requests means 3 gaps.
	if elapsed := time.Since(start); elapsed < 50*time.Millisecond {
		t.Errorf("4 requests took %v, want at least 3 x 20ms of pacing", elapsed)
	}
	// A different host has its own budget and must not be delayed.
	start = time.Now()
	if err := l.wait(ctx, "other.example"); err != nil {
		t.Fatalf("wait: %v", err)
	}
	if elapsed := time.Since(start); elapsed > 15*time.Millisecond {
		t.Errorf("unrelated host waited %v; budgets should be per-host", elapsed)
	}
}

func TestLimiterHonorsContext(t *testing.T) {
	l := newLimiter()
	l.rules = nil
	l.fallback = time.Hour
	ctx, cancel := context.WithCancel(context.Background())
	if err := l.wait(ctx, "example.com"); err != nil { // first slot is free
		t.Fatalf("first wait: %v", err)
	}
	go func() {
		time.Sleep(10 * time.Millisecond)
		cancel()
	}()
	start := time.Now()
	if err := l.wait(ctx, "example.com"); !errors.Is(err, context.Canceled) {
		t.Fatalf("wait = %v, want context.Canceled", err)
	}
	if time.Since(start) > time.Second {
		t.Error("cancellation did not interrupt the wait promptly")
	}
}

func TestParseRetryAfter(t *testing.T) {
	if got := parseRetryAfter("5"); got != 5*time.Second {
		t.Errorf("parseRetryAfter(\"5\") = %v", got)
	}
	if got := parseRetryAfter(""); got != 0 {
		t.Errorf("parseRetryAfter(\"\") = %v", got)
	}
	if got := parseRetryAfter("garbage"); got != 0 {
		t.Errorf("parseRetryAfter(garbage) = %v", got)
	}
	if got := parseRetryAfter("-3"); got != 0 {
		t.Errorf("parseRetryAfter(-3) = %v", got)
	}
	future := time.Now().Add(30 * time.Second).UTC().Format(http.TimeFormat)
	if got := parseRetryAfter(future); got < 20*time.Second || got > 31*time.Second {
		t.Errorf("parseRetryAfter(http-date) = %v, want ~30s", got)
	}
	past := time.Now().Add(-time.Hour).UTC().Format(http.TimeFormat)
	if got := parseRetryAfter(past); got != 0 {
		t.Errorf("parseRetryAfter(past date) = %v, want 0", got)
	}
}

func TestBackoffFor(t *testing.T) {
	if d := backoffFor(1, 0); d < baseBackoff || d > baseBackoff+time.Second {
		t.Errorf("backoffFor(1,0) = %v", d)
	}
	if backoffFor(2, 0) <= baseBackoff {
		t.Error("backoff is not growing")
	}
	if d := backoffFor(20, 0); d > maxRetryAfter {
		t.Errorf("backoffFor(20,0) = %v, exceeds the cap", d)
	}
	if d := backoffFor(1, 10*time.Second); d != 10*time.Second {
		t.Errorf("backoffFor honoring Retry-After = %v, want 10s", d)
	}
	if d := backoffFor(1, 10*time.Hour); d != maxRetryAfter {
		t.Errorf("absurd Retry-After = %v, want it clamped to %v", d, maxRetryAfter)
	}
}

func TestRetryableStatus(t *testing.T) {
	for _, code := range []int{429, 500, 502, 503, 504} {
		if !retryableStatus(code) {
			t.Errorf("status %d should be retryable", code)
		}
	}
	for _, code := range []int{200, 301, 400, 401, 403, 404} {
		if retryableStatus(code) {
			t.Errorf("status %d should not be retryable", code)
		}
	}
}

// TestDoRetriesRateLimit is the regression test for "Deezer 429s were
// flattened into a generic error with no retry": a 429 with a
// Retry-After must be waited out and the request re-issued.
func TestDoRetriesRateLimit(t *testing.T) {
	defer func(o time.Duration) { baseBackoff = o }(baseBackoff)
	baseBackoff = time.Millisecond

	var calls int32
	f := testFetcher(func(req *http.Request) (*http.Response, error) {
		if atomic.AddInt32(&calls, 1) == 1 {
			return bytesResponse(req, 429, []byte("slow down"),
				http.Header{"Retry-After": []string{"0"}}), nil
		}
		return bytesResponse(req, 200, []byte("ok"), nil), nil
	})
	body, err := f.httpGet(context.Background(), "https://api.deezer.com/search/artist")
	if err != nil {
		t.Fatalf("httpGet: %v", err)
	}
	if string(body) != "ok" {
		t.Errorf("body = %q", body)
	}
	if got := atomic.LoadInt32(&calls); got != 2 {
		t.Errorf("made %d requests, want 2 (one 429 then a retry)", got)
	}
}

func TestDoGivesUpAfterMaxAttempts(t *testing.T) {
	defer func(o time.Duration) { baseBackoff = o }(baseBackoff)
	baseBackoff = time.Millisecond

	var calls int32
	f := testFetcher(func(req *http.Request) (*http.Response, error) {
		atomic.AddInt32(&calls, 1)
		return bytesResponse(req, 503, []byte("down"), nil), nil
	})
	if _, err := f.httpGet(context.Background(), "https://api.deezer.com/x"); err == nil {
		t.Fatal("want an error after exhausting retries")
	}
	if got := atomic.LoadInt32(&calls); got != maxAttempts {
		t.Errorf("made %d requests, want %d", got, maxAttempts)
	}
}

func TestDoDoesNotRetryClientErrors(t *testing.T) {
	var calls int32
	f := testFetcher(func(req *http.Request) (*http.Response, error) {
		atomic.AddInt32(&calls, 1)
		return bytesResponse(req, 404, []byte("nope"), nil), nil
	})
	if _, err := f.httpGet(context.Background(), "https://api.deezer.com/x"); err == nil {
		t.Fatal("want an error")
	}
	if got := atomic.LoadInt32(&calls); got != 1 {
		t.Errorf("made %d requests, want 1 (404 is not retryable)", got)
	}
}

func TestDoCapsResponseSize(t *testing.T) {
	defer func(o int64) { maxResponseBytes = o }(maxResponseBytes)
	maxResponseBytes = 1024

	f := testFetcher(func(req *http.Request) (*http.Response, error) {
		return bytesResponse(req, 200, bytes.Repeat([]byte("A"), 4096), nil), nil
	})
	_, err := f.httpGet(context.Background(), "https://api.deezer.com/x")
	if !errors.Is(err, errResponseTooLarge) {
		t.Fatalf("err = %v, want errResponseTooLarge", err)
	}
}

func TestDoStopsOnCancelledContext(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	f := testFetcher(func(req *http.Request) (*http.Response, error) {
		t.Error("request issued despite a cancelled context")
		return bytesResponse(req, 200, nil, nil), nil
	})
	if _, err := f.httpGet(ctx, "https://api.deezer.com/x"); !errors.Is(err, context.Canceled) {
		t.Fatalf("err = %v, want context.Canceled", err)
	}
}

func TestFetchClassifiesDeezerProvenance(t *testing.T) {
	img := testJPEG(t, 200, 200)
	var calls int32
	f := testFetcher(deezerStub(t, map[string][]byte{"Aphex Twin": img}, &calls))
	got, src, err := f.Fetch(context.Background(), "Aphex Twin")
	if err != nil {
		t.Fatalf("Fetch: %v", err)
	}
	if !bytes.Equal(got, img) {
		t.Error("Fetch returned different bytes than the source served")
	}
	if src.Provider != ProviderDeezer || src.Redistributable {
		t.Errorf("provenance = %+v", src)
	}
}
