#!/usr/bin/env python3
"""
tools/text_metrics.py — measure letter spacing objectively, from the SHIPPED
atlases, using the device's own pen arithmetic.

Why this exists: "looks cramped" and "looks airy" are not actionable, and a
zoomed PNG cannot tell you WHICH pairs are wrong. Uniform tracking cannot fix a
per-pair problem, so before changing a global number it is worth knowing
whether the complaint is the distribution or a handful of outliers.

What it measures: for every ordered letter pair, the horizontal daylight
between the two glyphs' ACTUAL INK — read out of the baked alpha bitmaps at a
coverage threshold — after placing them exactly as core/ui/text.c does:

    pen = 0                       -> x_a = round(pen/64) + offset_x[a]
    pen += tracking (unless space)
    pen += kern[a][b] * 2         (1/32px -> 1/64px)
    pen += advance[a]             -> x_b = round(pen/64) + offset_x[b]

Reading the bitmaps rather than the bboxes matters: ~37% of nonzero pixels in
these atlases are sub-25%-alpha fringe, so a bbox-based gap says "touching"
while the visible stems are still a pixel apart. Two thresholds are reported:
CORE (>=50% alpha, what the eye reads as the letter) and SOFT (>0, any ink).

Usage:
    tools/.venv/bin/python3 tools/text_metrics.py [--worst N] [--atlas NAME]
"""
import argparse
import glob
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ATLAS_DIR = os.path.join(REPO, "core", "ui", "atlas")

GLYPH_RE = re.compile(
    r"\[\s*(\d+)\] = \{ \.data_offset =\s*(\d+), \.w =\s*(\d+), \.h =\s*(\d+), "
    r"\.offset_x =\s*(-?\d+), \.offset_y =\s*(-?\d+), \.advance =\s*(\d+) \}")
KERN_RE = re.compile(r"\{\s*(\d+),\s*(\d+),\s*(-?\d+) \},")
TRACK_RE = re.compile(r"\.tracking\s*=\s*(-?\d+)")


def load_atlas(path):
    src = open(path, encoding="utf-8").read()
    glyphs = {}
    for m in GLYPH_RE.finditer(src):
        glyphs[int(m.group(1))] = dict(
            off=int(m.group(2)), w=int(m.group(3)), h=int(m.group(4)),
            ox=int(m.group(5)), oy=int(m.group(6)), adv=int(m.group(7)))
    # The DATA array: every 0xNN byte between "_DATA[" and the closing "};".
    ds = src.index("_DATA[")
    de = src.index("};", ds)
    data = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", src[ds:de])]
    kern = {(int(a), int(b)): int(v) for a, b, v in KERN_RE.findall(src)}
    tm = TRACK_RE.search(src)
    tracking = int(tm.group(1)) if tm else 0
    return glyphs, data, kern, tracking


def row_extents(g, data, thresh):
    """Per-row (first, last) ink columns, indexed by row from the glyph top.

    Rows are keyed by BASELINE-relative y so two glyphs of different heights
    can be compared row against row. offset_y is "px below the ascender", so
    baseline-relative row = offset_y + row - ascent; we only need a consistent
    key, so offset_y + row serves.
    """
    out = {}
    for row in range(g["h"]):
        base = g["off"] + row * g["w"]
        first = last = None
        for col in range(g["w"]):
            if data[base + col] >= thresh:
                if first is None:
                    first = col
                last = col
        if first is not None:
            out[g["oy"] + row] = (first, last)
    return out


