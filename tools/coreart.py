#!/usr/bin/env python3
"""coreart.py — extract embedded FLAC album art into a CoreArt RGB565 sidecar.

The firmware can't decode JPEG on the FPU-less ARM7 in real time, so album art
is pre-rendered on the host to a fixed-size RGB565 bitmap the device just blits.
This is the reference implementation of that step (the companion loader app will
do the same). ffmpeg does the heavy lifting: it exposes a FLAC's embedded
PICTURE block as a video stream, and can scale + convert to raw rgb565le in one
pass.

Two sidecar sizes ship per album:
    folder.art  — full-size cover (default 120x120), now-playing screen
    folder.thm  — small thumbnail (default  28x28), list chip / album-detail

Both use the SAME CoreArt container; only the width/height differ.

CoreArt sidecar format (little-endian):
    off 0  : magic  "CART"        (4 bytes)
    off 4  : u16    version = 1
    off 6  : u16    width
    off 8  : u16    height
    off 10 : u16    reserved = 0
    off 12 : width*height * u16   RGB565 pixels, row-major (rgb565le)

Usage:
    coreart.py <input.flac> <output.art> [size]        # one file -> .art
    coreart.py --album <folder> <output.art> [size]    # first FLAC in a folder -> .art
    coreart.py --thumb <folder> [art_size] [thm_size]  # folder.art + folder.thm in <folder>
    coreart.py --batch <root>  [art_size] [thm_size]   # every subfolder -> folder.art + .thm

Defaults: art_size = 120, thm_size = 28. (thm_size is 28 because it must match
the firmware's ARTCACHE_DIM exactly — at the exact size the device blits the
chip with no on-device resample. This line used to say 24, contradicting the
THM_SIZE_DEFAULT constant twelve lines below it; the constant was right.)
"""
import sys, subprocess, struct, glob, os

MAGIC = b"CART"
VERSION = 1

ART_SIZE_DEFAULT = 120
THM_SIZE_DEFAULT = 28   # matches firmware ARTCACHE_DIM: exact-size = no on-device resample


def extract(flac_path, size):
    try:
        proc = subprocess.run(
            ["ffmpeg", "-v", "error", "-i", flac_path, "-an", "-map", "0:v:0",
             "-vf", f"scale={size}:{size}:flags=lanczos",
             "-pix_fmt", "rgb565le", "-f", "rawvideo", "-"],
            capture_output=True)
    except FileNotFoundError:
        raise SystemExit("ffmpeg not found on PATH — it does the decode, "
                         "scale and RGB565 conversion here.")

    # ffmpeg's exit status was previously ignored entirely: only the output
    # LENGTH was checked. That conflates every distinct failure (unreadable
    # file, no embedded PICTURE block, a bad filter string, ffmpeg killed) into
    # one "no embedded art?" guess, and would have accepted a truncated stream
    # outright had it happened to be the right length. Report what actually
    # went wrong, including ffmpeg's own message.
    if proc.returncode != 0:
        err = proc.stderr.decode("utf-8", "replace").strip()
        raise RuntimeError(f"{flac_path}: ffmpeg exited {proc.returncode}"
                           + (f"\n{err}" if err else ""))

    raw = proc.stdout
    if len(raw) != size * size * 2:
        err = proc.stderr.decode("utf-8", "replace").strip()
        raise RuntimeError(f"{flac_path}: got {len(raw)} bytes, "
                           f"expected {size*size*2} (no embedded art?)"
                           + (f"\n{err}" if err else ""))
    return raw


def write_art(out_path, raw, size):
    hdr = MAGIC + struct.pack("<HHHH", VERSION, size, size, 0)
    with open(out_path, "wb") as f:
        f.write(hdr + raw)
    print(f"{out_path}: {size}x{size} ({len(hdr)+len(raw)} bytes)")


def first_flac(folder):
    """First FLAC in a folder (sorted, case-insensitive .flac/.FLAC), or None."""
    flacs = sorted(glob.glob(os.path.join(folder, "*.flac")) +
                   glob.glob(os.path.join(folder, "*.FLAC")))
    return flacs[0] if flacs else None


def write_pair(folder, art_size, thm_size):
    """Extract from the folder's first FLAC and write BOTH folder.art (art_size)
    and folder.thm (thm_size). Returns True on success, False if the folder has
    no FLAC or no embedded art (either way a one-line summary is printed)."""
    src = first_flac(folder)
    if src is None:
        print(f"{folder}: skip (no FLAC)")
        return False
    try:
        write_art(os.path.join(folder, "folder.art"), extract(src, art_size), art_size)
        write_art(os.path.join(folder, "folder.thm"), extract(src, thm_size), thm_size)
    except RuntimeError as e:
        print(f"{folder}: skip ({e})")
        return False
    print(f"{folder}: ok ({os.path.basename(src)} -> "
          f"folder.art {art_size}x{art_size} + folder.thm {thm_size}x{thm_size})")
    return True


def batch(root, art_size, thm_size):
    """Walk every immediate subfolder of <root>; emit an art/thm pair for each
    folder that has a FLAC with embedded art. Prints a per-folder summary."""
    subs = sorted(d.path for d in os.scandir(root) if d.is_dir())
    made = 0
    for folder in subs:
        if write_pair(folder, art_size, thm_size):
            made += 1
    print(f"{root}: {made}/{len(subs)} folder(s) got art")


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__); sys.exit(2)

    if args[0] == "--batch":
        root = args[1]
        art_size = int(args[2]) if len(args) > 2 else ART_SIZE_DEFAULT
        thm_size = int(args[3]) if len(args) > 3 else THM_SIZE_DEFAULT
        batch(root, art_size, thm_size)
        return

    if args[0] == "--thumb":
        folder = args[1]
        art_size = int(args[2]) if len(args) > 2 else ART_SIZE_DEFAULT
        thm_size = int(args[3]) if len(args) > 3 else THM_SIZE_DEFAULT
        write_pair(folder, art_size, thm_size)
        return

    if args[0] == "--album":
        folder, out = args[1], args[2]
        size = int(args[3]) if len(args) > 3 else ART_SIZE_DEFAULT
        src = first_flac(folder)
        if src is None:
            raise RuntimeError(f"no FLAC in {folder}")
    else:
        src, out = args[0], args[1]
        size = int(args[2]) if len(args) > 2 else ART_SIZE_DEFAULT

    write_art(out, extract(src, size), size)


if __name__ == "__main__":
    main()
