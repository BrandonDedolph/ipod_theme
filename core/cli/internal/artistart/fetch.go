// Package artistart fetches artist photos from public music databases
// for the host-side tagcache.
//
// This is opt-in (`core tagcache build --fetch-art`) and local-only. The
// images it retrieves are third-party works under third-party terms —
// see LICENSING below — and must never be bundled into a release
// artifact.
//
// Chain:
//
//  1. Deezer artist search             artist name  -> picture URL
//  2. MusicBrainz artist search        artist name  -> MBID
//  3. MusicBrainz artist lookup        MBID         -> Wikidata QID (via url-rels)
//  4. Wikidata entity data             QID          -> Wikimedia Commons filename (P18)
//  5. Wikimedia Commons file URL       filename     -> actual image URL
//  6. HTTP GET that URL                             -> JPEG/PNG bytes
//
// Every request — Deezer, MusicBrainz, Wikidata, Commons, and the image
// downloads — is paced by one shared per-host limiter (see ratelimit.go)
// and retried with bounded exponential backoff on 429/5xx/timeout,
// honoring Retry-After. MusicBrainz's stated budget is 1 req/sec;
// Deezer's is roughly 50 requests per 5 seconds per IP. Plan for a few
// minutes per fresh fetch of a large library; cache aggressively.
//
// # LICENSING
//
// Nothing this package downloads is ours, and most of it is not freely
// licensed:
//
//   - Deezer's picture_big is label / press imagery served under
//     Deezer's API terms. It is not under a free license and may not be
//     redistributed.
//   - Wikimedia Commons images are mostly CC BY-SA, which permits reuse
//     but *requires* attribution and share-alike.
//
// So each cached image is stored with a provenance sidecar recording
// the source URL, provider and license terms (see Source and
// CachedProvenance). This project ships Apache-2.0 cleanroom code; an
// index full of unattributed third-party photos would be a real
// licensing hazard, and the sidecar is what makes the obligation
// auditable rather than invisible.
//
// Design choices:
//
//   - We do not require an API key anywhere. Deezer, MusicBrainz,
//     Wikidata, and Commons are all free + token-less; trading the
//     polish of keyed APIs (Last.fm, Spotify, fanart.tv) for a simpler
//     distribution story.
//   - The User-Agent string identifies us per MusicBrainz's policy so
//     we don't get rate-limit-blocked harder than usual. Bump the
//     version when the project name changes.
//   - The fetcher returns raw image bytes (whatever the source serves —
//     typically JPEG, sometimes PNG). Callers downscale + recompress
//     before storing.
//   - Failures are not fatal: an artist with no match anywhere returns
//     ErrNotFound and the build continues. The user sees fewer artist
//     thumbs, not a build break.
package artistart

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"
)

// ErrNotFound is returned when any step in the fetch chain comes up
// empty (no MB match, no Wikidata link, no P18, etc.). Distinct from
// network / parse errors so callers can render a graceful "no photo"
// outcome rather than aborting the whole build.
var ErrNotFound = errors.New("artistart: not found")

// userAgent satisfies MusicBrainz's policy that scripted clients must
// identify themselves. They suggest "Application/Version (contact)".
// Keep this stable so MB's per-UA budgets aren't double-counted across
// runs.
const userAgent = "core-tagcache/0.1 (https://github.com/BrandonDedolph/ipod_theme)"

// Fetcher runs the fetch chain. Every outbound request shares one
// per-host rate limiter and one http.Client (hence one connection pool
// and one timeout budget).
type Fetcher struct {
	HTTP  *http.Client
	limit *limiter
}

// NewFetcher returns a Fetcher with sensible defaults: 30 s per-request
// HTTP timeout and per-host pacing per hostIntervals.
func NewFetcher() *Fetcher {
	return &Fetcher{
		HTTP:  &http.Client{Timeout: 30 * time.Second},
		limit: newLimiter(),
	}
}

// Close releases pooled connections. Idempotent.
func (f *Fetcher) Close() {
	if f.HTTP != nil {
		f.HTTP.CloseIdleConnections()
	}
}

// Fetch runs the full chain for `artistName` and returns the raw image
// bytes the source served, plus that image's provenance. Returns
// ErrNotFound when every available source comes up empty.
//
// Order of attempts:
//
//  1. Deezer's public Search API — no auth, broad coverage of the
//     modern catalog. One JSON call + one image download.
//  2. MusicBrainz → Wikidata → Commons — kept as a backstop for the
//     rare niche artist Deezer doesn't index.
//
// Either source can fail without aborting; we only return the
// originating error when *both* paths fail.
//
// The returned Source is not decoration: the two paths carry materially
// different reuse terms (see the package LICENSING notes), and callers
// are expected to persist it alongside the bytes.
func (f *Fetcher) Fetch(ctx context.Context, artistName string) ([]byte, Source, error) {
	if imgURL, err := f.fetchDeezer(ctx, artistName); err == nil {
		bytes, derr := f.httpGet(ctx, imgURL)
		if derr == nil {
			return bytes, SourceFor(imgURL), nil
		}
		/* Download failure on a Deezer URL — surprising but possible
		 * (CDN hiccup). Fall through to MB rather than giving up. */
	}
	mbid, err := f.searchMBID(ctx, artistName)
	if err != nil {
		return nil, Source{}, fmt.Errorf("mb search %q: %w", artistName, err)
	}
	qid, err := f.lookupWikidataQID(ctx, mbid)
	if err != nil {
		return nil, Source{}, fmt.Errorf("mb lookup mbid=%s: %w", mbid, err)
	}
	filename, err := f.wikidataImageFilename(ctx, qid)
	if err != nil {
		return nil, Source{}, fmt.Errorf("wikidata qid=%s: %w", qid, err)
	}
	imgURL, err := f.commonsImageURL(ctx, filename)
	if err != nil {
		return nil, Source{}, fmt.Errorf("commons file=%s: %w", filename, err)
	}
	bytes, err := f.httpGet(ctx, imgURL)
	if err != nil {
		return nil, SourceFor(imgURL), fmt.Errorf("download %s: %w", imgURL, err)
	}
	return bytes, SourceFor(imgURL), nil
}

