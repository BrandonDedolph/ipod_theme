# tools — host toolchain

Host-side helpers that prepare assets and data the firmware consumes. None
of this runs on the device; it produces the atlases, album art, and library
index that get built into or copied alongside the firmware.

| Tool | What it does |
|---|---|
| `atlas_gen.py` / `atlas_gen.sh` | Pre-rasterize the Nunito TTFs (regular + bold, 9/11/13/17 px) into the `core/ui/atlas/*.h` glyph atlases, and emit `glyphmap.h` (the codepoint → glyph-index table for the non-ASCII glyphs: Latin-1 supplement + smart punctuation + the UI chevrons/middot). Run when the font set or glyph coverage changes; outputs are committed. |
| `build_index.py` | Read a source music tree with `ffprobe` and emit `CORELIB.IDX` — the single-read library index the firmware loads at boot (duration, track/disc, UTF-8 title/artist/album/genre, and a normalized-name hash locator per track). Paths are `--src` / `--out`; ffprobe failures are counted and listed at the end (a non-zero exit), and the record count is checked against the firmware's `LIB_MAX_SONGS`. |
| `artist_genres.json` | The artist → primary genre map `build_index.py` applies when a file's own genre tag is empty or a comma-list. Data, not code — add artists here, not in the script. |
| `make_config.py` | Create (`--create`) the pre-allocated `CORECFG.DAT` settings file in the volume root, and verify (`--verify`, read-only) the absolute LBA the firmware will overwrite. The firmware cannot create files, so this must run once before settings can persist — and `--verify` is the mandatory pre-flight before the device's first write. See `core/docs/design/settings-persistence.md`. |
| `coreart.py` | Extract a FLAC's embedded cover into the CoreArt RGB565 sidecars the firmware blits directly — `folder.art` (120×120, now-playing hero) and `folder.thm` (28×28, list chip). No JPEG decoding on the device. |
| `install_deps.sh` | One-shot install of the build prerequisites (toolchain + SDL2 + Go). |
| `fonts-src/` | Nunito TTF sources (SIL Open Font License 1.1) the atlas generator reads. |

## Regenerating assets

```bash
# Font atlases + glyphmap (after changing fonts or glyph coverage)
bash tools/atlas_gen.sh          # → core/ui/atlas/*.h

# Library index from a music tree
python3 tools/build_index.py --src "/path/to/Music" --out /path/to/CORELIB.IDX
# (both default to the original hardcoded WSL paths, so a bare run still works)

# Album-art sidecars for one album (or --batch a whole tree)
python3 tools/coreart.py --thumb "/path/to/Album - Artist"

# Settings file — ONCE per device, into the volume root
python3 tools/make_config.py --create /mnt/d          # → CORECFG.DAT

# Pre-flight before the device's FIRST write: the LBA this prints must equal
# the one the firmware logs over UART ("core: cfg ... lba XXXXXXXX/YYYYYYYY").
sudo python3 tools/make_config.py --verify /dev/sdX
```

The Python tools need `Pillow` (atlas/art) and `ffmpeg`/`ffprobe` (art/index);
`atlas_gen.sh` sets up a local venv under `tools/.venv`. `atlas_gen.py`
additionally needs **Python 3.12+** (it uses a backslash inside an f-string
expression, legal only since PEP 701) and checks that explicitly rather than
failing as a bare `SyntaxError`.

### The library locator hash

`build_index.py`'s `name_hash()` is duplicated in `core/kernel/main.c`, and the
two **must** agree byte for byte: that hash is the only thing binding an index
record to the file on disk, so a disagreement doesn't error — the affected track
just silently stops appearing. Both sides are asserted against one golden vector
table (`core/tests/kernel/name_hash_vectors.h`) by the `name-hash` and
`hw-name-hash-parity` tests. If you touch either implementation, run:

```bash
python3 core/tests/scripts/check_name_hash_parity.py
```

Known gap: the C side has no 4-byte UTF-8 branch in its re-encoder, so the two
disagree for astral codepoints (emoji). Tracks with one in the name never
resolve on the device. See the note in `name_hash_vectors.h`.
