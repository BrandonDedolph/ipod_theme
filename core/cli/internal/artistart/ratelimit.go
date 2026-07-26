package artistart

import (
	"context"
	"errors"
	"math/rand"
	"net"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"
)

// Per-host minimum spacing between requests.
//
// Every request goes through this, not just the MusicBrainz ones. The
// previous code paced only the two MB calls and let Deezer — the primary
// art source — plus every image download run unthrottled. Deezer throttles
// around 50 requests per 5 seconds per IP, so a few-hundred-artist library
// walked at full speed starts collecting 429s partway through, and those
// failures are not cached, so every rebuild fails identically.
//
// Hosts are matched by suffix so CDN shards (e-cdns-images.dzcdn.net,
// upload.wikimedia.org) inherit their provider's budget.
var hostIntervals = []struct {
	suffix   string
	interval time.Duration
}{
	{"musicbrainz.org", 1100 * time.Millisecond}, // MB policy: 1 req/sec, plus headroom
	{"api.deezer.com", 150 * time.Millisecond},   // ~50 req / 5 s per IP
	{"dzcdn.net", 150 * time.Millisecond},        // Deezer's image CDN
	{"wikidata.org", 250 * time.Millisecond},
	{"wikimedia.org", 250 * time.Millisecond},
}

// defaultHostInterval paces hosts we have no specific budget for. Being
// a good citizen costs a few seconds across a whole library.
const defaultHostInterval = 250 * time.Millisecond

// limiter spaces requests per host. Zero value is usable and unpaced.
type limiter struct {
	mu       sync.Mutex
	next     map[string]time.Time
	fallback time.Duration // per-host interval when no rule matches
	rules    []struct {
		suffix   string
		interval time.Duration
	}
}

func newLimiter() *limiter {
	l := &limiter{
		next:     make(map[string]time.Time),
		fallback: defaultHostInterval,
	}
	l.rules = append(l.rules, hostIntervals...)
	return l
}

func (l *limiter) intervalFor(host string) time.Duration {
	host = strings.ToLower(host)
	if h, _, err := net.SplitHostPort(host); err == nil {
		host = h
	}
	for _, r := range l.rules {
		if host == r.suffix || strings.HasSuffix(host, "."+r.suffix) {
			return r.interval
		}
	}
	return l.fallback
}

// wait blocks until this host's next slot opens, then reserves the one
// after it. Returns ctx.Err() if the caller is cancelled while waiting,
// so a ^C during a paced run aborts promptly.
func (l *limiter) wait(ctx context.Context, host string) error {
	interval := l.intervalFor(host)
	if interval <= 0 {
		return ctx.Err()
	}
	l.mu.Lock()
	if l.next == nil {
		l.next = make(map[string]time.Time)
	}
	now := time.Now()
	at := l.next[host]
	if at.Before(now) {
		at = now
	}
	l.next[host] = at.Add(interval)
	l.mu.Unlock()

	d := time.Until(at)
	if d <= 0 {
		return ctx.Err()
	}
	return sleepCtx(ctx, d)
}

// penalize pushes a host's next slot out by at least d, so a 429's
// Retry-After applies to every subsequent request to that host and not
// just the one being retried.
func (l *limiter) penalize(host string, d time.Duration) {
	if d <= 0 {
		return
	}
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.next == nil {
		l.next = make(map[string]time.Time)
	}
	until := time.Now().Add(d)
	if l.next[host].Before(until) {
		l.next[host] = until
	}
}

func sleepCtx(ctx context.Context, d time.Duration) error {
	if d <= 0 {
		return ctx.Err()
	}
	t := time.NewTimer(d)
	defer t.Stop()
	select {
	case <-t.C:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}

// Retry policy for transient failures. Bounded so a hard outage fails
// the artist rather than hanging the whole build.
//
// baseBackoff is a var rather than a const so tests can shrink it; the
// rest are fixed.
const (
	maxAttempts    = 4
	maxBackoff     = 30 * time.Second
	maxRetryAfter  = 60 * time.Second
	backoffJitterN = 250 // ms of jitter, to avoid lockstep retries
)

var baseBackoff = 500 * time.Millisecond

// backoffFor returns the delay before attempt n (1-based retry count),
// honoring a server-supplied Retry-After when it is larger.
func backoffFor(retry int, retryAfter time.Duration) time.Duration {
	d := baseBackoff << (retry - 1)
	if d > maxBackoff {
		d = maxBackoff
	}
	d += time.Duration(rand.Intn(backoffJitterN)) * time.Millisecond
	if retryAfter > d {
		d = retryAfter
	}
	if d > maxRetryAfter {
		d = maxRetryAfter
	}
	return d
}

// parseRetryAfter understands both forms RFC 7231 allows: delta-seconds
// and an HTTP-date. Returns 0 when absent or unparseable.
func parseRetryAfter(h string) time.Duration {
	h = strings.TrimSpace(h)
	if h == "" {
		return 0
	}
	if secs, err := strconv.Atoi(h); err == nil {
		if secs < 0 {
			return 0
		}
		return time.Duration(secs) * time.Second
	}
	if t, err := http.ParseTime(h); err == nil {
		if d := time.Until(t); d > 0 {
			return d
		}
	}
	return 0
}

// retryableStatus reports whether an HTTP status is worth retrying:
// 429 (rate limited) and 5xx (server-side, usually transient). 4xx
// other than 429 means we asked wrong; retrying won't help.
func retryableStatus(code int) bool {
	return code == http.StatusTooManyRequests || code >= 500
}

// retryableErr reports whether a transport-level failure is worth
// retrying. Timeouts are; context cancellation explicitly is not — the
// user asked us to stop. Callers must additionally check the request
// context, because an http.Client Timeout and a caller-cancelled
// deadline both surface as a net.Error with Timeout() == true.
func retryableErr(err error) bool {
	if err == nil {
		return false
	}
	if errors.Is(err, context.Canceled) {
		return false
	}
	var nerr net.Error
	if errors.As(err, &nerr) {
		return nerr.Timeout()
	}
	return false
}