// searchMBID asks MB for the artist's MBID. Picks the top score match;
// MB's scoring is reasonably accurate for canonical names.
func (f *Fetcher) searchMBID(ctx context.Context, name string) (string, error) {
	q := url.Values{}
	q.Set("query", "artist:"+name)
	q.Set("fmt", "json")
	q.Set("limit", "1")
	u := "https://musicbrainz.org/ws/2/artist/?" + q.Encode()

	body, err := f.httpGet(ctx, u)
	if err != nil {
		return "", err
	}
	var resp struct {
		Artists []struct {
			ID    string `json:"id"`
			Score int    `json:"score"`
			Name  string `json:"name"`
		} `json:"artists"`
	}
	if err := json.Unmarshal(body, &resp); err != nil {
		return "", fmt.Errorf("parse: %w", err)
	}
	if len(resp.Artists) == 0 {
		return "", ErrNotFound
	}
	a := resp.Artists[0]
	/* MB's score is 0..100. Anything below ~85 is usually a wrong
	 * artist (e.g. matching a remix of the name). The rate-limit cost
	 * of guessing wrong is high, so be picky. */
	if a.Score < 85 {
		return "", ErrNotFound
	}
	return a.ID, nil
}

// lookupWikidataQID fetches the artist's external relationships and
// extracts the wikidata QID (a Q-prefixed integer like Q23426).
func (f *Fetcher) lookupWikidataQID(ctx context.Context, mbid string) (string, error) {
	u := "https://musicbrainz.org/ws/2/artist/" + mbid + "?fmt=json&inc=url-rels"
	body, err := f.httpGet(ctx, u)
	if err != nil {
		return "", err
	}
	var resp struct {
		Relations []struct {
			Type string `json:"type"`
			URL  struct {
				Resource string `json:"resource"`
			} `json:"url"`
		} `json:"relations"`
	}
	if err := json.Unmarshal(body, &resp); err != nil {
		return "", fmt.Errorf("parse: %w", err)
	}
	for _, r := range resp.Relations {
		if r.Type == "wikidata" {
			/* URL looks like https://www.wikidata.org/wiki/Q23426 */
			i := strings.LastIndex(r.URL.Resource, "/")
			if i >= 0 && i+1 < len(r.URL.Resource) {
				return r.URL.Resource[i+1:], nil
			}
		}
	}
	return "", ErrNotFound
}

// wikidataImageFilename pulls the value of the P18 ("image") claim
// from the entity. P18's mainsnak.datavalue.value is the bare filename
// on Commons (no "File:" prefix), which is what the Commons file API
// expects.
//
// Wikidata's `datavalue.value` is polymorphic — string for
// commonsmedia, object for time / globe-coords / wikibase-entityid /
// etc. We only care about P18 (commonsmedia) so we capture every
// claim's value as RawMessage and only attempt the string decode for
// P18's first entry. That sidesteps Go's strict-typed unmarshal
// failing on the very first foreign-typed datavalue it walks past.
func (f *Fetcher) wikidataImageFilename(ctx context.Context, qid string) (string, error) {
	u := "https://www.wikidata.org/wiki/Special:EntityData/" + qid + ".json"
	body, err := f.httpGet(ctx, u)
	if err != nil {
		return "", err
	}
	var resp struct {
		Entities map[string]struct {
			Claims map[string][]struct {
				MainSnak struct {
					DataValue struct {
						Value json.RawMessage `json:"value"`
					} `json:"datavalue"`
				} `json:"mainsnak"`
			} `json:"claims"`
		} `json:"entities"`
	}
	if err := json.Unmarshal(body, &resp); err != nil {
		return "", fmt.Errorf("parse: %w", err)
	}
	ent, ok := resp.Entities[qid]
	if !ok {
		return "", ErrNotFound
	}
	p18, ok := ent.Claims["P18"]
	if !ok || len(p18) == 0 {
		return "", ErrNotFound
	}
	var name string
	if err := json.Unmarshal(p18[0].MainSnak.DataValue.Value, &name); err != nil {
		return "", fmt.Errorf("decode P18 value: %w", err)
	}
	if name == "" {
		return "", ErrNotFound
	}
	return name, nil
}

