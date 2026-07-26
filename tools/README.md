# tools — host toolchain

Host-side helpers that prepare assets and data the firmware consumes. None
of this runs on the device; it produces the atlases, album art, and library
index that get built into or copied alongside the firmware.

| Tool | What it does |
|---|---|
| `atlas_gen.py` / `atlas_gen.sh` | Pre-rasterize the Nunito TTFs (regular + bold, 9/11/13/17 px) into the `core/ui/atlas/*.h` glyph atlases, and emit `glyphmap.h` (the codepoint → glyph-index table for the non-ASCII glyphs: Latin-1 supplement + smart punctuation + the UI chevrons/middot). Run when the font set or glyph coverage changes; outputs are committed. |
| `build_index.py` | Read a source music tree with `ffprobe` and emit `CORELIB.IDX` — the single-read library index the firmware loads at boot (duration, track/disc, UTF-8 title/artist/album/genre, and a normalized-name hash locator per track). Also maps each artist to a clean primary genre. |
| `make_config.py` | Create (`--create`) the pre-allocated `CORECFG.DAT` settings file in the volume root, and verify (`--verify`, read-only) the absolute LBA the firmware will overwrite. The firmware cannot create files, so this must run once before settings can persist — and `--verify` is the mandatory pre-flight before the device's first write. See `core/docs/design/settings-persistence.md`. |
| `coreart.py` | Extract a FLAC's embedded cover into the CoreArt RGB565 sidecars the firmware blits directly — `folder.art` (120×120, now-playing hero) and `folder.thm` (28×28, list chip). No JPEG decoding on the device. |
| `install_deps.sh` | One-shot install of the build prerequisites (toolchain + SDL2 + Go). |
| `fonts-src/` | Nunito TTF sources (SIL Open Font License 1.1) the atlas generator reads. |

## Regenerating assets

```bash
# Font atlases + glyphmap (after changing fonts or glyph coverage)
bash tools/atlas_gen.sh          # → core/ui/atlas/*.h

# Library index from a music tree (edit SRC/OUT at the top of the file)
python3 tools/build_index.py     # → CORELIB.IDX

# Album-art sidecars for one album (or --batch a whole tree)
python3 tools/coreart.py --thumb "/path/to/Album - Artist"

# Settings file — ONCE per device, into the volume root
python3 tools/make_config.py --create /mnt/d          # → CORECFG.DAT

# Pre-flight before the device's FIRST write: the LBA this prints must equal
# the one the firmware logs over UART ("core: cfg ... lba XXXXXXXX/YYYYYYYY").
sudo python3 tools/make_config.py --verify /dev/sdX
```

The Python tools need `Pillow` (atlas/art) and `ffmpeg`/`ffprobe` (art/index);
`atlas_gen.sh` sets up a local venv under `tools/.venv`.
