package artistart

import (
	"net/url"
	"strings"
	"time"
)

// Source records where one fetched image came from and under what terms
// it may be used.
//
// This is deliberately persisted next to every cached image. The images
// come from third parties under third-party terms, and a project whose
// whole posture is cleanroom Apache-2.0 cannot afford an index full of
// photos whose origin nobody can reconstruct. Fetch returns it,
// CachedFetch writes it, CachedProvenance reads it back.
type Source struct {
	// URL is the exact URL the bytes were downloaded from.
	URL string `json:"url"`
	// Provider is the service that served (or brokered) the image.
	Provider string `json:"provider"`
	// License is a short human-readable statement of the reuse terms.
	License string `json:"license"`
	// Attribution is what a redistribution would have to carry, or ""
	// when redistribution isn't permitted at all.
	Attribution string `json:"attribution,omitempty"`
	// Redistributable is false when the terms do not permit shipping
	// the image in a release artifact. Deezer imagery is not
	// redistributable; Commons imagery is, with attribution.
	Redistributable bool `json:"redistributable"`
	// FetchedAt is when we downloaded it.
	FetchedAt time.Time `json:"fetched_at"`
}

// Provider strings. Kept as constants because they end up in the
// on-disk sidecars and in the CLI's end-of-run summary.
const (
	ProviderDeezer   = "deezer"
	ProviderCommons  = "wikimedia-commons"
	ProviderUnknown  = "unknown"
	licenseDeezer    = "Deezer API terms — label/press imagery, not a free license; local use only, do not redistribute"
	licenseCommons   = "Wikimedia Commons — typically CC BY-SA; redistribution requires attribution and share-alike"
	licenseUnknown   = "unknown terms — treat as all-rights-reserved; local use only"
	attributionCmns  = "Wikimedia Commons contributors; see the file page for the specific author and license"
	unknownSourceURL = ""
)

// SourceFor classifies a download URL into a provenance record. Host
// matching is by suffix so CDN shards inherit their provider.
func SourceFor(rawURL string) Source {
	s := Source{
		URL:             rawURL,
		Provider:        ProviderUnknown,
		License:         licenseUnknown,
		Redistributable: false,
		FetchedAt:       time.Now().UTC(),
	}
	if rawURL == unknownSourceURL {
		return s
	}
	u, err := url.Parse(rawURL)
	if err != nil {
		return s
	}
	host := strings.ToLower(u.Hostname())
	switch {
	case hostMatches(host, "deezer.com"), hostMatches(host, "dzcdn.net"):
		s.Provider = ProviderDeezer
		s.License = licenseDeezer
		s.Redistributable = false
	case hostMatches(host, "wikimedia.org"), hostMatches(host, "wikipedia.org"):
		s.Provider = ProviderCommons
		s.License = licenseCommons
		s.Attribution = attributionCmns
		// Permitted in principle, but only with the attribution above
		// actually carried. Nothing in this repo does that yet, so the
		// docs say fetched art must not be bundled into a release.
		s.Redistributable = true
	}
	return s
}

func hostMatches(host, suffix string) bool {
	return host == suffix || strings.HasSuffix(host, "."+suffix)
}