// commonsImageURL resolves a Commons filename to its hosted URL.
// We use the imageinfo API with iiurlwidth so Commons returns a
// downscaled thumbnail rather than a multi-megabyte original.
func (f *Fetcher) commonsImageURL(ctx context.Context, filename string) (string, error) {
	q := url.Values{}
	q.Set("action", "query")
	q.Set("titles", "File:"+filename)
	q.Set("prop", "imageinfo")
	q.Set("iiprop", "url")
	q.Set("iiurlwidth", "512")     /* max width; Commons returns a sized thumb */
	q.Set("format", "json")
	u := "https://commons.wikimedia.org/w/api.php?" + q.Encode()

	body, err := f.httpGet(ctx, u)
	if err != nil {
		return "", err
	}
	var resp struct {
		Query struct {
			Pages map[string]struct {
				ImageInfo []struct {
					URL       string `json:"url"`
					ThumbURL  string `json:"thumburl"`
				} `json:"imageinfo"`
			} `json:"pages"`
		} `json:"query"`
	}
	if err := json.Unmarshal(body, &resp); err != nil {
		return "", fmt.Errorf("parse: %w", err)
	}
	for _, page := range resp.Query.Pages {
		if len(page.ImageInfo) == 0 {
			continue
		}
		ii := page.ImageInfo[0]
		if ii.ThumbURL != "" {
			return ii.ThumbURL, nil
		}
		if ii.URL != "" {
			return ii.URL, nil
		}
	}
	return "", ErrNotFound
}

// httpGet does a GET with the project User-Agent, returns the body
// bytes. Non-2xx responses become errors.
func (f *Fetcher) httpGet(ctx context.Context, u string) ([]byte, error) {
	req, err := http.NewRequestWithContext(ctx, "GET", u, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("User-Agent", userAgent)
	return f.do(req)
}

// maxResponseBytes caps how much we will read from any one response.
// Nothing in this chain is legitimately large: the JSON payloads are
// kilobytes and the images are a few hundred KB. Without a cap, a
// misrouted or hostile response can drive an unbounded allocation.
//
// A var rather than a const so tests can shrink it.
var maxResponseBytes int64 = 16 << 20

// errResponseTooLarge is returned when a response exceeds maxResponseBytes.
var errResponseTooLarge = errors.New("artistart: response exceeds size limit")

// do executes a prepared request, pacing it against the per-host rate
// limiter and retrying transient failures with bounded exponential
// backoff. Non-2xx responses become errors.
//
// All requests go through here, which is the point: previously only the
// two MusicBrainz calls were paced, and a 429 from Deezer was flattened
// into a generic error with no Retry-After handling, no backoff and no
// retry — and unlike ErrNotFound those failures are not cached, so
// every rebuild re-failed identically.
func (f *Fetcher) do(req *http.Request) ([]byte, error) {
	ctx := req.Context()
	host := req.URL.Hostname()

	var lastErr error
	for attempt := 1; attempt <= maxAttempts; attempt++ {
		if f.limit != nil {
			if err := f.limit.wait(ctx, host); err != nil {
				return nil, err
			}
		}
		body, status, retryAfter, err := f.attempt(req.Clone(ctx))
		switch {
		case err == nil && status/100 == 2:
			return body, nil
		case err != nil:
			lastErr = err
			if ctx.Err() != nil {
				return nil, ctx.Err()
			}
			if !retryableErr(err) {
				return nil, err
			}
		default:
			snippet := string(body)
			if len(snippet) > 200 {
				snippet = snippet[:200]
			}
			lastErr = fmt.Errorf("http %d: %s", status, snippet)
			if !retryableStatus(status) {
				return nil, lastErr
			}
			// A Retry-After applies to the whole host, not just this
			// request; push everyone else's next slot out too.
			if f.limit != nil && retryAfter > 0 {
				f.limit.penalize(host, retryAfter)
			}
		}
		if attempt == maxAttempts {
			break
		}
		if err := sleepCtx(ctx, backoffFor(attempt, retryAfter)); err != nil {
			return nil, err
		}
	}
	return nil, fmt.Errorf("%s: giving up after %d attempts: %w", host, maxAttempts, lastErr)
}

// attempt performs one request round trip, reading at most
// maxResponseBytes of the body.
func (f *Fetcher) attempt(req *http.Request) (body []byte, status int, retryAfter time.Duration, err error) {
	resp, err := f.HTTP.Do(req)
	if err != nil {
		return nil, 0, 0, err
	}
	defer resp.Body.Close()
	body, err = io.ReadAll(io.LimitReader(resp.Body, maxResponseBytes+1))
	if err != nil {
		return nil, resp.StatusCode, 0, err
	}
	if int64(len(body)) > maxResponseBytes {
		return nil, resp.StatusCode, 0, fmt.Errorf("%w (%d bytes) from %s",
			errResponseTooLarge, maxResponseBytes, req.URL.Host)
	}
	return body, resp.StatusCode, parseRetryAfter(resp.Header.Get("Retry-After")), nil
}
