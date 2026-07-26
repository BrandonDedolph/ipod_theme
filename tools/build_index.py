#!/usr/bin/env python3
"""build_index.py — build the device library index (CORELIB.IDX) from a source
music tree.

The iPod's FAT volume is READ-ONLY to the firmware, so the library index can't
be built on-device. Instead this host tool reads each track's tags (ffprobe)
and emits ONE binary index the firmware loads in a single read — no per-file
tag scan at boot. The device falls back to scanning only if this file is absent.

On-disk layout this must agree with: album folders are "Artist - Album", tracks
are "NN. Title.flac", multi-disc flattened. (An earlier version of this
docstring pointed at a companion `import_music.py` for the layout; that script
is not in this repository, so the convention is written out here instead of
being a dangling reference.)

CORELIB.IDX (little-endian):
  header: magic "CIDX"(4), u16 version=1, u16 rec_size=256, u32 count
  record (256 B): u32 duration_s, u16 track, u16 disc,
                  folder[64], file[64], title[48], artist[40], genre[24], pad

Usage:
  build_index.py [--src DIR] [--out FILE] [--genre-map FILE] [--jobs N]

The source and output paths used to be module-level constants pointing at one
machine's WSL mounts, which made the tool unrunnable anywhere else. They are
arguments now; the old values remain the defaults so the existing workflow is
unchanged.
"""
import argparse
import glob
import json
import os
import re
import struct
import subprocess
import sys

# Defaults preserved from when these were hardcoded constants — a personal
# workflow that still works, now overridable.
DEFAULT_SRC = "/mnt/c/Users/brandon-home/Music/MC"
DEFAULT_OUT = "/mnt/c/Users/brandon-home/ipod-staging/CORELIB.IDX"
DEFAULT_GENRE_MAP = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                 "artist_genres.json")

# The firmware's hard cap on index records, read from the firmware rather than
# duplicated here — see read_lib_max_songs().
MAIN_C = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      "..", "core", "kernel", "main.c")

REC_FMT = "<IHH64s64s48s40s24sII"        # 256 bytes (…24s + folder_hash + file_hash)
assert struct.calcsize(REC_FMT) == 256

BAD = re.compile(r'[\\/:*?"<>|]')
def fat_safe(s): return BAD.sub("_", s).rstrip(" .")
def straighten(s): return (s or "").replace('’', "'").replace('‘', "'")

def utf8_field(s, n):
    """Store the name as UTF-8 (the firmware atlas covers Latin-1 + smart
    punctuation now), dropping only C0 control chars. Truncate to n-1 bytes on a
    char boundary (decode('ignore') drops any split trailing sequence)."""
    s = "".join(c for c in (s or "") if ord(c) >= 0x20)
    return s.encode("utf-8")[:n-1].decode("utf-8", "ignore").encode("utf-8")

def norm_key(s):
    """Canonical form for name matching: fold smart quotes/dashes to ASCII and
    lowercase A-Z, so quote-style drift between the index and the on-disk name
    can't break a match. MUST stay identical to name_hash() in kernel/main.c."""
    out = []
    for ch in (s or ""):
        o = ord(ch)
        if   o in (0x2018, 0x2019): ch = "'"
        elif o in (0x201C, 0x201D): ch = '"'
        elif o in (0x2013, 0x2014): ch = "-"
        if "A" <= ch <= "Z": ch = ch.lower()
        out.append(ch)
    return "".join(out)

def load_genre_map(path):
    """Per-artist primary genre, keyed by the folder-derived artist (exact
    match). Was a dict literal in this file; see tools/artist_genres.json for
    why it is data. An empty path means "use tag genres only"."""
    if not path:
        return {}
    with open(path, encoding="utf-8") as fh:
        doc = json.load(fh)
    genres = doc.get("genres", doc)
    if not isinstance(genres, dict):
        raise SystemExit(f"{path}: expected an object of artist -> genre")
    return {k: v for k, v in genres.items() if not k.startswith("_")}


def read_lib_max_songs(path=MAIN_C):
    """The firmware's LIB_MAX_SONGS, read out of kernel/main.c.

    The device stops loading records at this cap, silently — songs past it just
    are not in the library, with no error anywhere. Nothing checked that the
    index we write fits, and duplicating the number here would only move the
    drift. Returns None if main.c can't be read (running outside the repo),
    which downgrades the cap check to a warning."""
    try:
        with open(path, encoding="utf-8") as fh:
            m = re.search(r"^#define\s+LIB_MAX_SONGS\s+(\d+)", fh.read(),
                          re.MULTILINE)
        return int(m.group(1)) if m else None
    except OSError:
        return None

