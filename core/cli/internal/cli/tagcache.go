package cli

import (
	"errors"
	"fmt"
	"io"
	"os"
	"os/signal"
	"path/filepath"
	"sort"
	"syscall"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/artistart"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/tagcache"
	"github.com/spf13/cobra"
)

// tcdbStatusNote is repeated in the help text and in command output on
// purpose. TCDB is a host-side format that nothing on the device reads
// (the firmware consumes CIDX, written by tools/build_index.py), and
// the previous help text claimed the opposite. Someone copying a .tcdb
// to an iPod and waiting for it to do something is the failure mode
// this note exists to prevent. See internal/tagcache/README.md.
const tcdbStatusNote = "note: .tcdb is a host-side format. The firmware does NOT read it — " +
	"it reads a CIDX index built by tools/build_index.py. Copying a .tcdb to the " +
	"device has no effect. See core/cli/internal/tagcache/README.md."

func newTagcacheCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "tagcache",
		Short: "Build and inspect the host-side binary music index (.tcdb)",
		Long: `Operations on the .tcdb binary music index.

IMPORTANT: .tcdb is a host-side format only. The firmware on the device
does not read it. The index the firmware actually loads is CIDX, built
by tools/build_index.py, and the two formats share neither their magic,
their header, nor their record layout. This command is useful for
host-side experimentation with the index shape; its output is not
something to copy onto an iPod.

Run "core tagcache build <music-dir>" to scan a music directory and emit
a .tcdb, and "core tagcache dump <file>" to decode one.

See core/cli/internal/tagcache/README.md for the full relationship
between TCDB and CIDX.`,
	}
	cmd.AddCommand(newTagcacheBuildCmd())
	cmd.AddCommand(newTagcacheDumpCmd())
	return cmd
}

func newTagcacheDumpCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "dump <file.tcdb>",
		Short: "Decode a .tcdb file and print its contents (debug aid)",
		Args:  cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			data, err := os.ReadFile(args[0])
			if err != nil {
				return err
			}
			m, err := tagcache.Read(data)
			if err != nil {
				return err
			}
			out := cmd.OutOrStdout()
			fmt.Fprintf(out, "Songs (%d):\n", len(m.Songs))
			for i, s := range m.Songs {
				fmt.Fprintf(out,
					"  [%d] %q  artist=%d album=%d genre=%d composer=%d  art=%dB\n",
					i, s.Title,
					m.SongArtistIdx[i], m.SongAlbumIdx[i],
					m.SongGenreIdx[i], m.SongComposerIdx[i],
					len(s.ArtBytes))
			}
			fmt.Fprintf(out, "Artists (%d):   %v\n", len(m.UniqArtists), m.UniqArtists)
			fmt.Fprintf(out, "Albums (%d):    %v\n", len(m.UniqAlbums), m.UniqAlbums)
			fmt.Fprintf(out, "Genres (%d):    %v\n", len(m.UniqGenres), m.UniqGenres)
			fmt.Fprintf(out, "Composers (%d): %v\n", len(m.UniqComposers), m.UniqComposers)
			fmt.Fprintf(out, "Artist groups:   %v\n", m.ArtistGroups)
			fmt.Fprintf(out, "Album groups:    %v\n", m.AlbumGroups)
			fmt.Fprintf(out, "Genre groups:    %v\n", m.GenreGroups)
			fmt.Fprintf(out, "Composer groups: %v\n", m.ComposerGroups)
			return nil
		},
	}
	return cmd
}

func newTagcacheBuildCmd() *cobra.Command {
	var (
		out      string
		force    bool
		fetchArt bool
	)
	cmd := &cobra.Command{
		Use:   "build <music-dir>",
		Short: "Scan a music directory and emit a .tcdb file (host-side only)",
		Long: `Scans a music directory and writes a .tcdb index.

` + tcdbStatusNote,
		Args: cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			dir := args[0]
			if out == "" {
				out = filepath.Join(dir, "tagcache.tcdb")
			}
			songs, err := tagcache.Scan(dir)
			if err != nil {
				return err
			}
			model := tagcache.Build(songs)

			if fetchArt {
				if err := fetchArtistArt(cmd, model); err != nil {
					return err
				}
			}

			// Refuse to overwrite an existing file unless --force, and
			// replace it atomically so an interrupted build can't leave
			// a half-written index in place of a good one.
			if err := writeFileAtomic(out, force,
				func(w io.Writer) error { return model.Write(w) }, nil); err != nil {
				return err
			}
			fi, err := os.Stat(out)
			if err != nil {
				return fmt.Errorf("stat %s: %w", out, err)
			}
			fmt.Fprintf(cmd.OutOrStdout(),
				"%s: %d song%s, %d artist%s, %d album%s, %d genre%s, %d composer%s (%d bytes)\n",
				out,
				len(model.Songs), pl(len(model.Songs)),
				len(model.UniqArtists), pl(len(model.UniqArtists)),
				len(model.UniqAlbums), pl(len(model.UniqAlbums)),
				len(model.UniqGenres), pl(len(model.UniqGenres)),
				len(model.UniqComposers), pl(len(model.UniqComposers)),
				fi.Size(),
			)
			fmt.Fprintln(cmd.OutOrStdout(), tcdbStatusNote)
			return nil
		},
	}
	cmd.Flags().StringVarP(&out, "out", "o", "",
		"Output file path (default: <music-dir>/tagcache.tcdb)")
	cmd.Flags().BoolVarP(&force, "force", "f", false,
		"Overwrite the output file if it already exists")
	cmd.Flags().BoolVar(&fetchArt, "fetch-art", false,
		"Fetch artist photos from Deezer/MusicBrainz/Wikimedia Commons and embed them. "+
			"Opt-in and local-only: every request is rate-limited per host (MusicBrainz 1/sec, "+
			"Deezer ~7/sec) and retried with backoff, so a large library takes minutes on the "+
			"first run and is fast on rebuild via the local cache (~/.cache/core/artist-art). "+
			"The images are third-party works under third-party terms and MUST NOT be bundled "+
			"into a release artifact; provenance is recorded per image in the cache.")
	return cmd
}