def gap(ga, gb, data, kern, tracking, ia, ib, thresh):
    """Visual daylight: the MINIMUM horizontal clearance over the rows where
    both glyphs actually have ink.

    Comparing each glyph's extreme column over all rows (the obvious version,
    and the one this tool shipped with first) is wrong: 'y' reaches furthest
    right at its top arm while 'z' reaches furthest left at its baseline, so
    that pair scores as wide open when the shapes never approach each other at
    the same height. Only rows where both have ink can collide.
    """
    ra, rb = row_extents(ga, data, thresh), row_extents(gb, data, thresh)
    if not ra or not rb:
        return None
    xa = 0 + ga["ox"]
    pen = 0
    if ia != 0 and ib != 0:            # glyph 0 is space: tracking exempt
        pen += tracking
    pen += kern.get((ia, ib), 0) * 2
    pen += ga["adv"]
    xb = ((pen + 32) >> 6) + gb["ox"]
    shared = set(ra) & set(rb)
    if not shared:
        return None                    # never adjacent at any height
    return min((xb + rb[y][0]) - (xa + ra[y][1]) - 1 for y in shared)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--worst", type=int, default=10)
    ap.add_argument("--atlas", default=None)
    ap.add_argument("--target", type=float, default=None,
                    help="solve for the tracking (px) that puts each atlas's "
                         "soft mean gap at this many pixels, and print a "
                         "TRACKING_PX table")
    args = ap.parse_args()

    files = sorted(glob.glob(os.path.join(ATLAS_DIR, "nunito_*.h")))
    if args.atlas:
        files = [f for f in files if args.atlas in os.path.basename(f)]
    letters = ([chr(c) for c in range(0x41, 0x5B)] +
               [chr(c) for c in range(0x61, 0x7B)])

    print(f"{'atlas':<22}{'track':>6}{'core mean':>11}{'core<=0':>9}"
          f"{'soft mean':>11}{'p10':>6}{'p90':>6}")
    print("-" * 71)
    report = {}
    for path in files:
        glyphs, data, kern, tracking = load_atlas(path)
        name = os.path.basename(path).replace("nunito_", "").replace(".h", "")
        core, soft = [], []
        for a in letters:
            for b in letters:
                ia, ib = ord(a) - 0x20, ord(b) - 0x20
                if ia not in glyphs or ib not in glyphs:
                    continue
                gc = gap(glyphs[ia], glyphs[ib], data, kern, tracking,
                         ia, ib, 128)
                gs = gap(glyphs[ia], glyphs[ib], data, kern, tracking,
                         ia, ib, 1)
                if gc is not None:
                    core.append((gc, a + b))
                if gs is not None:
                    soft.append((gs, a + b))
        core.sort()
        soft.sort()
        cm = sum(g for g, _ in core) / len(core)
        sm = sum(g for g, _ in soft) / len(soft)
        touch = sum(1 for g, _ in core if g <= 0) * 100.0 / len(core)
        p10 = soft[len(soft) // 10][0]
        p90 = soft[len(soft) * 9 // 10][0]
        print(f"{name:<22}{tracking/64:>6.2f}{cm:>11.2f}{touch:>8.1f}%"
              f"{sm:>11.2f}{p10:>6d}{p90:>6d}")
        report[name] = (core, soft)

    if args.target is not None:
        # Solve per atlas. Tracking shifts every gap by (nearly) itself, but
        # the pen rounds to whole pixels, so the relationship is a staircase —
        # sweep rather than divide, and take the smallest tracking that
        # reaches the target so we never over-space.
        print(f"\n--- tracking to put the soft mean gap at {args.target:.2f}px ---")
        print("TRACKING_PX = {")
        for path in files:
            glyphs, data, kern, _ = load_atlas(path)
            name = os.path.basename(path).replace("nunito_", "").replace(".h", "")
            bold = "bold" in name
            px = int(name.split("_")[-1])
            best, best_err = None, None
            for t64 in range(0, 129):
                gaps = []
                for a in letters:
                    for b in letters:
                        ia, ib = ord(a) - 0x20, ord(b) - 0x20
                        if ia not in glyphs or ib not in glyphs:
                            continue
                        g = gap(glyphs[ia], glyphs[ib], data, kern, t64,
                                ia, ib, 1)
                        if g is not None:
                            gaps.append(g)
                m = sum(gaps) / len(gaps)
                err = abs(m - args.target)
                if best_err is None or err < best_err:
                    best, best_err = t64, err
            print(f"    ({bold}, {px:2d}): {best/64:.2f},"
                  f"   # {name}: {best}/64, soft mean err {best_err:+.2f}px")
        print("}")
        return 0

    n = args.worst
    for name, (core, soft) in report.items():
        tight = [p for g, p in core if g <= 0][:n]
        airy = [f"{p}({g})" for g, p in reversed(core[-n:])]
        print(f"\n{name}")
        print(f"  tightest (cores touching): {' '.join(tight) if tight else '-'}")
        print(f"  airiest:                   {' '.join(airy)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