def name_hash(s):
    """FNV-1a 32 over the UTF-8 bytes of norm_key(s). Mirrors name_hash() in
    kernel/main.c byte-for-byte."""
    h = 0x811c9dc5
    for b in norm_key(s).encode("utf-8"):
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h

def split_album_artist(folder):
    i = folder.rfind(" - ")
    return (folder[i+3:].strip(), folder[:i].strip()) if i >= 0 else (None, folder)

def track_title(fname):
    stem = os.path.splitext(fname)[0]
    parts = stem.split(" - ")
    return (" - ".join(parts[2:]) if len(parts) >= 3
            else parts[1] if len(parts) == 2 else stem).strip()

def disc_tracks(adir):
    """Return [(path, disc)] — disc from the 'Disc N' subfolder (ground truth),
    else disc 1 for a flat single-disc album."""
    discs = sorted(d for d in glob.glob(os.path.join(adir, "Disc *")) if os.path.isdir(d))
    if discs:
        out = []
        for d in discs:
            m = re.search(r"Disc\s+(\d+)", os.path.basename(d))
            dn = int(m.group(1)) if m else 1
            for f in sorted(glob.glob(os.path.join(d, "*.flac"))):
                out.append((f, dn))
        return out
    return [(f, 1) for f in sorted(glob.glob(os.path.join(adir, "*.flac")))]

def leadint(s):
    m = re.match(r"\s*(\d+)", s or "")
    return int(m.group(1)) if m else 0

# Tracks whose ffprobe failed, so the run can end loudly instead of quietly
# shipping an index full of zero-length, untitled songs.
PROBE_FAILURES = []


def probe(path):
    """Return (title, artist, album, genre, duration_s, track, disc) from tags.

    On failure returns empty tags and duration 0 — the same fallback as before,
    because a single unreadable file should not abort a 1000-track build — but
    the failure is now RECORDED and reported. Previously a bare
    `except Exception` swallowed everything (ffprobe missing, unparseable JSON,
    a corrupt file) and the tool still printed a success line, so a run could
    produce an index where every duration was 0 and every title came from the
    filename, and look exactly like a good one."""
    try:
        proc = subprocess.run(
            ["ffprobe", "-v", "quiet", "-print_format", "json",
             "-show_format", path],
            capture_output=True, text=True)
    except FileNotFoundError:
        # ffprobe itself is missing: nothing in this run can succeed, so say so
        # once and stop rather than emitting a whole index of empty records.
        raise SystemExit(
            "ffprobe not found on PATH. Install ffmpeg (it provides ffprobe); "
            "without it every track's tags and duration would be blank.")

    if proc.returncode != 0:
        PROBE_FAILURES.append((path, f"ffprobe exit {proc.returncode}"))
        return ("", "", "", "", 0, 0, 0)

    try:
        fmt = json.loads(proc.stdout).get("format", {})
    except json.JSONDecodeError as exc:
        PROBE_FAILURES.append((path, f"unparseable ffprobe output: {exc}"))
        return ("", "", "", "", 0, 0, 0)

    tags = {k.lower(): v for k, v in fmt.get("tags", {}).items()}
    try:
        dur = int(float(fmt.get("duration", 0)))
    except (TypeError, ValueError):
        PROBE_FAILURES.append((path, "no usable duration in ffprobe output"))
        dur = 0
    return (tags.get("title", ""),
            tags.get("artist", tags.get("album_artist", "")),
            tags.get("album", ""), tags.get("genre", ""), dur,
            leadint(tags.get("track", "")), leadint(tags.get("disc", "")))