// fetchArtistArt walks UniqArtists and populates model.ArtistArt by
// hitting Deezer, then MusicBrainz / Wikidata / Commons for each.
//
// Signal handling: the first SIGINT/SIGTERM cancels the context, which
// aborts the in-flight HTTP request immediately (rather than waiting out
// the 30 s client timeout) and stops the loop; a second one exits the
// process with status 130. The previous version read exactly one signal
// from a 1-slot buffer and returned, leaving signal.Notify registered —
// which disabled Go's default "SIGINT terminates" for the remainder of
// the run and dropped every later signal on the floor, so a second ^C
// did nothing at all.
//
// On a per-artist failure we keep going — the build is allowed to ship
// with fewer artist photos than artists.
func fetchArtistArt(cmd *cobra.Command, model *tagcache.Model) error {
	out := cmd.OutOrStdout()
	cacheDir, err := artistart.DefaultCacheDir()
	if err != nil {
		if !errors.Is(err, artistart.ErrNoCacheDir) {
			return fmt.Errorf("artist-art cache: %w", err)
		}
		// No cache dir on this platform: run with caching disabled
		// rather than writing 40-hex-char files into the CWD.
		cacheDir = ""
		fmt.Fprintln(out, "warning: no user cache dir; artist-art caching is disabled "+
			"and every artist will be re-fetched on each build")
	}

	ctx, stop := signal.NotifyContext(cmd.Context(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	// A second signal must kill us promptly. NotifyContext only cancels
	// its context; it does not restore the default disposition, so
	// without this the process would sit through the rest of the
	// in-flight work no matter how many times the user hit ^C.
	hard := make(chan os.Signal, 1)
	signal.Notify(hard, os.Interrupt, syscall.SIGTERM)
	defer signal.Stop(hard)
	done := make(chan struct{})
	defer close(done)
	go func() {
		seen := 0
		for {
			select {
			case <-hard:
				seen++
				if seen == 1 {
					fmt.Fprintln(out, "\ninterrupt — cancelling in-flight fetch "+
						"(press ^C again to exit immediately)")
					continue
				}
				fmt.Fprintln(out, "\ninterrupt — exiting")
				os.Exit(130)
			case <-done:
				return
			}
		}
	}()

	f := artistart.NewFetcher()
	defer f.Close()

	model.ArtistArt = make([][]byte, len(model.UniqArtists))
	hits, miss, fail := 0, 0, 0
	providers := map[string]int{}
	start := time.Now()
	const maxDim = 128
	const quality = 85
	for i, name := range model.UniqArtists {
		if ctx.Err() != nil {
			fmt.Fprintf(out, "stopped after %d of %d artists\n", i, len(model.UniqArtists))
			break
		}
		bytes, cached, err := artistart.CachedFetch(ctx, f, cacheDir, name, maxDim, quality)
		switch {
		case err == nil:
			model.ArtistArt[i] = bytes
			hits++
			tag := "fetched"
			if cached {
				tag = "cached"
			}
			if src, perr := artistart.CachedProvenance(cacheDir, name); perr == nil {
				providers[src.Provider]++
			}
			fmt.Fprintf(out, "  [%d/%d] %-40s %s (%d B)\n",
				i+1, len(model.UniqArtists), truncate(name, 40), tag, len(bytes))
		case errors.Is(err, artistart.ErrNotFound):
			miss++
			fmt.Fprintf(out, "  [%d/%d] %-40s no photo\n",
				i+1, len(model.UniqArtists), truncate(name, 40))
		case ctx.Err() != nil:
			// Cancelled mid-request; not a real failure.
			fmt.Fprintf(out, "  [%d/%d] %-40s cancelled\n",
				i+1, len(model.UniqArtists), truncate(name, 40))
		default:
			fail++
			fmt.Fprintf(out, "  [%d/%d] %-40s error: %v\n",
				i+1, len(model.UniqArtists), truncate(name, 40), err)
		}
	}
	fmt.Fprintf(out, "artist art: %d ok, %d not-found, %d errors in %s\n",
		hits, miss, fail, time.Since(start).Round(time.Millisecond))
	printArtProvenance(out, providers, cacheDir)
	return nil
}

// printArtProvenance reminds the user what they just downloaded and
// under whose terms. Fetched art is for local use; nothing here is
// ours to redistribute.
func printArtProvenance(out io.Writer, providers map[string]int, cacheDir string) {
	if len(providers) == 0 {
		return
	}
	names := make([]string, 0, len(providers))
	for p := range providers {
		names = append(names, p)
	}
	sort.Strings(names)
	fmt.Fprint(out, "art sources:")
	for _, p := range names {
		fmt.Fprintf(out, " %s=%d", p, providers[p])
	}
	fmt.Fprintln(out)
	fmt.Fprintln(out, "these images are third-party works under third-party terms "+
		"(Deezer press imagery is not freely licensed; Commons images are usually "+
		"CC BY-SA and require attribution).")
	fmt.Fprintln(out, "they are for local use only and must not be bundled into a release artifact.")
	if cacheDir != "" {
		fmt.Fprintf(out, "per-image provenance is recorded in %s (*.json).\n", cacheDir)
	}
}

func truncate(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n-1] + "…"
}

func pl(n int) string {
	if n == 1 {
		return ""
	}
	return "s"
}
