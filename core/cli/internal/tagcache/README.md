# `internal/tagcache` — TCDB, and its relationship to CIDX

**Status: host-side only. The firmware does not read anything this
package produces.**

This document exists because the package used to claim otherwise, in
five separate places, and someone acting on those claims would have
copied a `.tcdb` onto an iPod and waited for something to happen.

## The two formats

| | **CIDX** (shipping) | **TCDB** (this package) |
|---|---|---|
| Written by | `tools/build_index.py` | `core tagcache build` |
| Read by | `library_load_index()` in `core/kernel/main.c` | `tagcache.Read` (host, tests, `core tagcache dump`) |
| On-device name | `Music/CORELIB.IDX` | — never loaded — |
| Magic | `"CIDX"` | `"TCDB"` |
| Header | 12 bytes: magic, `u16 version=1`, `u16 rec_size=256`, `u32 count` | 156 bytes (`HeaderSize`), version 2 |
| Records | fixed 256 B, `<IHH64s64s48s40s24sII` | variable-length sections + 40 B song records |
| Per-track fields | duration, track no., disc no., folder, file, title, artist, genre, folder hash, file hash | title, path, artist/album/genre/composer indices, embedded art extent |
| File locator | FNV-1a hashes of normalized folder + file names | host absolute path |
| Album art | none in the index (`folder.art` alongside the album) | embedded JPEG blob, deduped by content |
| Artist photos | none | optional blob (`--fetch-art`) |

The firmware's check is literal and unforgiving
(`core/kernel/main.c`, `library_load_index`):

```c
if (hdr[0] != 'C' || hdr[1] != 'I' || hdr[2] != 'D' || hdr[3] != 'X') return 0;
```

A `.tcdb` fails that on byte 0, `library_load_index` returns 0, and the
device silently falls back to a directory scan. No error, no log line,
no clue — which is exactly why this was worth writing down.

## Why the gap is not a small one

It is not a matter of changing the magic bytes. Three things are
missing before this package could emit a device-loadable index:

1. **Fields.** `SongInfo` (`scan.go`) carries no duration, no track
   number, no disc number. CIDX records all three, and the UI shows
   them.
2. **Locators.** The firmware finds a file by FNV-1a hashes of the
   *normalized* folder and file names — normalization folds smart
   quotes and dashes to ASCII and lowercases, so quote-style drift
   between index and filesystem doesn't break matching (see `norm_key`
   and `name_hash` in `tools/build_index.py`). This package keys songs
   by host absolute path, which is meaningless on the device.
3. **Layout assumptions.** CIDX presumes the on-device layout
   `import_music.py` produces: album folders named `Artist - Album`,
   tracks named `NN. Title.flac`, multi-disc flattened. `Scan` walks an
   arbitrary tree.

So `Scan` -> CIDX is not a port, it is a rewrite of the scanner against
a different set of requirements.

## Why the package is still here

The encoder/decoder is decent work and the format is a genuinely richer
design than CIDX: a deduped string table, uniq tables per dimension with
precomputed per-group song lists (so a drilldown is a slice, not a
filter), embedded art deduped by content, and optional per-artist
photos. `decode.go`'s bounds checking is more careful than anything on
the shipping path — every offset and length off disk is validated
against both the file length and its parent section's length before it
is used to slice, and `TestReadCorrupt` pins that. If the device index
ever needs to grow past 256-byte fixed records, this is the starting
point.

What it must not be is *described* as something the device consumes.

## If you do decide to converge them

Sketch, in the order the work has to happen:

1. Extend `SongInfo` + `readTags` with duration, track, disc.
2. Add the normalization + FNV-1a hashing from `tools/build_index.py`,
   and test the two implementations agree on a corpus — the
   normalization is the part that silently breaks lookups when it
   drifts.
3. Decide whether the firmware gains a TCDB reader or this package
   gains a CIDX encoder. The latter is far less work and does not put
   new parsing code on the device, where a malformed index is a crash
   at boot rather than a failed build.
4. Whichever way: one writer, one reader, one format. Two index
   formats in a repo is how this situation arose.

## Artist art and licensing

`--fetch-art` embeds artist photos fetched from Deezer, MusicBrainz,
Wikidata and Wikimedia Commons. Those are third-party works under
third-party terms — Deezer's imagery is press/label material under
their API terms and is *not* freely licensed; Commons imagery is
typically CC BY-SA and requires attribution. The fetch is opt-in and
local-only, provenance is recorded per image in the cache
(`~/.cache/core/artist-art/*.json`), and fetched art must never be
bundled into a release artifact. See `internal/artistart` for details.