def parse_args(argv=None):
    p = argparse.ArgumentParser(
        description="Build the device library index (CORELIB.IDX).")
    p.add_argument("--src", default=DEFAULT_SRC,
                   help="source music tree of 'Artist - Album' folders "
                        "(default: %(default)s)")
    p.add_argument("--out", default=DEFAULT_OUT,
                   help="path to write CORELIB.IDX to (default: %(default)s)")
    p.add_argument("--genre-map", default=DEFAULT_GENRE_MAP,
                   help="JSON artist -> genre map; pass '' to use only the "
                        "genres in the files' own tags (default: %(default)s)")
    p.add_argument("--max-songs", type=int, default=None,
                   help="override the firmware's LIB_MAX_SONGS cap check "
                        "(default: read from core/kernel/main.c)")
    p.add_argument("-q", "--quiet", action="store_true",
                   help="don't print a line per album")
    return p.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    src, out_path = args.src, args.out
    artist_genre = load_genre_map(args.genre_map)

    if not os.path.isdir(src):
        raise SystemExit(f"source tree not found: {src}  (pass --src)")

    recs = []
    albums = 0
    for folder in sorted(os.listdir(src)):
        adir = os.path.join(src, folder)
        if not os.path.isdir(adir):
            continue
        artist_f, album_f = split_album_artist(folder)
        if not artist_f:
            continue
        dest = fat_safe(f"{artist_f} - {album_f}")   # FAT-safe: hash + device match
        tracks = disc_tracks(adir)
        if not tracks:
            continue
        folder_hash = name_hash(dest)                 # locator (fat-safe on both sides)
        album_disp = None                             # real album name for DISPLAY
        seen = set()
        albums += 1
        for i, (t, disc) in enumerate(tracks, 1):
            # Destination filename convention: continuous "NN. Title.flac".
            ftitle = fat_safe(track_title(os.path.basename(t)))
            fname = f"{i:02d}. {ftitle}.flac"
            while fname.lower() in seen:
                fname = f"{i:02d}. {ftitle} ({len(seen)}).flac"
            seen.add(fname.lower())
            title, tartist, talbum, genre, dur, ttrk, tdisc = probe(t)
            # Trust structure over tags: disc from the "Disc N" folder, track
            # number from the filename's NN, album/artist from the folder name;
            # title from the tag with a filename fallback.
            trk = leadint(os.path.basename(t)) or ttrk or i
            if not title: title = track_title(os.path.basename(t))
            artist = artist_f or tartist
            # Genre: prefer a clean per-artist genre; only keep the tag genre if
            # the artist isn't mapped AND the tag is a single (non-comma) value.
            mapped = artist_genre.get(artist_f)
            if mapped:
                genre = mapped
            elif genre and "," in genre:
                genre = genre.split(",")[0].strip()
            # DISPLAY folder = real "Artist - Album": use the tag album when it
            # only differs from the folder by FAT sanitization (so ?,*,:,/ come
            # back on screen — the album list & detail header read this field).
            # The locator stays fat_safe via folder_hash, so matching is unaffected.
            # Match tag album to the folder album ignoring apostrophe STYLE (the
            # folder uses straight ', some tags use curly ’); display the tag's
            # real chars but straightened, so ?,*,:,/ return while ' stays ASCII.
            if album_disp is None:
                if talbum and straighten(fat_safe(talbum)) == straighten(album_f):
                    album_disp = straighten(talbum)
                else:
                    album_disp = album_f
            folder_disp = f"{artist_f} - {album_disp}"
            recs.append(struct.pack(
                REC_FMT, dur, trk & 0xFFFF, disc & 0xFFFF,
                utf8_field(folder_disp, 64), utf8_field(fname, 64),
                utf8_field(title, 48), utf8_field(artist, 40),
                utf8_field(genre, 24),
                folder_hash, name_hash(fname)))   # folder + file locators
        if not args.quiet:
            print(f"  {dest}: {len(tracks)}")

    # --- the firmware's record cap -------------------------------------
    # library_load_index() stops at LIB_MAX_SONGS with no error and no log, so
    # everything past it simply is not in the library. Check before writing, so
    # a too-large index is a message here rather than a mystery on the device.
    cap = args.max_songs if args.max_songs is not None else read_lib_max_songs()
    if cap is None:
        print("warning: could not read LIB_MAX_SONGS from core/kernel/main.c; "
              "the record-count check was skipped", file=sys.stderr)
    elif len(recs) > cap:
        raise SystemExit(
            f"{len(recs)} records exceeds the firmware's LIB_MAX_SONGS ({cap}).\n"
            f"The device would load the first {cap} and silently drop "
            f"{len(recs) - cap} tracks — no error, they just would not appear "
            f"in the library.\nRaise LIB_MAX_SONGS in core/kernel/main.c — it "
            f"costs roughly 180 bytes of .bss per song (lib_song_t plus the "
            f"two index arrays), and .bss is budgeted, so check "
            f"core/tests/scripts/check_size.sh — or reduce the source tree.")
    elif cap and len(recs) > cap * 9 // 10:
        print(f"warning: {len(recs)} records is within 10% of the firmware's "
              f"LIB_MAX_SONGS ({cap})", file=sys.stderr)

    out_dir = os.path.dirname(out_path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(b"CIDX" + struct.pack("<HHI", 1, 256, len(recs)))
        for r in recs:
            f.write(r)

    print(f"\nCORELIB.IDX: {len(recs)} songs from {albums} albums, "
          f"{os.path.getsize(out_path)} bytes -> {out_path}")

    # --- report the failures instead of shipping them ------------------
    if PROBE_FAILURES:
        print(f"\n{len(PROBE_FAILURES)} track(s) could not be probed. Their "
              f"records carry duration 0 and a filename-derived title, which "
              f"is exactly what a healthy index looks like from the device's "
              f"side — so this list is the only warning you get:",
              file=sys.stderr)
        for path, why in PROBE_FAILURES:
            print(f"  {path}: {why}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
